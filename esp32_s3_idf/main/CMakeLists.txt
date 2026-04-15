idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event esp_netif esp_http_client esp_driver_gpio esp_driver_i2s esp_driver_rmt json nvs_flash
)
