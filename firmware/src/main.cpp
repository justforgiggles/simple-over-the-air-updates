#include <algorithm>
#include <atomic>
#include <cstring>
#include <ctime>

#include "application.h"
#if __has_include("config.h")
#include "config.h"
#else
#include "config.example.h"
#warning "Using placeholder Wi-Fi credentials. Copy config.example.h to config.h before flashing."
#endif

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "nvs_flash.h"

#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE || CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK
#error "This project requires rollback and anti-rollback to be disabled."
#endif

static_assert(sizeof(WIFI_SSID) <= 33, "Wi-Fi SSID is too long");
static_assert(sizeof(WIFI_PASSWORD) <= 65, "Wi-Fi password is too long");
static_assert(UPDATE_INTERVAL_MS > 0 && NETWORK_TIMEOUT_MS > 0, "Timeouts must be positive");
static_assert(NETWORK_TIMEOUT_MS < APP_WATCHDOG_SECONDS * 1000,
              "Network timeout must be shorter than watchdog timeout");

namespace {
constexpr char TAG[] = "ota";
std::atomic<bool> connected{false};
char runningETag[67] = {};
bool recoveryMode = false;

uint64_t milliseconds() {
    return esp_timer_get_time() / 1000;
}

void pauseFor(uint32_t duration) {
    const auto start = milliseconds();
    do {
        ESP_ERROR_CHECK(esp_task_wdt_reset());
        vTaskDelay(pdMS_TO_TICKS(100));
    } while (milliseconds() - start < duration);
}

void formatETag(const uint8_t digest[32], char output[67]) {
    constexpr char hex[] = "0123456789abcdef";
    output[0] = '"';
    for (size_t i = 0; i < 32; ++i) {
        output[1 + i * 2] = hex[digest[i] >> 4];
        output[2 + i * 2] = hex[digest[i] & 15];
    }
    output[65] = '"';
    output[66] = '\0';
}

bool validETag(const char* etag) {
    if (!etag || strlen(etag) != 66 || etag[0] != '"' || etag[65] != '"') {
        return false;
    }
    for (size_t i = 1; i < 65; ++i) {
        if (!((etag[i] >= '0' && etag[i] <= '9') || (etag[i] >= 'a' && etag[i] <= 'f'))) {
            return false;
        }
    }
    return true;
}

// Hash precisely the unsigned application .bin, including its appended digest.
// esp_partition_get_sha256() alone omits that appended digest and is not the
// SHA-256 of the file served by Go. No stored version or ETag is necessary.
bool imageETag(const esp_partition_t* partition, char output[67], uint32_t* imageSize = nullptr) {
    esp_partition_pos_t position = {partition->address, partition->size};
    esp_image_metadata_t metadata = {};
    if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &position, &metadata) != ESP_OK) {
        return false;
    }
    mbedtls_sha256_context hash;
    mbedtls_sha256_init(&hash);
    bool ok = mbedtls_sha256_starts(&hash, 0) == 0;
    uint8_t buffer[1024];
    for (uint32_t offset = 0; ok && offset < metadata.image_len;) {
        size_t count = std::min<size_t>(sizeof(buffer), metadata.image_len - offset);
        ok = esp_partition_read(partition, offset, buffer, count) == ESP_OK &&
             mbedtls_sha256_update(&hash, buffer, count) == 0;
        offset += count;
        ESP_ERROR_CHECK(esp_task_wdt_reset());
        vTaskDelay(1);
    }
    uint8_t digest[32];
    ok = ok && mbedtls_sha256_finish(&hash, digest) == 0;
    mbedtls_sha256_free(&hash);
    if (ok) {
        formatETag(digest, output);
        if (imageSize) {
            *imageSize = metadata.image_len;
        }
    }
    return ok;
}

