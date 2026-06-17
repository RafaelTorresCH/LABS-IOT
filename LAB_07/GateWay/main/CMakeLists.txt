idf_component_register(SRCS "sensor_gateway.c" "sensor_node.c" "esp_ot_cli.c" "coap_demo.c" "valve_demo.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_adc esp_event esp_netif esp_timer esp_wifi mqtt nvs_flash openthread vfs
                                ot_examples_common ot_led
                                espressif__esp_ot_cli_extension coap driver)
