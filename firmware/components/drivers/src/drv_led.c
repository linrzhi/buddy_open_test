/* SPDX-License-Identifier: Apache-2.0 */
/*
 * @file drv_led.c
 * @brief Solid, blink, breathe and rainbow effects for the common-cathode RGB LED.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "drv_led.h"

#include "board_config.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define LED_R_GPIO                 BOARD_LED_R_GPIO
#define LED_G_GPIO                 BOARD_LED_G_GPIO
#define LED_B_GPIO                 BOARD_LED_B_GPIO

#define LEDC_TIMER                 LEDC_TIMER_0
#define LEDC_MODE                  LEDC_LOW_SPEED_MODE
#define LEDC_DUTY_RES              LEDC_TIMER_8_BIT
#define LEDC_FREQUENCY_HZ          5000
#define LEDC_DUTY_MAX              255U

#define MICROSECONDS_PER_MS        1000LL
#define BREATHE_UPDATE_INTERVAL_MS 10U
#define EFFECT_UPDATE_INTERVAL_MS  20U

static const char *tag = "drv_led";

static const ledc_channel_t s_channels[] =
{
    LEDC_CHANNEL_0,
    LEDC_CHANNEL_1,
    LEDC_CHANNEL_2,
};

static const int s_gpios[] =
{
    LED_R_GPIO,
    LED_G_GPIO,
    LED_B_GPIO,
};

static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_update_timer;
static drv_led_config_t s_config;
static int64_t s_effect_start_us;
static int64_t s_next_update_us;
static uint8_t s_output_red;
static uint8_t s_output_green;
static uint8_t s_output_blue;
static bool s_initialized;

/**
 * @brief Set the brightness of one LEDC channel.
 * @param  channel: LEDC channel.
 * @param  brightness: brightness, 0-255.
 * @return ESP_OK on success, otherwise setting the LEDC duty failed.
 */
static esp_err_t set_channel_brightness(ledc_channel_t channel, uint8_t brightness)
{
    /* Common-cathode RGB LED, active high. */
    const uint32_t duty = brightness;
    esp_err_t err = ledc_set_duty(LEDC_MODE, channel, duty);
    if (err != ESP_OK)
    {
        return err;
    }

    return ledc_update_duty(LEDC_MODE, channel);
}

/**
 * @brief Update all three LED channels; the caller holds the mutex.
 * @param  red: red brightness.
 * @param  green: green brightness.
 * @param  blue: blue brightness.
 * @return ESP_OK on success, otherwise an LEDC channel update failed.
 */
static esp_err_t set_output_locked(uint8_t red, uint8_t green, uint8_t blue)
{
    const uint8_t previous_red = s_output_red;
    const uint8_t previous_green = s_output_green;
    const uint8_t previous_blue = s_output_blue;

    esp_err_t err = set_channel_brightness(s_channels[0], red);
    if (err != ESP_OK)
    {
        return err;
    }

    err = set_channel_brightness(s_channels[1], green);
    if (err != ESP_OK)
    {
        (void)set_channel_brightness(s_channels[0], previous_red);
        return err;
    }

    err = set_channel_brightness(s_channels[2], blue);
    if (err != ESP_OK)
    {
        (void)set_channel_brightness(s_channels[0], previous_red);
        (void)set_channel_brightness(s_channels[1], previous_green);
        (void)set_channel_brightness(s_channels[2], previous_blue);
        return err;
    }

    s_output_red = red;
    s_output_green = green;
    s_output_blue = blue;
    return ESP_OK;
}

/**
 * @brief Validate the LED mode and its timing.
 * @param  config: configuration to check.
 * @return true if valid.
 */
static bool config_valid(const drv_led_config_t *config)
{
    switch (config->mode)
    {
    case DRV_LED_MODE_CONSTANT:
    case DRV_LED_MODE_EFFECT:
        return true;
    case DRV_LED_MODE_BLINK:
        return config->timing_ms >= DRV_LED_BLINK_MIN_INTERVAL_MS &&
               config->timing_ms <= DRV_LED_BLINK_MAX_INTERVAL_MS;
    case DRV_LED_MODE_BREATHE:
        return config->timing_ms >= DRV_LED_BREATHE_MIN_PERIOD_MS &&
               config->timing_ms <= DRV_LED_BREATHE_MAX_PERIOD_MS;
    default:
        return false;
    }
}

/**
 * @brief Clear the fields the current mode does not use.
 * @param  config: configuration to normalize.
 */
