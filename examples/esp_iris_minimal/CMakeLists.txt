cmake_minimum_required(VERSION 3.16)

set(ESP_IRIS_COMPONENT_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../../components/esp_iris"
)
list(APPEND EXTRA_COMPONENT_DIRS
    "${ESP_IRIS_COMPONENT_DIR}"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp_iris_minimal)
