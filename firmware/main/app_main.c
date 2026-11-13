/* SPDX-License-Identifier: Apache-2.0 */
/*
 * @file app_main.c
 * @brief Firmware entry: USB-Serial-JTAG + Buddy text command protocol.
 *
 * USB-Serial-JTAG carries only flashing and text commands, never logs;
 * ESP_LOG goes to UART0 (GPIO43/44, on-board debug pads) only.
 */

#include <stdbool.h>
#include <stdint.h>

#include "board.h"
#include "buddy_cmd.h"
#include "drv_led.h"
#include "drv_solenoid.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb_serial_port.h"

#define APP_PM_MAX_CPU_FREQ_MHZ 160
#define APP_PM_MIN_CPU_FREQ_MHZ 80

static const char *tag = "app";

/**
 * @brief Enable dynamic frequency scaling; light sleep stays off (USB-Serial-JTAG stops in light sleep).
 * @return ESP_OK on success, otherwise power management setup failed.
 */
static esp_err_t app_power_init(void)
{
    const esp_pm_config_t pm_config =
    {
        .max_freq_mhz = APP_PM_MAX_CPU_FREQ_MHZ,
        .min_freq_mhz = APP_PM_MIN_CPU_FREQ_MHZ,
        .light_sleep_enable = false,
    };
    return esp_pm_configure(&pm_config);
}

/**
 * @brief Serial output adapter: command protocol -> USB-Serial-JTAG.
 */
static esp_err_t app_serial_write(const uint8_t *data, size_t length,
                                  void *context)
{
    (void)context;
    return usb_serial_port_write(data, length);
}

/**
 * @brief Serial input adapter: USB-Serial-JTAG -> command protocol.
 */
static void app_serial_receive(const uint8_t *data, size_t length,
                               void *context)
{
    (void)context;
    buddy_cmd_feed(data, length);
}

/**
 * @brief Initialize the open firmware.
 */
void app_main(void)
{
    ESP_ERROR_CHECK(app_power_init());
    ESP_ERROR_CHECK(board_init());
    ESP_ERROR_CHECK(drv_led_init());
    ESP_ERROR_CHECK(drv_solenoid_init());
    ESP_ERROR_CHECK(usb_serial_port_init(app_serial_receive, NULL));
    ESP_ERROR_CHECK(buddy_cmd_init(app_serial_write, NULL));

    ESP_LOGI(tag, "==============================");
    ESP_LOGI(tag, "Device : Buddy");
    ESP_LOGI(tag, "Version: %s%s", BUDDY_CMD_VERSION_PREFIX,
             esp_app_get_description()->version);
    ESP_LOGI(tag, "HW Rev : %s", board_revision_name(board_get_info()->revision));
    ESP_LOGI(tag, "==============================");
}