static void normalize_config(drv_led_config_t *config)
{
    if (config->mode == DRV_LED_MODE_CONSTANT)
    {
        config->timing_ms = 0;
    }
    else if (config->mode == DRV_LED_MODE_EFFECT)
    {
        config->red = 0;
        config->green = 0;
        config->blue = 0;
        config->timing_ms = 0;
    }
}

/**
 * @brief Start or restart the one-shot LED timer; the caller holds the mutex.
 * @param  timeout_us: time until it fires, in us.
 * @return ESP_OK on success, otherwise a timer operation failed.
 */
static esp_err_t start_or_restart_timer_locked(uint64_t timeout_us)
{
    if (timeout_us == 0U)
    {
        timeout_us = 1U;
    }
    if (esp_timer_is_active(s_update_timer))
    {
        return esp_timer_restart(s_update_timer, timeout_us);
    }
    return esp_timer_start_once(s_update_timer, timeout_us);
}

/**
 * @brief Schedule the next LED update at an absolute time; the caller holds the mutex.
 * @param  deadline_us: absolute time of the next update, in us.
 * @return ESP_OK on success, otherwise a timer operation failed.
 */
static esp_err_t schedule_deadline_locked(int64_t deadline_us)
{
    const int64_t remaining_us = deadline_us - esp_timer_get_time();
    return start_or_restart_timer_locked(
        remaining_us > 0 ? (uint64_t)remaining_us : 1U);
}

/**
 * @brief On a scheduling failure, keep the current output and switch to solid mode.
 */
static void fallback_to_current_output_locked(void)
{
    s_config.mode = DRV_LED_MODE_CONSTANT;
    s_config.red = s_output_red;
    s_config.green = s_output_green;
    s_config.blue = s_output_blue;
    s_config.timing_ms = 0;
    s_effect_start_us = 0;
    s_next_update_us = 0;
}

/**
 * @brief Convert a hue on the color wheel to RGB brightness.
 * @param  hue: hue, 0-255.
 * @param  red: red brightness (output).
 * @param  green: green brightness (output).
 * @param  blue: blue brightness (output).
 */
static void hue_to_rgb(uint8_t hue, uint8_t *red, uint8_t *green, uint8_t *blue)
{
    const uint8_t sector = hue / 43U;
    const uint8_t remainder = (uint8_t)((hue % 43U) * 6U);

    switch (sector)
    {
    case 0:
        *red = 255;
        *green = remainder;
        *blue = 0;
        break;
    case 1:
        *red = (uint8_t)(255U - remainder);
        *green = 255;
        *blue = 0;
        break;
    case 2:
        *red = 0;
        *green = 255;
        *blue = remainder;
        break;
    case 3:
        *red = 0;
        *green = (uint8_t)(255U - remainder);
        *blue = 255;
        break;
    case 4:
        *red = remainder;
        *green = 0;
        *blue = 255;
        break;
    default:
        *red = 255;
        *green = 0;
        *blue = (uint8_t)(255U - remainder);
        break;
    }
}

/**
 * @brief Next update time strictly after now and aligned to the start time.
 * @param  start_us: animation start time, in us.
 * @param  now_us: current time, in us.
 * @param  interval_ms: update interval, in ms.
 * @return Absolute time of the next update, in us.
 */
static int64_t next_aligned_deadline(int64_t start_us,
                                     int64_t now_us,
                                     uint32_t interval_ms)
{
    const int64_t interval_us = (int64_t)interval_ms * MICROSECONDS_PER_MS;
    const int64_t elapsed_us = now_us - start_us;
    const int64_t completed_intervals = elapsed_us >= 0 ? elapsed_us / interval_us : 0;
    return start_us + (completed_intervals + 1) * interval_us;
}

/**
 * @brief Compute and apply the next animation frame for the current mode.
 * @param  arg: timer callback argument, unused.
 */
