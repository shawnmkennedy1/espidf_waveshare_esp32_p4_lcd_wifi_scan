/*
 * SPDX-FileCopyrightText: 2026 Shawn Kennedy
 * SPDX-License-Identifier: Apache-2.0
 * Rev: 2026-06-22 v1  (WiFi scan list + on-screen keyboard connect)
 *
 * See wifi_scan_ui.h for behavior.
 *
 * THREADING:
 *   esp_wifi_scan_start(NULL, false) is async; results arrive on the WiFi
 *   event task as WIFI_EVENT_SCAN_DONE. LVGL is single-threaded, so the
 *   event handler does NOT touch LVGL -- it copies the AP records into a
 *   module buffer and schedules build_list_async() via lv_async_call(),
 *   which runs on the LVGL task during lv_timer_handler().
 *
 * DRIVER ASSUMPTION:
 *   The boot WiFi task already did esp_wifi_init + esp_netif + set_mode(STA)
 *   + esp_wifi_start and connected once. We reuse that running driver. To
 *   connect to a tapped network we esp_wifi_disconnect() then set a new
 *   STA config and esp_wifi_connect() -- independent of the boot connect
 *   routine, so it doesn't matter what that routine tears down.
 */
#include "wifi_scan_ui.h"

#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_wifi_types.h"
#include "wifi_conn.h"

static const char *TAG = "wifi_ui";

#define MAX_APS            20
#define SSID_MAX           33   // 32 + NUL
#define PASS_MAX           65   // 64 + NUL

// ---- Widget handles (LVGL task only) ----------------------------------------
static lv_obj_t *s_root      = NULL;  // container for list view
static lv_obj_t *s_scan_btn  = NULL;
static lv_obj_t *s_list      = NULL;
static lv_obj_t *s_status    = NULL;

static lv_obj_t *s_pw_panel  = NULL;  // password entry overlay (created on tap)
static lv_obj_t *s_pw_ta      = NULL;  // password textarea
static lv_obj_t *s_pw_kb      = NULL;  // on-screen keyboard
static lv_obj_t *s_pw_label   = NULL;  // "Password for <ssid>"

// ---- Scan results shared between WiFi task and LVGL task ---------------------
// Written by the event handler, read by build_list_async on the LVGL task.
// Single-producer/single-consumer with a one-shot async hop; the volatile
// count gate is sufficient here (no concurrent rebuild is scheduled).
static wifi_ap_record_t s_aps[MAX_APS];
static volatile uint16_t s_ap_count = 0;

static char s_selected_ssid[SSID_MAX] = {0};

// Poll timer: watches wifi_is_connected() after a connect attempt and updates
// the status label on the LVGL task (label updates must run here, not in the
// WiFi event handler).
static lv_timer_t *s_conn_poll = NULL;

static void conn_poll_cb(lv_timer_t *t)
{
    if (wifi_is_connected()) {
        lv_label_set_text_fmt(s_status, "Connected: %s", s_selected_ssid);
        lv_timer_del(t);
        s_conn_poll = NULL;
    }
}

// =============================================================================
// Forward decls
// =============================================================================
static void build_list_async(void *unused);
static void scan_btn_cb(lv_event_t *e);
static void ap_item_cb(lv_event_t *e);
static void pw_connect_cb(lv_event_t *e);
static void pw_cancel_cb(lv_event_t *e);
static void kb_event_cb(lv_event_t *e);
static void open_password_panel(const char *ssid);
static void close_password_panel(void);

// =============================================================================
// WiFi event handler  (runs on WiFi event task -- NO LVGL CALLS HERE)
// =============================================================================
static void wifi_evt_handler(void *arg, esp_event_base_t base,
                             int32_t id, void *data)
{
    if (base != WIFI_EVENT) {
        return;
    }

    switch (id) {
    case WIFI_EVENT_SCAN_DONE: {
        uint16_t num = MAX_APS;
        if (esp_wifi_scan_get_ap_records(&num, s_aps) == ESP_OK) {
            s_ap_count = num;
            ESP_LOGI(TAG, "Scan done: %u APs", (unsigned)num);
        } else {
            s_ap_count = 0;
            ESP_LOGW(TAG, "scan_get_ap_records failed");
        }
        // Hop to the LVGL task to build the list.
        lv_async_call(build_list_async, NULL);
        break;
    }
    case WIFI_EVENT_STA_CONNECTED:
        ESP_LOGI(TAG, "STA connected");
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        ESP_LOGW(TAG, "STA disconnected");
        break;
    default:
        break;
    }
}

