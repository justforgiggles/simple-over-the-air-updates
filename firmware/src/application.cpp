#include "esp_log.h"
#include "esp_timer.h"
#include "application.h"

// Put your application here. Each call must return within the watchdog period.
// Keep Wi-Fi and OTA supervision in main.cpp functional in every release.
void applicationSetup() {
    ESP_LOGI("app", "Application started");
}

void applicationLoop() {
    static int64_t lastMessage = 0;
    const int64_t now = esp_timer_get_time();
    if (now - lastMessage >= 5 * 1000 * 1000) {
        lastMessage = now;
        ESP_LOGI("app", "Application running");
    }
}