static void led_update_timer_callback(void *arg)
{
    (void)arg;

    esp_err_t output_err = ESP_OK;
    esp_err_t timer_err = ESP_OK;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (s_config.mode == DRV_LED_MODE_CONSTANT)
    {
        xSemaphoreGive(s_lock);
        return;
    }

    const int64_t now_us = esp_timer_get_time();
    if (s_next_update_us > now_us)
    {
        timer_err = schedule_deadline_locked(s_next_update_us);
    }
    else
    {
        uint8_t red = 0;
        uint8_t green = 0;
        uint8_t blue = 0;

        if (s_config.mode == DRV_LED_MODE_BLINK)
        {
            const int64_t interval_us =
                (int64_t)s_config.timing_ms * MICROSECONDS_PER_MS;
            const uint64_t interval_index =
                (uint64_t)((now_us - s_effect_start_us) / interval_us);
            if ((interval_index & 1U) == 0U)
            {
                red = s_config.red;
                green = s_config.green;
                blue = s_config.blue;
            }
            s_next_update_us = next_aligned_deadline(
                s_effect_start_us, now_us, s_config.timing_ms);
        }
        else if (s_config.mode == DRV_LED_MODE_BREATHE)
        {
            const int64_t period_us =
                (int64_t)s_config.timing_ms * MICROSECONDS_PER_MS;
            const int64_t phase_us = (now_us - s_effect_start_us) % period_us;
            const int64_t distance_us = phase_us <= period_us / 2
                ? phase_us
                : period_us - phase_us;
            const uint16_t level = (uint16_t)((distance_us * 510LL) / period_us);
            red = (uint8_t)(((uint16_t)s_config.red * level) / 255U);
            green = (uint8_t)(((uint16_t)s_config.green * level) / 255U);
            blue = (uint8_t)(((uint16_t)s_config.blue * level) / 255U);
            s_next_update_us = next_aligned_deadline(
                s_effect_start_us, now_us, BREATHE_UPDATE_INTERVAL_MS);
        }
        else if (s_config.mode == DRV_LED_MODE_EFFECT)
        {
            const uint64_t step = (uint64_t)((now_us - s_effect_start_us) /
                ((int64_t)EFFECT_UPDATE_INTERVAL_MS * MICROSECONDS_PER_MS));
            hue_to_rgb((uint8_t)(step * 2U), &red, &green, &blue);
            s_next_update_us = next_aligned_deadline(
                s_effect_start_us, now_us, EFFECT_UPDATE_INTERVAL_MS);
        }
        else
        {
            fallback_to_current_output_locked();
        }

        if (s_config.mode != DRV_LED_MODE_CONSTANT)
        {
            output_err = set_output_locked(red, green, blue);
            timer_err = schedule_deadline_locked(s_next_update_us);
        }
    }

    if (timer_err != ESP_OK)
    {
        fallback_to_current_output_locked();
    }

    xSemaphoreGive(s_lock);

    if (output_err != ESP_OK)
    {
        ESP_LOGE(tag, "LED animation update failed: %s", esp_err_to_name(output_err));
    }
    if (timer_err != ESP_OK)
    {
        ESP_LOGE(tag,
                 "LED animation timer failed; holding current color: %s",
                 esp_err_to_name(timer_err));
    }
}

/**
 * @brief Restore the previous LED state after a new configuration failed to apply.
 * @param  config: previously applied configuration.
 * @param  effect_start_us: previous animation start time, in us.
 * @param  next_update_us: previous next-update time, in us.
 * @param  red: previous red output.
 * @param  green: previous green output.
 * @param  blue: previous blue output.
 */
static void restore_previous_locked(const drv_led_config_t *config,
                                    int64_t effect_start_us,
                                    int64_t next_update_us,
                                    uint8_t red,
                                    uint8_t green,
                                    uint8_t blue)
{
    s_config = *config;
    s_effect_start_us = effect_start_us;
    s_next_update_us = next_update_us;

    const esp_err_t output_err = set_output_locked(red, green, blue);
    esp_err_t timer_err = ESP_OK;
    if (s_config.mode != DRV_LED_MODE_CONSTANT && s_next_update_us > 0)
    {
        timer_err = schedule_deadline_locked(s_next_update_us);
    }
    if (output_err != ESP_OK || timer_err != ESP_OK)
    {
        fallback_to_current_output_locked();
    }
}

/**
 * @brief Initialize the RGB LED PWM channels and the update timer.
 * @return ESP_OK on success, otherwise peripheral or resource setup failed.
 */
