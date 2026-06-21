/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

typedef struct {
    int dummy;
} bsp_display_cfg_t;

lv_display_t *lvgl_adapter_init(const bsp_display_cfg_t *cfg);

#ifdef __cplusplus
}
#endif
