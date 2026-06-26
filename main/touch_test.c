/*
 * SPDX-FileCopyrightText: 2026 Shawn Kennedy
 * SPDX-License-Identifier: Apache-2.0
 * Rev: 2026-06-21 v4  (axis SWAP enabled; one diagonal was transposed)
 *
 * GT911 touch -> LVGL input device for Waveshare ESP32-P4-WIFI6-Touch-LCD-7B.
 *
 * Board facts (confirmed from schematic + Waveshare wiki):
 *   I2C SCL = GPIO8, SDA = GPIO7, onboard pull-ups present.
 *   GT911 INT/RST route through the touch FPC (not host GPIOs) -> use -1.
 *
 * ORIENTATION HISTORY:
 *   v2: added a 180 touch flip -> double-flipped (TL fired BR). Removed.
 *   v3: no flip, no swap -> TL/BR correct but TR fired BL (one diagonal
 *       transposed) == X and Y are swapped on this panel/orientation.
 *   v4: SWAP_XY enabled. raw Y drives screen X, raw X drives screen Y.
 *
 * Calibration (raw GT911 ranges measured on this panel):
 *   raw X spans 41..1271  (the long, 1024 axis when NOT swapped)
 *   raw Y spans 18..788   (the short, 600 axis when NOT swapped)
 * With SWAP_XY, raw Y maps to screen X (0..1023) and raw X maps to screen
 * Y (0..599). Because the swapped raw ranges don't fill the target axis
 * 1:1, the mapping below stretches each raw range onto its screen axis.
 *
 * SET_RAW_LOG 1 prints raw + mapped coords for re-calibration.
 */
#include "touch_test.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lcd_panel_io.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "touch";

#define TOUCH_I2C_PORT     I2C_NUM_0
#define TOUCH_I2C_SCL      GPIO_NUM_8
#define TOUCH_I2C_SDA      GPIO_NUM_7
#define SCREEN_H_RES       1024
#define SCREEN_V_RES       600

// --- Raw GT911 spans measured on this panel ---
#define RAW_X_MIN   41
#define RAW_X_MAX   1271
#define RAW_Y_MIN   18
#define RAW_Y_MAX   788

// X and Y are transposed on this panel/orientation -> swap them.
#define SWAP_XY            1

// Optional 180 flip applied AFTER swap (leave 0 unless corners come out
// mirrored after the swap).
#define TOUCH_ROTATE_180   0

// Set to 1 to print raw + mapped coords to the log.
#define SET_RAW_LOG        0

static esp_lcd_touch_handle_t s_tp = NULL;

static inline int clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int map_raw(int raw, int rmin, int rmax, int out_max)
{
    if (rmax == rmin) return 0;
    int v = (raw - rmin) * out_max / (rmax - rmin);
    return clampi(v, 0, out_max);
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    esp_lcd_touch_read_data(s_tp);

    uint16_t x[1] = {0};
    uint16_t y[1] = {0};
    uint16_t strength[1] = {0};
    uint8_t cnt = 0;

    bool pressed = esp_lcd_touch_get_coordinates(s_tp, x, y, strength, &cnt, 1);

    if (pressed && cnt > 0) {
        int sx, sy;

#if SWAP_XY
        // raw Y -> screen X, raw X -> screen Y
        sx = map_raw(y[0], RAW_Y_MIN, RAW_Y_MAX, SCREEN_H_RES - 1);
        sy = map_raw(x[0], RAW_X_MIN, RAW_X_MAX, SCREEN_V_RES - 1);
#else
        sx = map_raw(x[0], RAW_X_MIN, RAW_X_MAX, SCREEN_H_RES - 1);
        sy = map_raw(y[0], RAW_Y_MIN, RAW_Y_MAX, SCREEN_V_RES - 1);
#endif

#if TOUCH_ROTATE_180
        sx = (SCREEN_H_RES - 1) - sx;
        sy = (SCREEN_V_RES - 1) - sy;
#endif

#if SET_RAW_LOG
        ESP_LOGI(TAG, "raw(%4d,%4d) -> screen(%4d,%4d)", x[0], y[0], sx, sy);
#endif

        data->point.x = sx;
        data->point.y = sy;
        data->state   = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

void touch_test_start(void)
{
    ESP_LOGI(TAG, "Creating I2C master bus (SCL=%d SDA=%d)...",
             TOUCH_I2C_SCL, TOUCH_I2C_SDA);

    i2c_master_bus_handle_t i2c_bus = NULL;
    i2c_master_bus_config_t bus_cfg = {
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .i2c_port                     = TOUCH_I2C_PORT,
        .scl_io_num                   = TOUCH_I2C_SCL,
        .sda_io_num                   = TOUCH_I2C_SDA,
        .glitch_ignore_cnt            = 7,
        .flags.enable_internal_pullup = false,  // board has its own pull-ups
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &i2c_bus));

    ESP_LOGI(TAG, "Creating GT911 panel IO...");
    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus, &tp_io_cfg, &tp_io));

    ESP_LOGI(TAG, "Initializing GT911...");
    esp_lcd_touch_config_t tp_cfg = {
        .x_max        = SCREEN_H_RES,
        .y_max        = SCREEN_V_RES,
        .rst_gpio_num = -1,   // routed via FPC, not host-controlled
        .int_gpio_num = -1,   // polling mode
        .flags = {
            .swap_xy  = 0,    // swap done manually in read cb
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &s_tp));

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);

    ESP_LOGI(TAG, "GT911 LVGL input device ready (calibrated, swap_xy).");
}
