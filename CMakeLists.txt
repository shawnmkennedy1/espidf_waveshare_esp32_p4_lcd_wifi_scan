# ==============================================================================
# PROJECT: lvgl_demo_v9
# FILE: CMakeLists.txt
#
# REVISION HISTORY:
# [2026-06-07] - Added IDF_TARGET restriction for ESP32-P4 architecture alignment.
# [2026-06-08] - Fixed chip revision range for ESP32-P4 v1.3 (eco2 sample silicon)
#              - Embedded --force into esptool args for ECO2 revision check bypass
# ==============================================================================

cmake_minimum_required(VERSION 3.5)

# ------------------------------------------------------------------------------
# Hardware Architecture Locking
# ------------------------------------------------------------------------------
set(IDF_TARGET "esp32p4")

# ------------------------------------------------------------------------------
# Force flag for ECO2 engineering sample silicon (chip rev v1.3)
# esptool rejects bootloaders targeting rev ranges that don't match chip rev
# without --force. This permanently embeds the flag so plain "idf.py flash" works.
# ------------------------------------------------------------------------------
set(ESPTOOL_FLASH_ARGS "--force")
set(ESPTOOL_WRITE_FLASH_ARGS "--force")

# Load standard ESP-IDF framework macros
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# ------------------------------------------------------------------------------
# Custom Compiler Flags & Diagnostics Suppression
# ------------------------------------------------------------------------------
add_compile_options(-Wno-format)
add_compile_options(-Wno-ignored-qualifiers)
add_compile_options(-Wno-attributes)
add_compile_options(-Wno-unused-function)

# Clean up generic BSP calls since we initialize the MIPI DSI display driver directly
add_compile_definitions(BSP_CONFIG_NO_GRAPHIC_LIB=1)

project(lvgl_demo_v9)
