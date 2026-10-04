// Customer installer: paste this whole file into a new Arduino IDE sketch.
// Board package: esp32 by Espressif Systems 3.1.3; board: ESP32 Dev Module (ESP-32U).
// Flash: 4 MB, DIO, 40 MHz; Partition Scheme: Minimal SPIFFS (1.9MB APP with OTA).
// Edit credentials below to match those compiled into the published firmware.
// Upload by USB. This installer is replaced by the first verified application.

#include <cstddef>
#include <cstring>

#ifndef SOTAU_HOST_TEST
#ifdef ARDUINO
#include <Arduino.h>
#include <WiFi.h>
#include "esp_wifi.h"
// Print failures even when Arduino Core Debug Level is set to None.
#define OTA_ERROR(format, ...) Serial.printf("OTA: " format "\n", ##__VA_ARGS__)
constexpr char WIFI_SSID[] = "WR7010-2.4G-82E";
constexpr char WIFI_PASSWORD[] = "12345678";
constexpr char BUNDLE_URL[] = "https://raw.githubusercontent.com/justforgiggles/simple-over-the-air-updates/main/bundle";
// Retry delay only: the first installation attempt runs immediately in loop().
constexpr unsigned long UPDATE_INTERVAL_MS = 60 * 1000;
constexpr unsigned long NETWORK_TIMEOUT_MS = 10 * 1000;
constexpr unsigned long CLOCK_SYNC_TIMEOUT_MS = 30 * 1000;
constexpr unsigned long DOWNLOAD_TIMEOUT_MS = 120 * 1000;
constexpr unsigned long APP_WATCHDOG_SECONDS = 30;
#else
#if __has_include("config.h")
#include "config.h"
#else
#include "config.example.h"
#endif
#include "esp_crt_bundle.h"
#define OTA_ERROR(format, ...) ESP_LOGE("ota", format, ##__VA_ARGS__)
#endif

#include <algorithm>
#include <ctime>
#include <string>
#include "esp_http_client.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

#if !CONFIG_IDF_TARGET_ESP32
#error "Select ESP32 Dev Module: this bundle is for the original ESP32/ESP-32U, not ESP32-C3/S3."
#endif

static_assert(NETWORK_TIMEOUT_MS > 0 && NETWORK_TIMEOUT_MS < APP_WATCHDOG_SECONDS * 1000,
              "Network timeout must be positive and shorter than the watchdog timeout");
static_assert(DOWNLOAD_TIMEOUT_MS > 0 && UPDATE_INTERVAL_MS > 0, "Timeouts must be positive");

#ifdef ARDUINO
// ISRG Root X1 + X2, from ESP-IDF's Mozilla CA bundle. GitHub's current
// raw-content certificate chain was verified against these roots. If GitHub
// changes certificate authorities, refresh these roots before provisioning.
constexpr char GITHUB_ROOTS[] = R"PEM(-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAwTzELMAkGA1UE
BhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2VhcmNoIEdyb3VwMRUwEwYDVQQD
EwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQG
EwJVUzEpMCcGA1UEChMgSW50ZXJuZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMT
DElTUkcgUm9vdCBYMTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54r
Vygch77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+0TM8ukj1
3Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6UA5/TR5d8mUgjU+g4rk8K
b4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sWT8KOEUt+zwvo/7V3LvSye0rgTBIlDHCN
Aymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyHB5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ
4Q7e2RCOFvu396j3x+UCB5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf
1b0SHzUvKBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWnOlFu
hjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTnjh8BCNAw1FtxNrQH
usEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbwqHyGO0aoSCqI3Haadr8faqU9GY/r
OPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CIrU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4G
A1UdDwEB/wQEAwIBBjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY
9umbbjANBgkqhkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ3BebYhtF8GaV
0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KKNFtY2PwByVS5uCbMiogziUwt
hDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJw
TdwJx4nLCgdNbOhdjsnvzqvHu7UrTkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nx
e5AW0wdeRlN8NwdCjNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZA
JzVcoyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq4RgqsahD
YVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPAmRGunUHBcnWEvgJBQl9n
JEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57demyPxgcYxn/eR44/KJ4EBs+lVDR3veyJ
m+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIICGzCCAaGgAwIBAgIQQdKd0XLq7qeAwSxs6S+HUjAKBggqhkjOPQQDAzBPMQswCQYDVQQGEwJV
UzEpMCcGA1UEChMgSW50ZXJuZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElT
UkcgUm9vdCBYMjAeFw0yMDA5MDQwMDAwMDBaFw00MDA5MTcxNjAwMDBaME8xCzAJBgNVBAYTAlVT
MSkwJwYDVQQKEyBJbnRlcm5ldCBTZWN1cml0eSBSZXNlYXJjaCBHcm91cDEVMBMGA1UEAxMMSVNS
RyBSb290IFgyMHYwEAYHKoZIzj0CAQYFK4EEACIDYgAEzZvVn4CDCuwJSvMWSj5cz3es3mcFDR0H
ttwW+1qLFNvicWDEukWVEYmO6gbf9yoWHKS5xcUy4APgHoIYOIvXRdgKam7mAHf7AlF9ItgKbppb
d9/w+kHsOdx1ymgHDB/qo0IwQDAOBgNVHQ8BAf8EBAMCAQYwDwYDVR0TAQH/BAUwAwEB/zAdBgNV
HQ4EFgQUfEKWrt5LSDv6kviejM9ti6lyN5UwCgYIKoZIzj0EAwMDaAAwZQIwe3lORlCEwkSHRhtF
cP9Ymd70/aTSVaYgLXTWNLxBo1BfASdWtL4ndQavEi51mI38AjEAi/V3bNTIZargCyzuFJ0nN6T5
U6VR5CmD1/iQMVtCnwr1/q4AaOeMSQ+2b1tbFfLn
-----END CERTIFICATE-----
)PEM";
#endif
#endif // SOTAU_HOST_TEST

