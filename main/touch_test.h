/*
 * SPDX-FileCopyrightText: 2026 Shawn Kennedy
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Starts the GT911 raw-coordinate test: inits I2C + GT911 and spawns a task
// that prints raw touch coordinates to the log. For calibration discovery.
void touch_test_start(void);

#ifdef __cplusplus
}
#endif
