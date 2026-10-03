#pragma once

// Copy to config.h. These values are compiled into every firmware binary.
constexpr char WIFI_SSID[] = "your-wifi-name";
constexpr char WIFI_PASSWORD[] = "your-wifi-password";
constexpr char UPDATE_URL[] = "https://sotau.barenderasmus.com/firmware";
constexpr unsigned long UPDATE_INTERVAL_MS = 60 * 1000;
constexpr unsigned long NETWORK_TIMEOUT_MS = 10 * 1000;
constexpr unsigned long DOWNLOAD_TIMEOUT_MS = 120 * 1000;
constexpr unsigned long APP_WATCHDOG_SECONDS = 30;

