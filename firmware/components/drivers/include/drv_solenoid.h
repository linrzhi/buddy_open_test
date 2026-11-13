/* SPDX-License-Identifier: Apache-2.0 */
/*
 * @file drv_solenoid.h
 * @brief Boost supply and solenoid driver.
 */

#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define DRV_SOLENOID_MIN_DURATION_MS 1U
#define DRV_SOLENOID_MAX_DURATION_MS 5000U
#define DRV_SOLENOID_MIN_COUNT       1U
#define DRV_SOLENOID_MAX_COUNT       255U
#define DRV_SOLENOID_MAX_INTERVAL_MS 5000U

/**
 * @brief Initialize the active-high solenoid output and enable the boost supply.
 * @return ESP_OK on success, otherwise GPIO, timer or resource setup failed.
 */
esp_err_t drv_solenoid_init(void);

/**
 * @brief Stop the current jump sequence and release the solenoid; the boost stays enabled.
 * @return ESP_OK on success, otherwise not initialized, timer or GPIO error.
 */
esp_err_t drv_solenoid_stop(void);

/**
 * @brief Start a non-blocking jump sequence.
 * @param  duration_ms: on-time of each jump in ms.
 * @param  count: number of jumps.
 * @param  interval_ms: gap between jumps in ms.
 * @return ESP_OK on success, otherwise invalid argument, state, timer or GPIO error.
 * @note A new valid sequence replaces the current one; the gap counts from the previous release.
 */
esp_err_t drv_solenoid_start_sequence(uint16_t duration_ms,
                                      uint8_t count,
                                      uint16_t interval_ms);

#ifdef __cplusplus
}
#endif
