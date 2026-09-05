cmake_minimum_required(VERSION 3.16)
set(ESP_IRIS_BUILD_PROFILE disabled CACHE STRING "" FORCE)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp_iris_disabled_test)