namespace ota {
// The committed checksum file is exactly 64 lowercase hex digits and a newline.
// Also compiled by the small host test; no duplicate parser in the test suite.
bool parseChecksum(const char* body, size_t size, char output[65]) {
    if (size != 65 || body[64] != '\n') {
        return false;
    }
    for (size_t i = 0; i < 64; ++i) {
        if (!((body[i] >= '0' && body[i] <= '9') || (body[i] >= 'a' && body[i] <= 'f'))) {
            return false;
        }
    }
    memcpy(output, body, 64);
    output[64] = '\0';
    return true;
}

#ifndef SOTAU_HOST_TEST
uint64_t milliseconds() {
    return esp_timer_get_time() / 1000;
}

bool layoutCompatible() {
    const auto* first = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
    const auto* second = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
    const auto* nvs = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, nullptr);
    const auto* data = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
    return first && second && nvs && data && first->address == 0x10000 &&
           second->address == 0x1f0000 && first->size == 0x1e0000 && second->size == 0x1e0000 &&
           nvs->address == 0x9000 && nvs->size == 0x5000 && data->address == 0xe000 && data->size == 0x2000;
}

void formatChecksum(const uint8_t digest[32], char output[65]) {
    constexpr char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; ++i) {
        output[i * 2] = hex[digest[i] >> 4];
        output[1 + i * 2] = hex[digest[i] & 15];
    }
    output[64] = '\0';
}

// Hash precisely the unsigned application .bin, including its appended digest.
// esp_partition_get_sha256() alone omits that appended digest and is not the
// SHA-256 of the published file. No stored version is necessary.
bool imageChecksum(const esp_partition_t* partition, char output[65], uint32_t* imageSize) {
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
        formatChecksum(digest, output);
        if (imageSize) {
            *imageSize = metadata.image_len;
        }
    }
    return ok;
}

// A fresh connection on every request lets normal DNS TTLs take effect.
struct Request {
    esp_http_client_handle_t client = nullptr;
    int64_t length = -1;

    ~Request() {
        if (client) {
            esp_http_client_cleanup(client);
        }
    }

