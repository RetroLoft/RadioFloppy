# RadioFloppy - top-level ESP-IDF project file
cmake_minimum_required(VERSION 3.16)

# Firmware version: shown on the boot screen (esp_app_get_description()).
set(PROJECT_VER "1.0.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(RadioFloppy)
