idf_component_register(SRCS "sensor_node.c" "esp_ot_cli.c" "coap_demo.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_adc esp_event esp_netif esp_timer nvs_flash openthread vfs coap driver)
