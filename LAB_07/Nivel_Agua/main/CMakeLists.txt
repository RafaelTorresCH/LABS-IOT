idf_component_register(SRCS "water_level_pump.c" "esp_ot_cli.c" "valve_demo.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_event esp_netif esp_timer nvs_flash openthread vfs coap driver)