// =============================================================================
// LVGL-task: build the scrollable AP list from s_aps[]
// =============================================================================
static int rssi_bars(int8_t rssi)
{
    if (rssi >= -55) return 4;
    if (rssi >= -65) return 3;
    if (rssi >= -75) return 2;
    return 1;
}

static void build_list_async(void *unused)
{
    (void)unused;

    if (!s_list) {
        return;
    }

    lv_obj_clean(s_list);  // drop previous entries

    uint16_t n = s_ap_count;
    if (n == 0) {
        lv_label_set_text(s_status, "No networks found");
        lv_obj_clear_state(s_scan_btn, LV_STATE_DISABLED);
        return;
    }

    for (uint16_t i = 0; i < n; i++) {
        const char *ssid = (const char *)s_aps[i].ssid;
        if (ssid[0] == '\0') {
            continue;  // hidden SSID, skip
        }

        char txt[SSID_MAX + 32];
        int bars = rssi_bars(s_aps[i].rssi);
        bool locked = (s_aps[i].authmode != WIFI_AUTH_OPEN);
        // e.g. "Yeshua   -58 dBm  |||.  [*]"  ([*] = secured, [ ] = open)
        snprintf(txt, sizeof(txt), "%-20s %d dBm  %.*s%.*s  %s",
                 ssid, s_aps[i].rssi, bars, "||||", 4 - bars, "....",
                 locked ? "[*]" : "[ ]");

        lv_obj_t *btn = lv_list_add_button(s_list, LV_SYMBOL_WIFI, txt);
        // Stash the SSID string pointer (stable: lives in s_aps until next scan)
        lv_obj_add_event_cb(btn, ap_item_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);
    }

    lv_label_set_text_fmt(s_status, "%u networks", (unsigned)n);
    lv_obj_clear_state(s_scan_btn, LV_STATE_DISABLED);
}

// =============================================================================
// Scan button
// =============================================================================
static void scan_btn_cb(lv_event_t *e)
{
    (void)e;

    lv_label_set_text(s_status, "Scanning...");
    lv_obj_add_state(s_scan_btn, LV_STATE_DISABLED);

    wifi_scan_config_t scan_cfg = {
        .ssid        = NULL,
        .bssid       = NULL,
        .channel     = 0,
        .show_hidden = false,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
    };

    // Async: returns immediately, completion fires WIFI_EVENT_SCAN_DONE.
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan_start failed: %s", esp_err_to_name(err));
        lv_label_set_text_fmt(s_status, "Scan error: %s",
                              esp_err_to_name(err));
        lv_obj_clear_state(s_scan_btn, LV_STATE_DISABLED);
    }
}

// =============================================================================
// Tapping an AP -> open password panel
// =============================================================================
static void ap_item_cb(lv_event_t *e)
{
    uint16_t idx = (uint16_t)(uintptr_t)lv_event_get_user_data(e);
    if (idx >= s_ap_count) {
        return;
    }
    const char *ssid = (const char *)s_aps[idx].ssid;
    strncpy(s_selected_ssid, ssid, sizeof(s_selected_ssid) - 1);
    s_selected_ssid[sizeof(s_selected_ssid) - 1] = '\0';
    open_password_panel(s_selected_ssid);
}

