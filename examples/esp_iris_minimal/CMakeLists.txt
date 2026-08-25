cmake_minimum_required(VERSION 3.16)

list(APPEND EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../bsp"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp_iris_minimal)
