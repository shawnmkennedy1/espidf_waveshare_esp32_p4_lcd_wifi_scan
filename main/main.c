/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "lv_demos.h"
#include "lvgl_adapter_init.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <assert.h>

static const char *TAG = "main";

// Feed LVGL its tick from an esp_timer (2ms period)
static void lv_tick_cb(void *arg)
{
    lv_tick_inc(2);
}

void app_main(void)
{
    bsp_display_cfg_t cfg = { 0 };

    lv_display_t *disp = lvgl_adapter_init(&cfg);
    assert(disp != NULL && "Failed to init LVGL adapter");

    // Our own tick source (the adapter's worker won't drive our manual display)
    const esp_timer_create_args_t tick_args = {
        .callback = &lv_tick_cb,
        .name = "lv_tick"
    };
    esp_timer_handle_t tick_timer;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, 2000)); // 2ms

    ESP_LOGI(TAG, "Launching LVGL benchmark demo...");
    lv_demo_benchmark();
    ESP_LOGI(TAG, "Demo launched, running LVGL loop");

    // Drive LVGL ourselves — call the handler continuously
    while (1) {
        uint32_t delay_ms = lv_timer_handler();
        if (delay_ms == LV_NO_TIMER_READY) delay_ms = 5;
        if (delay_ms > 50) delay_ms = 50;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}
