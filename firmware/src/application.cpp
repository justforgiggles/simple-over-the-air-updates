#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "application.h"
#if __has_include("config.h")
#include "config.h"
#else
#include "config.example.h"
#endif

static_assert(LED_GPIO == -1 || (GPIO_IS_VALID_OUTPUT_GPIO(LED_GPIO) &&
              !(LED_GPIO >= 6 && LED_GPIO <= 11)), "LED must use an output GPIO outside the flash pins");

namespace {
bool ledOn = false;
int64_t lastToggle = 0;

void setLed(bool on) {
    ESP_ERROR_CHECK(gpio_set_level(static_cast<gpio_num_t>(LED_GPIO), on == LED_ACTIVE_HIGH));
}
}

// Put your application here. Each call must return within the watchdog period.
// Keep Wi-Fi and OTA supervision in main.cpp functional in every release.
void applicationSetup() {
    if (LED_GPIO < 0) {
        ESP_LOGI("app", "LED disabled");
        return;
    }
    ESP_ERROR_CHECK(gpio_reset_pin(static_cast<gpio_num_t>(LED_GPIO)));
    setLed(false);
    ESP_ERROR_CHECK(gpio_set_direction(static_cast<gpio_num_t>(LED_GPIO), GPIO_MODE_OUTPUT));
    lastToggle = esp_timer_get_time();
    ESP_LOGI("app", "Flashing LED on GPIO%d", LED_GPIO);
}

void applicationLoop() {
    if (LED_GPIO < 0) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (now - lastToggle >= 500 * 1000) {
        lastToggle = now;
        ledOn = !ledOn;
        setLed(ledOn);
    }
}
