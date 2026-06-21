# Define the absolute or relative path to the managed LVGL demos directory
set(LV_DEMO_DIR "${CMAKE_CURRENT_LIST_DIR}/../managed_components/lvgl__lvgl/demos")

# Recursively locate all C source implementation files for the demos
file(GLOB_RECURSE LV_DEMOS_SOURCES "${LV_DEMO_DIR}/*.c")

idf_component_register(
    SRCS
        "main.c"
        "lvgl_adapter_init.c"
        "esp_lcd_ek79007.c"
        ${LV_DEMOS_SOURCES}
    INCLUDE_DIRS
        .
        "${LV_DEMO_DIR}"
    REQUIRES
        espressif__esp_lvgl_adapter
        lvgl__lvgl
        esp_lcd
        esp_hw_support
        driver
    PRIV_REQUIRES
        nvs_flash
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    LV_CONF_INCLUDE_SIMPLE=1
    LV_LVGL_H_INCLUDE_SIMPLE=1
    LV_USE_DEMO_BENCHMARK=1
    # Version macros for local EK79007 driver (not from component registry)
    ESP_LCD_EK79007_VER_MAJOR=1
    ESP_LCD_EK79007_VER_MINOR=0
    ESP_LCD_EK79007_VER_PATCH=2
)
