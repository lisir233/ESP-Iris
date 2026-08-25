idf_component_register(
    SRCS "main.c" "iris_example.c"
    INCLUDE_DIRS "."
    REQUIRES esp_iris esp_system freertos
)
