# Requirements are expanded before Kconfig. Use a pre-configuration profile,
# never CONFIG_* conditions, to make dependency pruning deterministic.
set(ESP_IRIS_BUILD_PROFILE "full" CACHE STRING "Iris dependency profile: full, usb, usj, tcp, disabled")
set_property(CACHE ESP_IRIS_BUILD_PROFILE PROPERTY STRINGS full usb usj tcp disabled)
if(NOT ESP_IRIS_BUILD_PROFILE MATCHES "^(full|usb|usj|tcp|disabled)$")
    message(FATAL_ERROR "Unknown ESP_IRIS_BUILD_PROFILE=${ESP_IRIS_BUILD_PROFILE}")
endif()

if(CONFIG_ESP_IRIS_ENABLE)
    if(ESP_IRIS_BUILD_PROFILE STREQUAL "disabled")
        message(FATAL_ERROR "disabled Iris profile requires CONFIG_ESP_IRIS_ENABLE=n")
    endif()
    if(NOT ESP_IRIS_BUILD_PROFILE STREQUAL "full")
        if((CONFIG_ESP_IRIS_TRANSPORT_USB AND NOT ESP_IRIS_BUILD_PROFILE STREQUAL "usb") OR
           (CONFIG_ESP_IRIS_TRANSPORT_USB_SERIAL_JTAG AND NOT ESP_IRIS_BUILD_PROFILE STREQUAL "usj") OR
           (CONFIG_ESP_IRIS_TRANSPORT_TCP AND NOT ESP_IRIS_BUILD_PROFILE STREQUAL "tcp"))
            message(FATAL_ERROR "Iris transport Kconfig does not match dependency profile")
        endif()
    endif()
    set(ESP_IRIS_SRCS
        "src/esp_iris.c"
        "src/esp_iris_codec.c"
        "src/esp_iris_crash.c"
        "src/esp_iris_crash_recovery.c"
        "src/esp_iris_identity.c"
        "src/esp_iris_log.c"
        "src/esp_iris_mdns.c"
        "src/esp_iris_files.c"
        "src/esp_iris_platform.c"
        "src/esp_iris_services.c"
        "src/esp_iris_state.c"
        "src/esp_iris_system_inventory.c"
        "src/esp_iris_system_update.c"
        "src/esp_iris_transport.c"
    )
    if(CONFIG_ESP_IRIS_TRANSPORT_USB)
        list(APPEND ESP_IRIS_SRCS "src/esp_iris_transport_usb.c")
    endif()
    if(CONFIG_ESP_IRIS_TRANSPORT_USB_SERIAL_JTAG)
        list(APPEND ESP_IRIS_SRCS "src/esp_iris_transport_usb_serial_jtag.c")
    endif()
    if(CONFIG_ESP_IRIS_TRANSPORT_TCP)
        list(APPEND ESP_IRIS_SRCS "src/esp_iris_transport_tcp.c")
    endif()
    if(NOT CONFIG_ESP_IRIS_TRANSPORT_USB AND
       NOT CONFIG_ESP_IRIS_TRANSPORT_USB_SERIAL_JTAG AND
       NOT CONFIG_ESP_IRIS_TRANSPORT_TCP)
        message(FATAL_ERROR "ESP-Iris requires at least one device transport")
    endif()
else()
    set(ESP_IRIS_SRCS
        "src/esp_iris_disabled.c"
        "src/esp_iris_platform.c"
    )
endif()

set(ESP_IRIS_PRIV_REQUIRES)
if(NOT ESP_IRIS_BUILD_PROFILE STREQUAL "disabled")
    list(APPEND ESP_IRIS_PRIV_REQUIRES
        app_update esp_app_format esp_system esp_timer nvs_flash vfs
        esp_partition espcoredump mbedtls)
    if(ESP_IRIS_BUILD_PROFILE MATCHES "^(full|usj)$")
        list(APPEND ESP_IRIS_PRIV_REQUIRES esp_driver_usb_serial_jtag)
    endif()
    if(ESP_IRIS_BUILD_PROFILE MATCHES "^(full|tcp)$")
        list(APPEND ESP_IRIS_PRIV_REQUIRES esp_netif lwip mdns)
    endif()
    if(ESP_IRIS_BUILD_PROFILE MATCHES "^(full|usb)$" AND IDF_TARGET STREQUAL "esp32s31")
        list(APPEND ESP_IRIS_PRIV_REQUIRES esp_tinyusb)
    endif()
endif()
idf_component_register(
    SRCS ${ESP_IRIS_SRCS}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES esp_common
    PRIV_REQUIRES ${ESP_IRIS_PRIV_REQUIRES}
)
