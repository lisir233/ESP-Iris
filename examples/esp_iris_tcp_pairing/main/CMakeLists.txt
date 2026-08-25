idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_event esp_iris esp_netif esp_rom esp_wifi freertos nvs_flash
)
