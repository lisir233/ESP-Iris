if(CONFIG_ESP_IRIS_ENABLE)
    set(ESP_IRIS_SRCS
        "src/esp_iris.c"
        "src/esp_iris_codec.c"
        "src/esp_iris_crash.c"
        "src/esp_iris_identity.c"
        "src/esp_iris_log.c"
        "src/esp_iris_files.c"
        "src/esp_iris_platform.c"
        "src/esp_iris_services.c"
        "src/esp_iris_state.c"
    )
    if(CONFIG_ESP_IRIS_TRANSPORT_USB)
        list(APPEND ESP_IRIS_SRCS "src/esp_iris_transport_usb.c")
    elseif(CONFIG_ESP_IRIS_TRANSPORT_USB_SERIAL_JTAG)
        list(APPEND ESP_IRIS_SRCS "src/esp_iris_transport_usb_serial_jtag.c")
    else()
        list(APPEND ESP_IRIS_SRCS "src/esp_iris_transport_tcp.c")
    endif()
else()
    set(ESP_IRIS_SRCS
        "src/esp_iris_disabled.c"
        "src/esp_iris_platform.c"
    )
endif()

set(ESP_IRIS_PRIV_REQUIRES
    esp_driver_usb_serial_jtag
    esp_partition
    espcoredump
    mbedtls
)
if(CONFIG_ESP_IRIS_TRANSPORT_USB)
    list(APPEND ESP_IRIS_PRIV_REQUIRES esp_tinyusb)
elseif(NOT CONFIG_ESP_IRIS_TRANSPORT_USB_SERIAL_JTAG)
    list(APPEND ESP_IRIS_PRIV_REQUIRES esp_netif lwip)
endif()

idf_component_register(
    SRCS ${ESP_IRIS_SRCS}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES app_update esp_app_format esp_system esp_timer nvs_flash vfs
    PRIV_REQUIRES ${ESP_IRIS_PRIV_REQUIRES}
)