    bool open(const char* filename) {
        if (strncmp(BUNDLE_URL, "https://", 8) != 0) {
            OTA_ERROR("Bundle URL must use HTTPS");
            return false;
        }
        const std::string url = std::string(BUNDLE_URL) + "/" + filename;
        esp_http_client_config_t config = {};
        config.url = url.c_str();
        config.timeout_ms = NETWORK_TIMEOUT_MS;
#ifdef ARDUINO
        config.cert_pem = GITHUB_ROOTS;
#else
        config.crt_bundle_attach = esp_crt_bundle_attach;
#endif
        config.disable_auto_redirect = true;
        client = esp_http_client_init(&config);
        if (!client) {
            OTA_ERROR("Cannot allocate HTTP client for %s", filename);
            return false;
        }
        esp_http_client_set_header(client, "Connection", "close");
        esp_http_client_set_header(client, "Accept-Encoding", "identity");
        ESP_ERROR_CHECK(esp_task_wdt_reset());
        const esp_err_t error = esp_http_client_open(client, 0);
        if (error != ESP_OK) {
            OTA_ERROR("GET %s failed: %s (DNS, connection or TLS)", url.c_str(), esp_err_to_name(error));
            return false;
        }
        ESP_ERROR_CHECK(esp_task_wdt_reset());
        length = esp_http_client_fetch_headers(client);
        const int status = esp_http_client_get_status_code(client);
        const bool chunked = esp_http_client_is_chunked_response(client);
        if (length < 0 || status != 200 || chunked) {
            OTA_ERROR("GET %s: HTTP %d, length %lld, chunked %d; require HTTP 200 and Content-Length",
                      url.c_str(), status, static_cast<long long>(length), chunked);
            return false;
        }
        return true;
    }
};

// A read timeout means no data is ready yet, not that the image is corrupt.
// Keep the connection and offset; only retry EAGAIN, within the caller's budget.
int readBody(esp_http_client_handle_t client, char* buffer, int size, uint64_t deadline) {
    while (true) {
        const auto now = milliseconds();
        if (now >= deadline) return -ESP_ERR_HTTP_EAGAIN;
        const auto remaining = deadline - now;
        ESP_ERROR_CHECK(esp_task_wdt_reset());
        esp_http_client_set_timeout_ms(client, std::min<uint64_t>(1000, remaining));
        const int count = esp_http_client_read(client, buffer, size);
        if (count != -ESP_ERR_HTTP_EAGAIN) return count;
        vTaskDelay(1);
    }
}

bool fetchChecksum(char output[65]) {
    Request request;
    if (!request.open("firmware.sha256")) {
        return false;
    }
    if (request.length != 65) {
        OTA_ERROR("Checksum length is %lld; expected 65", static_cast<long long>(request.length));
        return false;
    }
    char body[65];
    size_t received = 0;
    const auto start = milliseconds();
    while (received < sizeof(body)) {
        const auto elapsed = milliseconds() - start;
        if (elapsed >= NETWORK_TIMEOUT_MS) {
            OTA_ERROR("Checksum download timed out");
            return false;
        }
        int count = readBody(request.client, body + received, sizeof(body) - received,
                             start + NETWORK_TIMEOUT_MS);
        if (count <= 0) {
            OTA_ERROR("Checksum read failed (%d), received %u/65 bytes", count, static_cast<unsigned>(received));
            return false;
        }
        received += count;
    }
    if (!esp_http_client_is_complete_data_received(request.client) ||
        !parseChecksum(body, sizeof(body), output)) {
        OTA_ERROR("Invalid checksum body; expected 64 lowercase hex digits and a newline");
        return false;
    }
    return true;
}

