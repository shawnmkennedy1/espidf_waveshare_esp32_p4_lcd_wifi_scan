/*
 * SPDX-FileCopyrightText: 2026 Shawn Kennedy
 * SPDX-License-Identifier: Apache-2.0
 * Rev: 2026-06-22 v3  (WiFi scan UI replaces touch corner-button test)
 *
 * Touch is already verified, so the corner-button test UI is removed and
 * replaced with the WiFi scan/connect UI (wifi_scan_ui.c). The boot WiFi
 * task still brings the driver up and connects once; the scan UI reuses
 * that running STA driver to scan and re-connect to a tapped network.
 */
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "lvgl_adapter_init.h"
#include "touch_test.h"
#include "wifi_scan_ui.h"
#include "wifi_conn.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <assert.h>

static const char *TAG = "main";

// Bring up the WiFi driver WITHOUT connecting, so the device can scan and
// let the user pick a network. No hardcoded SSID -- works anywhere.
static void wifi_task(void *arg)
{
    esp_err_t err = wifi_init_only();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi init failed (%s)", esp_err_to_name(err));
    }
    vTaskDelete(NULL);
}

static void lv_tick_cb(void *arg)
{
    lv_tick_inc(2);
}

void app_main(void)
{
    bsp_display_cfg_t cfg = { 0 };

    lv_display_t *disp = lvgl_adapter_init(&cfg);
    assert(disp != NULL && "Failed to init LVGL adapter");

    const esp_timer_create_args_t tick_args = {
        .callback = &lv_tick_cb,
        .name = "lv_tick"
    };
    esp_timer_handle_t tick_timer;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, 2000));

    // Start touch BEFORE building UI so the indev exists
    touch_test_start();

    // Bring up WiFi in the background. wifi_start() creates the default event
    // loop, netif, and WiFi driver, then connects to the boot SSID (blocking
    // up to 30s for an IP) -- so it runs in its own task to keep the UI live.
    // The scan UI tolerates the loop not existing yet, and wifi_start()
    // tolerates the loop already existing, so order between the two is safe.
    xTaskCreate(wifi_task, "wifi_task", 5120, NULL, 5, NULL);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x102840), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // Build the WiFi scan / connect UI. The Scan button calls esp_wifi_scan_*,
    // which needs the driver started by wifi_task above; by the time a user
    // taps Scan, the boot connect has long since initialized the driver.
    wifi_scan_ui_create(scr);

    ESP_LOGI(TAG, "WiFi scan UI up.");

    while (1) {
        uint32_t delay_ms = lv_timer_handler();
        if (delay_ms == LV_NO_TIMER_READY) delay_ms = 5;
        if (delay_ms > 50) delay_ms = 50;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}
