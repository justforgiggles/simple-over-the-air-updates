#pragma once

// Copy to config.h. These values are compiled into every firmware binary.
constexpr char WIFI_SSID[] = "your-wifi-name";
constexpr char WIFI_PASSWORD[] = "your-wifi-password";
constexpr char BUNDLE_URL[] = "https://raw.githubusercontent.com/justforgiggles/simple-over-the-air-updates/main/bundle";
constexpr unsigned long UPDATE_INTERVAL_MS = 60UL * 60 * 1000;
constexpr unsigned long NETWORK_TIMEOUT_MS = 10 * 1000;
constexpr unsigned long DOWNLOAD_TIMEOUT_MS = 120 * 1000;
constexpr unsigned long APP_WATCHDOG_SECONDS = 30;
// Board-dependent user LED: GPIO2 is a common default, not confirmed by the photo.
// Set LED_GPIO to -1 if the board only has a power LED or uses this pin elsewhere.
constexpr int LED_GPIO = 2;
constexpr bool LED_ACTIVE_HIGH = true;
