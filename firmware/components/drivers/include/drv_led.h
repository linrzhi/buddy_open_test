/* SPDX-License-Identifier: Apache-2.0 */
/*
 * @file drv_led.h
 * @brief Common-cathode RGB LED driver.
 */

#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define DRV_LED_BLINK_MIN_INTERVAL_MS   1U
#define DRV_LED_BLINK_MAX_INTERVAL_MS   60000U
#define DRV_LED_BREATHE_MIN_PERIOD_MS   100U
#define DRV_LED_BREATHE_MAX_PERIOD_MS   60000U

typedef enum
{
    DRV_LED_MODE_CONSTANT = 0x01, /**< Solid color. */
    DRV_LED_MODE_BLINK = 0x02,    /**< Blink. */
    DRV_LED_MODE_BREATHE = 0x03,  /**< Breathe. */
    DRV_LED_MODE_EFFECT = 0x04,   /**< Rainbow color cycle. */
} drv_led_mode_t;

/** @brief RGB LED configuration. */
typedef struct
{
    drv_led_mode_t mode; /**< Mode. */
    uint8_t red;         /**< Red brightness, 0-255. */
    uint8_t green;       /**< Green brightness, 0-255. */
    uint8_t blue;        /**< Blue brightness, 0-255. */
    uint16_t timing_ms;  /**< Blink interval or breathe period in ms. */
} drv_led_config_t;

/**
 * @brief Initialize the RGB LED PWM channels and the update timer.
 * @return ESP_OK on success, otherwise peripheral or resource setup failed.
 */
esp_err_t drv_led_init(void);

/**
 * @brief Validate and atomically apply an LED configuration.
 * @param  config: configuration to apply.
 * @return ESP_OK on success, otherwise invalid argument, state, timer or hardware error.
 */
esp_err_t drv_led_set_config(const drv_led_config_t *config);

/**
 * @brief Get the last successfully applied LED configuration.
 * @param  config: buffer for the current configuration.
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not initialized,
 *         ESP_ERR_INVALID_ARG if config is NULL.
 */
esp_err_t drv_led_get_config(drv_led_config_t *config);

#ifdef __cplusplus
}
#endif