bool install(const char* expected) {
    const auto start = milliseconds();
    Request request;
    if (!layoutCompatible()) {
        OTA_ERROR("Incompatible OTA partition layout");
        return false;
    }
    if (!request.open("firmware.bin")) {
        return false;
    }
    const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);
    if (!target || request.length <= 0 || request.length > target->size) {
        OTA_ERROR("Firmware length %lld does not fit OTA slot (%lu bytes)",
                  static_cast<long long>(request.length), static_cast<unsigned long>(target ? target->size : 0));
        return false;
    }
    esp_ota_handle_t update;
    ESP_ERROR_CHECK(esp_task_wdt_reset());
    esp_err_t error = esp_ota_begin(target, request.length, &update);
    if (error != ESP_OK) {
        OTA_ERROR("Starting flash update failed: %s", esp_err_to_name(error));
        return false;
    }
    int64_t received = 0;
    char buffer[1024];
    while (received < request.length) {
        const auto elapsed = milliseconds() - start;
        if (elapsed >= DOWNLOAD_TIMEOUT_MS) {
            break;
        }
        int count = readBody(request.client, buffer,
            std::min<int64_t>(sizeof(buffer), request.length - received), start + DOWNLOAD_TIMEOUT_MS);
        if (count <= 0) {
            OTA_ERROR("Firmware read failed (%d) after %lld/%lld bytes", count,
                      static_cast<long long>(received), static_cast<long long>(request.length));
            esp_ota_abort(update);
            return false;
        }
        error = esp_ota_write(update, buffer, count);
        if (error != ESP_OK) {
            OTA_ERROR("Flash write failed: %s", esp_err_to_name(error));
            esp_ota_abort(update);
            return false;
        }
        received += count;
        vTaskDelay(1);
    }
    if (received != request.length || milliseconds() - start >= DOWNLOAD_TIMEOUT_MS ||
        !esp_http_client_is_complete_data_received(request.client)) {
        OTA_ERROR("Firmware download incomplete or timed out: %lld/%lld bytes",
                  static_cast<long long>(received), static_cast<long long>(request.length));
        esp_ota_abort(update);
        return false;
    }
    // Validates chip/image structure and frees the handle, including on failure.
    error = esp_ota_end(update);
    if (error != ESP_OK) {
        OTA_ERROR("Firmware image validation failed: %s", esp_err_to_name(error));
        return false;
    }
    char actual[65];
    uint32_t imageSize = 0;
    if (!imageChecksum(target, actual, &imageSize) || imageSize != request.length ||
        strcmp(actual, expected) != 0) {
        OTA_ERROR("Firmware verification failed: image size or SHA-256 differs from published bundle");
        return false;
    }
    error = esp_ota_set_boot_partition(target);
    if (error != ESP_OK) {
        OTA_ERROR("Selecting new boot partition failed: %s", esp_err_to_name(error));
        return false;
    }
    ESP_LOGI("ota", "Verified %s; rebooting", actual);
    esp_restart();
    return true;
}
#endif // SOTAU_HOST_TEST
} // namespace ota

#ifdef ARDUINO
void setup() {
    Serial.begin(115200);
    esp_task_wdt_config_t watchdog = {};
    watchdog.timeout_ms = APP_WATCHDOG_SECONDS * 1000;
    watchdog.idle_core_mask = (1U << portNUM_PROCESSORS) - 1;
    watchdog.trigger_panic = true;
    ESP_ERROR_CHECK(esp_task_wdt_reconfigure(&watchdog));
    ESP_ERROR_CHECK(esp_task_wdt_add(nullptr));
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    // loop() owns connection attempts; do not race an automatic reconnect.
    WiFi.setAutoReconnect(false);
}

void loop() {
    ESP_ERROR_CHECK(esp_task_wdt_reset());
    if (!ota::layoutCompatible()) {
        Serial.println("Select Minimal SPIFFS (1.9MB APP with OTA), then upload again.");
    } else {
        if (WiFi.status() != WL_CONNECTED) {
            // Arduino disconnect() does not cancel an association still in progress.
            esp_wifi_disconnect();
            WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        }
        const auto start = ota::milliseconds();
        while (WiFi.status() != WL_CONNECTED &&
               ota::milliseconds() - start < NETWORK_TIMEOUT_MS) {
            ESP_ERROR_CHECK(esp_task_wdt_reset());
            delay(100);
        }
        if (WiFi.status() == WL_CONNECTED && time(nullptr) < 1704067200) {
            Serial.printf("Wi-Fi connected: IP %s; synchronizing clock...\n", WiFi.localIP().toString().c_str());
            // Start/restart NTP only once DNS and the network are available.
            configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
            const auto clockStart = ota::milliseconds();
            while (WiFi.status() == WL_CONNECTED && time(nullptr) < 1704067200 &&
                   ota::milliseconds() - clockStart < CLOCK_SYNC_TIMEOUT_MS) {
                ESP_ERROR_CHECK(esp_task_wdt_reset());
                delay(100);
            }
        }
        char checksum[65];
        if (WiFi.status() != WL_CONNECTED) {
            Serial.printf("Wi-Fi connection failed: status %d; check credentials, 2.4 GHz signal and antenna.\n",
                          static_cast<int>(WiFi.status()));
        } else if (time(nullptr) < 1704067200) {
            Serial.println("Clock sync pending: NTP (UDP 123) must be reachable for HTTPS verification.");
        } else if (ota::fetchChecksum(checksum)) {
            Serial.println("Installing firmware...");
            ota::install(checksum);
        }
        Serial.println("Installation incomplete; retrying in 60 seconds.");
    }
    const auto start = ota::milliseconds();
    while (ota::milliseconds() - start < UPDATE_INTERVAL_MS) {
        ESP_ERROR_CHECK(esp_task_wdt_reset());
        delay(100);
    }
}
#endif
