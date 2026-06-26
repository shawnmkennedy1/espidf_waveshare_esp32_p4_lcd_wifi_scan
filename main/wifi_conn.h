/*
 * SPDX-FileCopyrightText: 2026 Shawn Kennedy
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Connects to a 2.4 GHz WiFi network via the onboard ESP32-C6.
// Blocks up to 30s for an IP. Returns ESP_OK on success.
esp_err_t wifi_start(const char *ssid, const char *password);

// Brings up NVS, netif, event loop, and the WiFi driver in STA mode and
// starts it -- WITHOUT connecting. Use this to scan before choosing a
// network. No-op if WiFi is already started.
esp_err_t wifi_init_only(void);

// Connect to a chosen network on the already-started driver (no init).
// Used by the scan UI after the user taps a network + enters a password.
esp_err_t wifi_connect_to(const char *ssid, const char *password);

// True once an IP has been acquired.
bool wifi_is_connected(void);

#ifdef __cplusplus
}
#endif
