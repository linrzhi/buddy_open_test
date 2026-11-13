/* SPDX-License-Identifier: Apache-2.0 */
/*
 * @file drv_solenoid.c
 * @brief Boost supply control and non-blocking solenoid jump sequences.
 */

#include <stdbool.h>
#include <stdint.h>

#include "drv_solenoid.h"

#include "board_config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define BOOST_ENABLE_GPIO   BOARD_BOOST_ENABLE_GPIO
#define SOLENOID_GPIO       BOARD_SOLENOID_GPIO
#define MICROSECONDS_PER_MS 1000ULL

static const char *tag = "drv_solenoid";

typedef enum
{
    SOLENOID_PHASE_IDLE = 0,
    SOLENOID_PHASE_ACTIVE,
    SOLENOID_PHASE_WAITING,
} solenoid_phase_t;

static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_phase_timer;
static solenoid_phase_t s_phase;
static int64_t s_phase_deadline_us;
static uint16_t s_duration_ms;
static uint16_t s_interval_ms;
static uint8_t s_remaining_activations;
static bool s_initialized;

/**
 * @brief Start or restart the solenoid phase timer; the caller holds the mutex.
 * @param  timeout_us: time until it fires, in us.
 * @return ESP_OK on success, otherwise a timer operation failed.
 */
static esp_err_t start_or_restart_timer_locked(uint64_t timeout_us)
{
    if (esp_timer_is_active(s_phase_timer))
    {
        return esp_timer_restart(s_phase_timer, timeout_us);
    }

    return esp_timer_start_once(s_phase_timer, timeout_us);
}

/**
 * @brief Clear the sequence state; the caller holds the mutex.
 */
static void clear_sequence_locked(void)
{
    s_phase = SOLENOID_PHASE_IDLE;
    s_phase_deadline_us = 0;
    s_duration_ms = 0;
    s_interval_ms = 0;
    s_remaining_activations = 0;
}

/**
 * @brief Clear the sequence and force the solenoid off; the caller holds the mutex.
 * @return ESP_OK on success, otherwise setting the GPIO failed.
 */
static esp_err_t fail_safe_off_locked(void)
{
    clear_sequence_locked();
    return gpio_set_level(SOLENOID_GPIO, 0);
}

/**
 * @brief Advance the on, gap and done phases of a sequence.
 * @param  arg: timer callback argument, unused.
 */
static void solenoid_phase_timer_callback(void *arg)
{
    (void)arg;

    bool sequence_complete = false;
    esp_err_t timer_err = ESP_OK;
    esp_err_t gpio_err = ESP_OK;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (s_phase == SOLENOID_PHASE_IDLE)
    {
        xSemaphoreGive(s_lock);
        return;
    }

    const int64_t now_us = esp_timer_get_time();
    const int64_t remaining_us = s_phase_deadline_us - now_us;
    if (remaining_us > 0)
    {
        timer_err = start_or_restart_timer_locked((uint64_t)remaining_us);
    }
    else if (s_phase == SOLENOID_PHASE_ACTIVE)
    {
        gpio_err = gpio_set_level(SOLENOID_GPIO, 0);
        if (gpio_err == ESP_OK && s_remaining_activations == 0U)
        {
            clear_sequence_locked();
            sequence_complete = true;
        }
        else if (gpio_err == ESP_OK && s_interval_ms == 0U)
        {
            gpio_err = gpio_set_level(SOLENOID_GPIO, 1);
            if (gpio_err == ESP_OK)
            {
                --s_remaining_activations;
                s_phase_deadline_us = now_us +
                    (int64_t)s_duration_ms * MICROSECONDS_PER_MS;
                timer_err = start_or_restart_timer_locked(
                    (uint64_t)s_duration_ms * MICROSECONDS_PER_MS);
            }
        }
        else if (gpio_err == ESP_OK)
        {
            s_phase = SOLENOID_PHASE_WAITING;
            s_phase_deadline_us = now_us +
                (int64_t)s_interval_ms * MICROSECONDS_PER_MS;
            timer_err = start_or_restart_timer_locked(
                (uint64_t)s_interval_ms * MICROSECONDS_PER_MS);
        }
    }
    else
    {
        gpio_err = gpio_set_level(SOLENOID_GPIO, 1);
        if (gpio_err == ESP_OK)
        {
            --s_remaining_activations;
            s_phase = SOLENOID_PHASE_ACTIVE;
            s_phase_deadline_us = now_us +
                (int64_t)s_duration_ms * MICROSECONDS_PER_MS;
            timer_err = start_or_restart_timer_locked(
                (uint64_t)s_duration_ms * MICROSECONDS_PER_MS);
        }
    }

    if (gpio_err != ESP_OK || timer_err != ESP_OK)
    {
        (void)fail_safe_off_locked();
    }

    xSemaphoreGive(s_lock);

    if (gpio_err != ESP_OK)
    {
        ESP_LOGE(tag, "Sequence GPIO update failed: %s", esp_err_to_name(gpio_err));
    }
    if (timer_err != ESP_OK)
    {
        ESP_LOGE(tag, "Sequence timer restart failed: %s", esp_err_to_name(timer_err));
    }
    if (sequence_complete)
    {
        ESP_LOGI(tag, "Solenoid sequence complete");
    }
}

