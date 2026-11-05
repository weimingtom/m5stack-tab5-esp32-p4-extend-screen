cmake_minimum_required(VERSION 3.16)

# ESP32-P4 side of the "PC screen -> Tab5" stream:
#   python/screen_stream.py captures the desktop, JPEG-encodes it and pushes
#   frames over USB (CDC-ACM) or Wi-Fi (TCP). The firmware keeps the incoming
#   frames in PSRAM and paints them on the Tab5 1280x720 MIPI-DSI panel.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32p4_screen_stream)
