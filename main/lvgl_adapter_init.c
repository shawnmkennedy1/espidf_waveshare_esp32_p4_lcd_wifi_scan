/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 *
 * Manual LVGL display setup that bypasses esp_lv_adapter_register_display().
 * The adapter's own flush modes never presented on this panel, but a direct
 * esp_lcd_panel_draw_bitmap() call provably does (red-screen test passed).
 * So we drive LVGL's flush callback straight to draw_bitmap.
 *
 * We still call esp_lv_adapter_init() — it gives us the LVGL tick timer,
 * the worker task that calls lv_timer_handler(), and the lock used in main.c.
 */
#include "lvgl_adapter_init.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_ek79007.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "lvgl_adapter_init";

#define WAVESHARE_LCD_H_RES             1024
#define WAVESHARE_LCD_V_RES             600
#define WAVESHARE_BACKLIGHT_PIN         GPIO_NUM_26
#define WAVESHARE_LCD_RST_PIN           GPIO_NUM_33
#define WAVESHARE_LCD_BIT_PER_PIXEL     16
#define MIPI_DSI_PHY_LDO_CHAN           3
#define MIPI_DSI_PHY_LDO_VOLTAGE_MV     2500
#define MIPI_DSI_LANE_NUM               2

// Draw-buffer height for LVGL partial rendering (lines).
#define LVGL_DRAW_BUF_LINES             100

static esp_lcd_panel_handle_t s_panel = NULL;

// LVGL v9 flush callback — uses the exact call proven to work (draw_bitmap).
static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    static int flush_count = 0;
    int x1 = area->x1;
    int y1 = area->y1;
    int x2 = area->x2;
    int y2 = area->y2;

    if (flush_count < 5) {
        ESP_LOGW(TAG, "FLUSH #%d area x:%d-%d y:%d-%d", flush_count, x1, x2, y1, y2);
        flush_count++;
    }

    // draw_bitmap expects exclusive end coords (x2+1, y2+1)
    esp_err_t r = esp_lcd_panel_draw_bitmap(s_panel, x1, y1, x2 + 1, y2 + 1, px_map);
    if (r != ESP_OK && flush_count <= 5) {
        ESP_LOGE(TAG, "draw_bitmap err: %s", esp_err_to_name(r));
    }

    lv_display_flush_ready(disp);
}

static void lvgl_adapter_get_resolution(uint32_t *out_hres, uint32_t *out_vres)
{
    if (out_hres) *out_hres = WAVESHARE_LCD_H_RES;
    if (out_vres) *out_vres = WAVESHARE_LCD_V_RES;
}

lv_display_t *lvgl_adapter_init(const bsp_display_cfg_t *cfg)
{
    if (cfg == NULL) {
        ESP_LOGE(TAG, "Display config is NULL");
        return NULL;
    }

    ESP_LOGI(TAG, "Step 1: Backlight ON (GPIO%d)...", WAVESHARE_BACKLIGHT_PIN);
    gpio_config_t bk_gpio_config = {
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
        .pin_bit_mask = 1ULL << WAVESHARE_BACKLIGHT_PIN
    };
    gpio_config(&bk_gpio_config);
    gpio_set_level(WAVESHARE_BACKLIGHT_PIN, 1);

    ESP_LOGI(TAG, "Step 2: MIPI DSI PHY LDO...");
    esp_ldo_channel_handle_t ldo_mipi_phy = NULL;
    esp_ldo_channel_config_t ldo_config = {
        .chan_id    = MIPI_DSI_PHY_LDO_CHAN,
        .voltage_mv = MIPI_DSI_PHY_LDO_VOLTAGE_MV,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_config, &ldo_mipi_phy));
    ESP_LOGI(TAG, "Step 2: PHY powered on");

    ESP_LOGI(TAG, "Step 3: DSI bus...");
    esp_lcd_dsi_bus_handle_t dsi_bus = NULL;
    esp_lcd_dsi_bus_config_t bus_config = EK79007_PANEL_BUS_DSI_2CH_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_config, &dsi_bus));
    ESP_LOGI(TAG, "Step 3: DSI bus OK");

    ESP_LOGI(TAG, "Step 4: DBI IO...");
    esp_lcd_panel_io_handle_t dbi_io = NULL;
    esp_lcd_dbi_io_config_t dbi_config = EK79007_PANEL_IO_DBI_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_config, &dbi_io));
    ESP_LOGI(TAG, "Step 4: DBI IO OK");

    ESP_LOGI(TAG, "Step 5: EK79007 panel (RST=GPIO%d, RGB565)...", WAVESHARE_LCD_RST_PIN);
    esp_lcd_dpi_panel_config_t dpi_config =
        EK79007_1024_600_PANEL_60HZ_CONFIG(LCD_COLOR_PIXEL_FORMAT_RGB565);

    ek79007_vendor_config_t vendor_config = {
        .mipi_config = {
            .dsi_bus    = dsi_bus,
            .dpi_config = &dpi_config,
            .lane_num   = MIPI_DSI_LANE_NUM,
        },
    };
    esp_lcd_panel_dev_config_t panel_dev_config = {
        .reset_gpio_num = WAVESHARE_LCD_RST_PIN,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = WAVESHARE_LCD_BIT_PER_PIXEL,
        .vendor_config  = &vendor_config,
    };

    ESP_ERROR_CHECK(esp_lcd_new_panel_ek79007(dbi_io, &panel_dev_config, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_LOGI(TAG, "Step 5: Panel ready");

    ESP_LOGI(TAG, "Step 6: lv_init()...");
    lv_init();
    ESP_LOGI(TAG, "Step 6: LVGL core OK");

    uint32_t hres = 0, vres = 0;
    lvgl_adapter_get_resolution(&hres, &vres);

    ESP_LOGI(TAG, "Step 7: Manual LVGL display setup %ldx%ld...", hres, vres);

    lv_display_t *disp = lv_display_create(hres, vres);
    if (!disp) {
        ESP_LOGE(TAG, "lv_display_create failed");
        return NULL;
    }

    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, lvgl_flush_cb);

    // Two partial draw buffers in PSRAM
    size_t buf_px = (size_t)hres * LVGL_DRAW_BUF_LINES;
    size_t buf_bytes = buf_px * sizeof(uint16_t);
    void *buf1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
    void *buf2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
    if (!buf1 || !buf2) {
        ESP_LOGE(TAG, "draw buffer alloc failed");
        return NULL;
    }
    lv_display_set_buffers(disp, buf1, buf2, buf_bytes,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    ESP_LOGI(TAG, "Step 7: Display ready — init complete");
    return disp;
}
