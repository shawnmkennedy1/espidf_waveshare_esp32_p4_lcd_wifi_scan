/*
 * SPDX-FileCopyrightText: 2026 Shawn Kennedy
 * SPDX-License-Identifier: Apache-2.0
 * Rev: 2026-06-22 v1  (WiFi scan list + on-screen keyboard connect)
 *
 * Builds a WiFi UI on the given LVGL screen:
 *   [Scan] button -> async esp_wifi_scan_start
 *   -> list of SSIDs (sorted by RSSI) with signal strength
 *   -> tap an SSID -> password textarea + on-screen lv_keyboard
 *   -> [Connect] -> esp_wifi_disconnect + esp_wifi_connect with new config
 *
 * Assumes the WiFi driver is already initialized and started in STA mode
 * (done by the existing boot-time WiFi task). This module never calls
 * esp_wifi_init / esp_wifi_start / esp_netif setup -- it only scans and
 * (re)connects on the already-running driver.
 *
 * All LVGL calls happen on the LVGL task. Scan-done arrives on the WiFi
 * event task and is marshalled back via lv_async_call().
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Build the WiFi scan UI as children of `parent` (typically lv_screen_active()).
// Registers a WIFI_EVENT handler for SCAN_DONE / connect status.
void wifi_scan_ui_create(lv_obj_t *parent);

#ifdef __cplusplus
}
#endif
