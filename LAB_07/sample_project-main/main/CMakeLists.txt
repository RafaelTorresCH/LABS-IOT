idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi nvs_flash esp_http_server spiffs cjson esp_http_client esp-tls
)

# Generar y flashear el Dashboard automáticamente
spiffs_create_partition_image(spiffs ../data FLASH_IN_PROJECT)
