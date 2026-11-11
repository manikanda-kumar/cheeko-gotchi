# Host project that builds one SDK app against cheeko_runtime.
#   idf.py build                                  -> examples/hello_display
#   idf.py -DCHEEKO_APP=../../examples/self_test build   (path relative to this dir, or absolute)
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../cheeko_runtime"
    "${CMAKE_CURRENT_LIST_DIR}/../hardware_test/components/esp_lcd_jd9853")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(cheeko_sdk_app)
