if(CONFIG_ESP_IRIS_ENABLE)
    set(iris_fixture_sources
        "main.c"
        "../../../examples/media_streams/main/media_assets.c")
else()
    set(iris_fixture_sources "disabled.c")
endif()

# Keep dependency names literal and unconditional: ESP-IDF performs its first
# component dependency scan before sdkconfig values are available.
idf_component_register(
    SRCS ${iris_fixture_sources}
    INCLUDE_DIRS "." "../../../examples/media_streams/main"
    REQUIRES esp_iris fatfs joltwallet__littlefs wear_levelling)
