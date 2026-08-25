cmake_minimum_required(VERSION 3.16)

list(APPEND EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../components/esp_iris"
)

set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.local.defaults")
    list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.local.defaults")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp_iris_tcp_pairing)
