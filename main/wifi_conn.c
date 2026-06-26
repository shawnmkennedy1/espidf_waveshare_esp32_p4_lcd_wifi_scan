/*
 * SPDX-FileCopyrightText: 2026 Shawn Kennedy
 * SPDX-License-Identifier: Apache-2.0
 * Rev: 2026-06-25 v2  (guard default-loop creation so scan UI can coexist)
 *
 * WiFi for Waveshare ESP32-P4-WIFI6-Touch-LCD-7B.
 *
 * The P4 has NO radio. WiFi comes from the onboard ESP32-C6 over SDIO.
 * esp_wifi_remote (added in idf_component.yml) forwards the standard
 * esp_wifi API to the C6, which is pre-flashed by Waveshare with the
 * ESP-Hosted slave firmware. So this code is plain esp_wifi station code;
 * nothing here mentions the C6 directly -- the remote layer is transparent.
 *
 * NOTE: the C6 is 2.4 GHz WiFi-6 only. The target AP must have a 2.4 GHz band.
 *
 * v2 CHANGE (only change from v1): esp_event_loop_create_default() is no
 * longer wrapped in ESP_ERROR_CHECK. The scan UI (wifi_scan_ui.c) may create
 * the default loop first; creating it again returns ESP_ERR_INVALID_STATE,
 * which is harmless and must not abort. All other logic is unchanged.
 */
#include "wifi_conn.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"

static const char *TAG = "wifi";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define WIFI_MAX_RETRY      8

static EventGroupHandle_t s_wifi_events;
static int s_retry = 0;
static bool s_started = false;
static bool s_want_connect = false;  // true only after a deliberate connect

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        // Do NOT auto-connect on start. Connection is initiated explicitly
        // by wifi_connect_to() so the device can scan first.
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (!s_want_connect) {
            return;  // not trying to connect (scanning/idle) -- ignore
        }
        if (s_retry < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry++;
            ESP_LOGW(TAG, "Disconnected, retry %d/%d", s_retry, WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
            ESP_LOGE(TAG, "Connect failed after %d retries", WIFI_MAX_RETRY);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry = 0;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_init_only(void)
{
    if (s_started) {
        return ESP_OK;
    }

    esp_err_t nv = nvs_flash_init();
    if (nv == ESP_ERR_NVS_NO_FREE_PAGES || nv == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(loop_err);
    }

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_started = true;

    ESP_LOGI(TAG, "WiFi driver started (STA, not connected).");
    return ESP_OK;
}

esp_err_t wifi_connect_to(const char *ssid, const char *password)
{
    if (!s_started) {
        ESP_ERROR_CHECK(wifi_init_only());
    }

    s_retry = 0;
    s_want_connect = true;
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    wifi_config_t wifi_config = { 0 };
    size_t sl = strnlen(ssid, sizeof(wifi_config.sta.ssid));
    memcpy(wifi_config.sta.ssid, ssid, sl);
    size_t pl = strnlen(password, sizeof(wifi_config.sta.password));
    memcpy(wifi_config.sta.password, password, pl);
    wifi_config.sta.threshold.authmode =
        (pl == 0) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    ESP_LOGI(TAG, "Connecting to SSID '%s' (2.4GHz)...", ssid);
    esp_wifi_disconnect();
    return esp_wifi_connect();
}

esp_err_t wifi_start(const char *ssid, const char *password)
{
    ESP_ERROR_CHECK(wifi_init_only());
    esp_err_t err = wifi_connect_to(ssid, password);
    if (err != ESP_OK) {
        return err;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected.");
        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect.");
        return ESP_FAIL;
    }
    ESP_LOGE(TAG, "Connect timed out.");
    return ESP_ERR_TIMEOUT;
}

bool wifi_is_connected(void)
{
    if (!s_wifi_events) return false;
    return (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) != 0;
}