void loadRecoveryState() {
    nvs_handle_t storage;
    ESP_ERROR_CHECK(nvs_open("sotau", NVS_READWRITE, &storage));
    char failedImage[67] = {};
    size_t size = sizeof(failedImage);
    esp_err_t err = nvs_get_str(storage, "failed_image", failedImage, &size);
    if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_ERROR_CHECK(err);
    }
    const auto reason = esp_reset_reason();
    const bool crashed = reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT ||
                         reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT;
    recoveryMode = crashed || strcmp(failedImage, runningETag) == 0;
    if (recoveryMode && strcmp(failedImage, runningETag) != 0) {
        ESP_ERROR_CHECK(nvs_set_str(storage, "failed_image", runningETag));
        ESP_ERROR_CHECK(nvs_commit(storage));
    } else if (!recoveryMode && failedImage[0]) {
        ESP_ERROR_CHECK(nvs_erase_key(storage, "failed_image"));
        ESP_ERROR_CHECK(nvs_commit(storage));
    }
    nvs_close(storage);
    ESP_LOGI(TAG, "Running %s; mode=%s", runningETag, recoveryMode ? "recovery" : "application");
}

void wifiEvent(void*, esp_event_base_t base, int32_t id, void*) {
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        connected = true;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        connected = false;
    }
}

void startWifi() {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_create_default_wifi_sta() ? ESP_OK : ESP_ERR_NO_MEM);
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifiEvent, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifiEvent, nullptr));
    wifi_config_t wifi = {};
    memcpy(wifi.sta.ssid, WIFI_SSID, sizeof(WIFI_SSID) - 1);
    memcpy(wifi.sta.password, WIFI_PASSWORD, sizeof(WIFI_PASSWORD) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.cloudflare.com");
    esp_sntp_init();
}

bool networkReady() {
    if (!connected) {
        // Cancel a stale association attempt before trying again.
        esp_wifi_disconnect();
        if (esp_wifi_connect() != ESP_OK) {
            return false;
        }
        const auto start = milliseconds();
        while (!connected && milliseconds() - start < NETWORK_TIMEOUT_MS) {
            pauseFor(100);
        }
    }
    if (!connected) {
        ESP_LOGW(TAG, "Wi-Fi unavailable; retrying next cycle");
        return false;
    }
    const auto start = milliseconds();
    while (time(nullptr) < 1704067200 && milliseconds() - start < NETWORK_TIMEOUT_MS) {
        pauseFor(100);
    }
    if (time(nullptr) < 1704067200) {
        ESP_LOGW(TAG, "Waiting for time synchronization before TLS; retrying next cycle");
        return false;
    }
    return true;
}

// Each request owns a fresh connection. DNS stays under the network stack's
// normal TTL cache; never save the resolved address in application state.
struct Request {
    esp_http_client_handle_t client = nullptr;
    char etag[67] = {};
    int64_t length = -1;

    ~Request() {
        if (client) {
            esp_http_client_cleanup(client);
        }
    }

    static esp_err_t event(esp_http_client_event_t* event) {
        auto* request = static_cast<Request*>(event->user_data);
        if (event->event_id == HTTP_EVENT_ON_HEADER && strcasecmp(event->header_key, "ETag") == 0) {
            request->etag[0] = '\0';
            if (validETag(event->header_value)) {
                memcpy(request->etag, event->header_value, sizeof(request->etag));
            }
        }
        return ESP_OK;
    }

    bool open(esp_http_client_method_t method, const char* expected = nullptr) {
        esp_http_client_config_t config = {};
        config.url = UPDATE_URL;
        config.method = method;
        config.timeout_ms = NETWORK_TIMEOUT_MS;
        config.crt_bundle_attach = esp_crt_bundle_attach;
        config.disable_auto_redirect = true;
        config.event_handler = event;
        config.user_data = this;
        client = esp_http_client_init(&config);
        if (!client) {
            return false;
        }
        esp_http_client_set_header(client, "Connection", "close");
        esp_http_client_set_header(client, "Accept-Encoding", "identity");
        if (expected) {
            esp_http_client_set_header(client, "If-Match", expected);
        }
        ESP_ERROR_CHECK(esp_task_wdt_reset());
        if (esp_http_client_open(client, 0) != ESP_OK) {
            return false;
        }
        ESP_ERROR_CHECK(esp_task_wdt_reset());
        length = esp_http_client_fetch_headers(client);
        return length >= 0 && validETag(etag);
    }
};

