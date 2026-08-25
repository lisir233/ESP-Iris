idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES app_update esp_app_format esp_iris esp_partition freertos nvs_flash
)