/**
 * @brief Initialize the active-high solenoid output and enable the boost supply.
 * @return ESP_OK on success, otherwise GPIO, timer or resource setup failed.
 */
esp_err_t drv_solenoid_init(void)
{
    if (s_initialized)
    {
        return ESP_OK;
    }

    esp_err_t err = gpio_reset_pin(SOLENOID_GPIO);
    if (err != ESP_OK)
    {
        return err;
    }

    err = gpio_reset_pin(BOOST_ENABLE_GPIO);
    if (err != ESP_OK)
    {
        return err;
    }

    err = gpio_set_level(SOLENOID_GPIO, 0);
    if (err != ESP_OK)
    {
        return err;
    }

    err = gpio_set_level(BOOST_ENABLE_GPIO, 0);
    if (err != ESP_OK)
    {
        return err;
    }

    const gpio_config_t config =
    {
        .pin_bit_mask = (1ULL << BOOST_ENABLE_GPIO) | (1ULL << SOLENOID_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    err = gpio_config(&config);
    if (err != ESP_OK)
    {
        return err;
    }

    err = gpio_set_level(SOLENOID_GPIO, 0);
    if (err != ESP_OK)
    {
        return err;
    }

    err = gpio_set_level(BOOST_ENABLE_GPIO, 0);
    if (err != ESP_OK)
    {
        return err;
    }

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    const esp_timer_create_args_t timer_args =
    {
        .callback = solenoid_phase_timer_callback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "solenoid_phase",
    };

    err = esp_timer_create(&timer_args, &s_phase_timer);
    if (err != ESP_OK)
    {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return err;
    }

    /* Enable the boost only after the output and release timer are ready, to avoid a stray pulse. */
    err = gpio_set_level(BOOST_ENABLE_GPIO, 1);
    if (err != ESP_OK)
    {
        gpio_set_level(SOLENOID_GPIO, 0);
        gpio_set_level(BOOST_ENABLE_GPIO, 0);
        esp_timer_delete(s_phase_timer);
        s_phase_timer = NULL;
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return err;
    }

    s_initialized = true;
    ESP_LOGI(tag,
             "Solenoid initialized on GPIO%d; boost enabled on GPIO%d",
             SOLENOID_GPIO,
             BOOST_ENABLE_GPIO);
    return ESP_OK;
}

/** @brief Stop the phase timer, release the solenoid now and drop pending jumps. */
esp_err_t drv_solenoid_stop(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (esp_timer_is_active(s_phase_timer))
    {
        err = esp_timer_stop(s_phase_timer);
    }
    const esp_err_t gpio_err = fail_safe_off_locked();
    xSemaphoreGive(s_lock);
    return err != ESP_OK ? err : gpio_err;
}

/**
 * @brief Start a non-blocking jump sequence.
 * @param  duration_ms: on-time of each jump in ms.
 * @param  count: number of jumps.
 * @param  interval_ms: gap between jumps in ms.
 * @return ESP_OK on success, otherwise invalid argument, state, timer or GPIO error.
 */
esp_err_t drv_solenoid_start_sequence(uint16_t duration_ms,
                                      uint8_t count,
                                      uint16_t interval_ms)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (duration_ms < DRV_SOLENOID_MIN_DURATION_MS ||
        duration_ms > DRV_SOLENOID_MAX_DURATION_MS ||
        count < DRV_SOLENOID_MIN_COUNT ||
        interval_ms > DRV_SOLENOID_MAX_INTERVAL_MS)
    {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    esp_err_t err = ESP_OK;
    if (esp_timer_is_active(s_phase_timer))
    {
        err = esp_timer_stop(s_phase_timer);
    }
    if (err == ESP_OK)
    {
        err = gpio_set_level(SOLENOID_GPIO, 0);
    }

    clear_sequence_locked();
    if (err == ESP_OK)
    {
        s_duration_ms = duration_ms;
        s_interval_ms = interval_ms;
        s_remaining_activations = (uint8_t)(count - 1U);
        s_phase = SOLENOID_PHASE_ACTIVE;
        s_phase_deadline_us = esp_timer_get_time() +
            (int64_t)duration_ms * MICROSECONDS_PER_MS;

        err = gpio_set_level(SOLENOID_GPIO, 1);
    }
    if (err == ESP_OK)
    {
        err = start_or_restart_timer_locked(
            (uint64_t)duration_ms * MICROSECONDS_PER_MS);
    }

    if (err != ESP_OK)
    {
        (void)fail_safe_off_locked();
    }

    xSemaphoreGive(s_lock);

    if (err == ESP_OK)
    {
        ESP_LOGI(tag,
                 "Solenoid sequence: duration=%u ms count=%u interval=%u ms",
                 duration_ms,
                 count,
                 interval_ms);
    }
    return err;
}