bool download(const char* expected) {
    const auto start = milliseconds();
    Request request;
    if (!request.open(HTTP_METHOD_GET, expected) ||
        esp_http_client_get_status_code(request.client) != 200 ||
        strcmp(request.etag, expected) != 0 ||
        esp_http_client_is_chunked_response(request.client)) {
        return false;
    }
    const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);
    if (!target || request.length <= 0 || request.length > target->size) {
        return false;
    }
    esp_ota_handle_t update;
    ESP_ERROR_CHECK(esp_task_wdt_reset());
    if (esp_ota_begin(target, request.length, &update) != ESP_OK) {
        return false;
    }
    int64_t received = 0;
    char buffer[1024];
    while (received < request.length) {
        const auto elapsed = milliseconds() - start;
        if (elapsed >= DOWNLOAD_TIMEOUT_MS) {
            break;
        }
        ESP_ERROR_CHECK(esp_task_wdt_reset());
        const auto remaining = DOWNLOAD_TIMEOUT_MS - elapsed;
        esp_http_client_set_timeout_ms(request.client,
            std::min<uint64_t>(NETWORK_TIMEOUT_MS, remaining));
        const int count = esp_http_client_read(request.client, buffer,
            std::min<int64_t>(sizeof(buffer), request.length - received));
        if (count <= 0 || esp_ota_write(update, buffer, count) != ESP_OK) {
            esp_ota_abort(update);
            return false;
        }
        received += count;
        vTaskDelay(1);
    }
    if (received != request.length || milliseconds() - start >= DOWNLOAD_TIMEOUT_MS ||
        !esp_http_client_is_complete_data_received(request.client)) {
        esp_ota_abort(update);
        return false;
    }
    // esp_ota_end validates the chip and image and frees the OTA handle even on failure.
    if (esp_ota_end(update) != ESP_OK) {
        return false;
    }
    char actual[67];
    uint32_t imageSize = 0;
    if (!imageETag(target, actual, &imageSize) || imageSize != request.length ||
        strcmp(actual, expected) != 0) {
        return false;
    }
    if (esp_ota_set_boot_partition(target) != ESP_OK) {
        return false;
    }
    ESP_LOGI(TAG, "Verified %s; rebooting", actual);
    esp_restart();
    return true;
}

void checkForUpdate() {
    char available[67];
    {
        Request request;
        if (!request.open(HTTP_METHOD_OPTIONS) ||
            esp_http_client_get_status_code(request.client) != 204) {
            ESP_LOGW(TAG, "Update check failed; retrying next cycle");
            return;
        }
        memcpy(available, request.etag, sizeof(available));
    }
    if (strcmp(available, runningETag) == 0) {
        ESP_LOGI(TAG, "Firmware unchanged%s", recoveryMode ? "; waiting in recovery" : "");
        return;
    }
    ESP_LOGI(TAG, "Downloading %s", available);
    if (!download(available)) {
        ESP_LOGW(TAG, "Update failed; current firmware retained; retrying next cycle");
    }
}

void updaterTask(void*) {
    ESP_ERROR_CHECK(esp_task_wdt_add(nullptr));
    while (true) {
        if (networkReady()) {
            checkForUpdate();
        }
        pauseFor(UPDATE_INTERVAL_MS);
    }
}
} // namespace

extern "C" void app_main() {
    // Do not silently erase NVS on failure: it holds the recovery latch.
    ESP_ERROR_CHECK(nvs_flash_init());
    esp_task_wdt_config_t watchdog = {};
    watchdog.timeout_ms = APP_WATCHDOG_SECONDS * 1000;
    watchdog.idle_core_mask = 1;
    watchdog.trigger_panic = true;
    ESP_ERROR_CHECK(esp_task_wdt_reconfigure(&watchdog));
    ESP_ERROR_CHECK(esp_task_wdt_add(nullptr));
    ESP_ERROR_CHECK(strncmp(UPDATE_URL, "https://", 8) == 0 ? ESP_OK : ESP_ERR_INVALID_ARG);
    ESP_ERROR_CHECK(imageETag(esp_ota_get_running_partition(), runningETag) ? ESP_OK : ESP_FAIL);
    loadRecoveryState();
    startWifi();
    ESP_ERROR_CHECK(xTaskCreate(updaterTask, "updater", 8192, nullptr, 5, nullptr) == pdPASS
                    ? ESP_OK : ESP_ERR_NO_MEM);
    if (!recoveryMode) {
        applicationSetup();
    }
    while (true) {
        if (!recoveryMode) {
            applicationLoop();
        }
        ESP_ERROR_CHECK(esp_task_wdt_reset());
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
