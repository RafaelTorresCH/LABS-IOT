idf_component_register(
    SRCS "ftd_battery.c"
    INCLUDE_DIRS "."
    REQUIRES esp_adc esp_app_format esp_event esp_netif esp_timer nvs_flash openthread
)