// =============================================================================
// Password panel + on-screen keyboard
// =============================================================================
static void open_password_panel(const char *ssid)
{
    if (s_pw_panel) {
        close_password_panel();
    }

    lv_obj_t *scr = lv_screen_active();

    s_pw_panel = lv_obj_create(scr);
    lv_obj_set_size(s_pw_panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_pw_panel, lv_color_hex(0x102840), 0);
    lv_obj_set_style_bg_opa(s_pw_panel, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_pw_panel, LV_OBJ_FLAG_SCROLLABLE);

    s_pw_label = lv_label_create(s_pw_panel);
    lv_label_set_text_fmt(s_pw_label, "Password for: %s", ssid);
    lv_obj_set_style_text_color(s_pw_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(s_pw_label, LV_ALIGN_TOP_LEFT, 10, 8);

    // Cancel button (top-right)
    lv_obj_t *cancel = lv_button_create(s_pw_panel);
    lv_obj_set_size(cancel, 110, 50);
    lv_obj_align(cancel, LV_ALIGN_TOP_RIGHT, -10, 4);
    lv_obj_add_event_cb(cancel, pw_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(cancel);
    lv_label_set_text(cl, "Cancel");
    lv_obj_center(cl);

    // Connect button (next to cancel)
    lv_obj_t *connect = lv_button_create(s_pw_panel);
    lv_obj_set_size(connect, 130, 50);
    lv_obj_align(connect, LV_ALIGN_TOP_RIGHT, -130, 4);
    lv_obj_add_event_cb(connect, pw_connect_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *col = lv_label_create(connect);
    lv_label_set_text(col, "Connect");
    lv_obj_center(col);

    // Password textarea
    s_pw_ta = lv_textarea_create(s_pw_panel);
    lv_textarea_set_one_line(s_pw_ta, true);
    lv_textarea_set_password_mode(s_pw_ta, true);
    lv_textarea_set_placeholder_text(s_pw_ta, "WiFi password");
    lv_obj_set_width(s_pw_ta, LV_PCT(94));
    lv_obj_align(s_pw_ta, LV_ALIGN_TOP_MID, 0, 64);

    // On-screen keyboard, bound to the textarea, docked bottom half
    s_pw_kb = lv_keyboard_create(s_pw_panel);
    lv_keyboard_set_textarea(s_pw_kb, s_pw_ta);
    lv_obj_set_size(s_pw_kb, LV_PCT(100), LV_PCT(55));
    lv_obj_align(s_pw_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    // READY (check) / CANCEL (X) on the keyboard itself
    lv_obj_add_event_cb(s_pw_kb, kb_event_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_pw_kb, kb_event_cb, LV_EVENT_CANCEL, NULL);
}

static void close_password_panel(void)
{
    if (s_pw_panel) {
        lv_obj_del(s_pw_panel);  // deletes children (kb, ta, labels) too
        s_pw_panel = NULL;
        s_pw_ta    = NULL;
        s_pw_kb    = NULL;
        s_pw_label = NULL;
    }
}

static void do_connect(void)
{
    const char *pw = lv_textarea_get_text(s_pw_ta);

    ESP_LOGI(TAG, "Connecting to '%s'...", s_selected_ssid);
    lv_label_set_text_fmt(s_status, "Connecting to %s...", s_selected_ssid);

    esp_err_t err = wifi_connect_to(s_selected_ssid, pw);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "connect failed: %s", esp_err_to_name(err));
        lv_label_set_text_fmt(s_status, "Connect error: %s",
                              esp_err_to_name(err));
    } else {
        // Poll for connection success and update the label (LVGL task).
        if (s_conn_poll) {
            lv_timer_del(s_conn_poll);
        }
        s_conn_poll = lv_timer_create(conn_poll_cb, 500, NULL);
    }

    close_password_panel();
}

static void pw_connect_cb(lv_event_t *e)
{
    (void)e;
    do_connect();
}

static void pw_cancel_cb(lv_event_t *e)
{
    (void)e;
    close_password_panel();
}

static void kb_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        do_connect();
    } else if (code == LV_EVENT_CANCEL) {
        close_password_panel();
    }
}

// =============================================================================
// Public: build the list view
// =============================================================================
void wifi_scan_ui_create(lv_obj_t *parent)
{
    // The default event loop and WiFi driver are brought up by wifi_start()
    // (wifi_conn.c), which must run before this. We only ADD a handler for
    // SCAN_DONE etc. Registering an extra handler is allowed -- handlers are
    // chained. If the loop somehow doesn't exist yet, create it defensively
    // rather than aborting (ESP_ERR_INVALID_STATE on register == no loop).
    esp_err_t reg = esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_evt_handler, NULL, NULL);
    if (reg == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "event loop missing; creating it");
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_evt_handler, NULL, NULL));
    } else {
        ESP_ERROR_CHECK(reg);
    }

    s_root = lv_obj_create(parent);
    lv_obj_set_size(s_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_root, lv_color_hex(0x102840), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    s_scan_btn = lv_button_create(s_root);
    lv_obj_set_size(s_scan_btn, 200, 60);
    lv_obj_align(s_scan_btn, LV_ALIGN_TOP_LEFT, 10, 10);
    lv_obj_add_event_cb(s_scan_btn, scan_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(s_scan_btn);
    lv_label_set_text(sl, LV_SYMBOL_REFRESH "  Scan WiFi");
    lv_obj_center(sl);

    s_status = lv_label_create(s_root);
    lv_label_set_text(s_status, "Tap Scan to list networks");
    lv_obj_set_style_text_color(s_status, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, 230, 30);

    s_list = lv_list_create(s_root);
    lv_obj_set_size(s_list, LV_PCT(96), 480);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 90);

    ESP_LOGI(TAG, "WiFi scan UI ready");
}
