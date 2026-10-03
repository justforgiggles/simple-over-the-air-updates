idf_component_register(
    SRCS "main.cpp" "application.cpp" "../../bootstrap.cpp"
    INCLUDE_DIRS "../include"
    REQUIRES esp_wifi nvs_flash esp_http_client app_update esp_timer
             esp_event esp_netif mbedtls bootloader_support
)
