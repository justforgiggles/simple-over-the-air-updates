#include <atomic>
#include <cstring>
#include <ctime>

#include "application.h"
#include "ota.h"
#if __has_include("config.h")
#include "config.h"
#else
#include "config.example.h"
#warning "Using placeholder Wi-Fi credentials. Copy config.example.h to config.h before flashing."
#endif

#include "esp_event.h"
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
char runningChecksum[65] = {};
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
    recoveryMode = crashed || strcmp(failedImage, runningChecksum) == 0;
    if (recoveryMode && strcmp(failedImage, runningChecksum) != 0) {
        ESP_ERROR_CHECK(nvs_set_str(storage, "failed_image", runningChecksum));
        ESP_ERROR_CHECK(nvs_commit(storage));
    } else if (!recoveryMode && failedImage[0]) {
        ESP_ERROR_CHECK(nvs_erase_key(storage, "failed_image"));
        ESP_ERROR_CHECK(nvs_commit(storage));
    }
    nvs_close(storage);
    ESP_LOGI(TAG, "Running %s; mode=%s", runningChecksum, recoveryMode ? "recovery" : "application");
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

void checkForUpdate() {
    char available[65];
    if (!ota::fetchChecksum(available)) {
        ESP_LOGW(TAG, "Checksum check failed; retrying next cycle");
        return;
    }
    if (strcmp(available, runningChecksum) == 0) {
        ESP_LOGI(TAG, "Firmware unchanged%s", recoveryMode ? "; waiting in recovery" : "");
        return;
    }
    ESP_LOGI(TAG, "Downloading %s", available);
    if (!ota::install(available)) {
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
    // Arduino's bootloader monitors the very first handoff from the installer.
    // Confirm it immediately; later IDF updates use UNDEFINED (no rollback).
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_ERROR_CHECK(esp_ota_mark_app_valid_cancel_rollback());
    }
    ESP_ERROR_CHECK(ota::layoutCompatible() ? ESP_OK : ESP_ERR_INVALID_STATE);
    // Do not silently erase NVS on failure: it holds the recovery latch.
    ESP_ERROR_CHECK(nvs_flash_init());
    esp_task_wdt_config_t watchdog = {};
    watchdog.timeout_ms = APP_WATCHDOG_SECONDS * 1000;
    watchdog.idle_core_mask = (1U << portNUM_PROCESSORS) - 1;
    watchdog.trigger_panic = true;
    ESP_ERROR_CHECK(esp_task_wdt_reconfigure(&watchdog));
    ESP_ERROR_CHECK(esp_task_wdt_add(nullptr));
    ESP_ERROR_CHECK(strncmp(BUNDLE_URL, "https://", 8) == 0 ? ESP_OK : ESP_ERR_INVALID_ARG);
    ESP_ERROR_CHECK(ota::imageChecksum(esp_ota_get_running_partition(), runningChecksum, nullptr) ? ESP_OK : ESP_FAIL);
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
