cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
endif()
set(ESP_IRIS_LOCAL_DEFAULTS
    "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.local.defaults"
)
if(EXISTS "${ESP_IRIS_LOCAL_DEFAULTS}")
    list(APPEND SDKCONFIG_DEFAULTS "${ESP_IRIS_LOCAL_DEFAULTS}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp_iris_tcp_wifi)