esp_err_t drv_led_init(void)
{
    if (s_initialized)
    {
        return ESP_OK;
    }

    const ledc_timer_config_t timer_config =
    {
        .speed_mode = LEDC_MODE,
        .duty_resolution = LEDC_DUTY_RES,
        .timer_num = LEDC_TIMER,
        .freq_hz = LEDC_FREQUENCY_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };

    esp_err_t err = ledc_timer_config(&timer_config);
    if (err != ESP_OK)
    {
        return err;
    }

    for (size_t i = 0; i < sizeof(s_channels) / sizeof(s_channels[0]); ++i)
    {
        const ledc_channel_config_t channel_config =
        {
            .gpio_num = s_gpios[i],
            .speed_mode = LEDC_MODE,
            .channel = s_channels[i],
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LEDC_TIMER,
            .duty = 0,
            .hpoint = 0,
            .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
            .flags =
            {
                .output_invert = 0,
            },
        };

        err = ledc_channel_config(&channel_config);
        if (err != ESP_OK)
        {
            return err;
        }
    }

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    const esp_timer_create_args_t timer_args =
    {
        .callback = led_update_timer_callback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "led_update",
    };
    err = esp_timer_create(&timer_args, &s_update_timer);
    if (err != ESP_OK)
    {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return err;
    }

    s_config = (drv_led_config_t)
    {
        .mode = DRV_LED_MODE_CONSTANT,
        .red = 170,
        .green = 255,
        .blue = 255,
        .timing_ms = 0,
    };
    err = set_output_locked(s_config.red, s_config.green, s_config.blue);
    if (err != ESP_OK)
    {
        (void)esp_timer_delete(s_update_timer);
        s_update_timer = NULL;
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return err;
    }

    s_initialized = true;
    ESP_LOGI(tag, "RGB LED initialized with white constant output");
    return ESP_OK;
}

/**
 * @brief Validate and atomically apply an LED configuration.
 * @param  config: configuration to apply.
 * @return ESP_OK on success, otherwise invalid argument, state, timer or hardware error.
 */
esp_err_t drv_led_set_config(const drv_led_config_t *config)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (config == NULL || !config_valid(config))
    {
        return ESP_ERR_INVALID_ARG;
    }

    drv_led_config_t next_config = *config;
    normalize_config(&next_config);

    xSemaphoreTake(s_lock, portMAX_DELAY);

    const drv_led_config_t previous_config = s_config;
    const int64_t previous_effect_start_us = s_effect_start_us;
    const int64_t previous_next_update_us = s_next_update_us;
    const uint8_t previous_red = s_output_red;
    const uint8_t previous_green = s_output_green;
    const uint8_t previous_blue = s_output_blue;

    esp_err_t err = ESP_OK;
    if (esp_timer_is_active(s_update_timer))
    {
        err = esp_timer_stop(s_update_timer);
    }

    const int64_t now_us = esp_timer_get_time();
    uint8_t first_red = 0;
    uint8_t first_green = 0;
    uint8_t first_blue = 0;
    int64_t next_update_us = 0;

    if (err == ESP_OK)
    {
        if (next_config.mode == DRV_LED_MODE_CONSTANT ||
            next_config.mode == DRV_LED_MODE_BLINK)
        {
            first_red = next_config.red;
            first_green = next_config.green;
            first_blue = next_config.blue;
        }
        else if (next_config.mode == DRV_LED_MODE_EFFECT)
        {
            hue_to_rgb(0, &first_red, &first_green, &first_blue);
        }

        err = set_output_locked(first_red, first_green, first_blue);
    }

    if (err == ESP_OK)
    {
        if (next_config.mode == DRV_LED_MODE_BLINK)
        {
            next_update_us = now_us +
                (int64_t)next_config.timing_ms * MICROSECONDS_PER_MS;
        }
        else if (next_config.mode == DRV_LED_MODE_BREATHE)
        {
            next_update_us = now_us +
                (int64_t)BREATHE_UPDATE_INTERVAL_MS * MICROSECONDS_PER_MS;
        }
        else if (next_config.mode == DRV_LED_MODE_EFFECT)
        {
            next_update_us = now_us +
                (int64_t)EFFECT_UPDATE_INTERVAL_MS * MICROSECONDS_PER_MS;
        }

        if (next_update_us > 0)
        {
            err = schedule_deadline_locked(next_update_us);
        }
    }

    if (err == ESP_OK)
    {
        s_config = next_config;
        s_effect_start_us = now_us;
        s_next_update_us = next_update_us;
    }
    else
    {
        restore_previous_locked(&previous_config,
                                previous_effect_start_us,
                                previous_next_update_us,
                                previous_red,
                                previous_green,
                                previous_blue);
    }

    xSemaphoreGive(s_lock);
    return err;
}

/**
 * @brief Get the last successfully applied LED configuration.
 * @param  config: buffer for the current configuration.
 * @return ESP_OK on success, otherwise not initialized or config is NULL.
 */
esp_err_t drv_led_get_config(drv_led_config_t *config)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    *config = s_config;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}
