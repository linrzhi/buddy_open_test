/* SPDX-License-Identifier: Apache-2.0 */
/*
 * @file usb_serial_port.c
 * @brief USB-Serial-JTAG serial I/O.
 */

#include "usb_serial_port.h"

#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define USB_SERIAL_PORT_RX_BUFFER_SIZE   512U
#define USB_SERIAL_PORT_TX_BUFFER_SIZE   1024U
#define USB_SERIAL_PORT_READ_CHUNK       64U
#define USB_SERIAL_PORT_READ_TIMEOUT_MS  100U
/* Max wait when the host is not reading; then data is dropped so the command task never stalls. */
#define USB_SERIAL_PORT_WRITE_TIMEOUT_MS 50U
#define USB_SERIAL_PORT_TASK_STACK_SIZE  4096U
#define USB_SERIAL_PORT_TASK_PRIORITY    4U

static const char *tag = "usb_serial";

static usb_serial_port_rx_cb_t s_rx_cb;
static void *s_rx_context;
static SemaphoreHandle_t s_tx_lock;
static bool s_initialized;

/**
 * @brief Read USB-Serial-JTAG data in a loop and pass it to the receive callback.
 * @param  arg: task argument, unused.
 */
static void usb_serial_port_rx_task(void *arg)
{
    (void)arg;
    uint8_t buffer[USB_SERIAL_PORT_READ_CHUNK];

    while (true)
    {
        const int length = usb_serial_jtag_read_bytes(
            buffer,
            sizeof(buffer),
            pdMS_TO_TICKS(USB_SERIAL_PORT_READ_TIMEOUT_MS));
        if (length > 0)
        {
            s_rx_cb(buffer, (size_t)length, s_rx_context);
        }
    }
}

esp_err_t usb_serial_port_init(usb_serial_port_rx_cb_t rx_cb, void *context)
{
    if (rx_cb == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_tx_lock = xSemaphoreCreateMutex();
    if (s_tx_lock == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    usb_serial_jtag_driver_config_t config =
    {
        .rx_buffer_size = USB_SERIAL_PORT_RX_BUFFER_SIZE,
        .tx_buffer_size = USB_SERIAL_PORT_TX_BUFFER_SIZE,
    };
    esp_err_t err = usb_serial_jtag_driver_install(&config);
    if (err != ESP_OK)
    {
        vSemaphoreDelete(s_tx_lock);
        s_tx_lock = NULL;
        return err;
    }

    s_rx_cb = rx_cb;
    s_rx_context = context;
    if (xTaskCreate(usb_serial_port_rx_task,
                    "usb_serial_rx",
                    USB_SERIAL_PORT_TASK_STACK_SIZE,
                    NULL,
                    USB_SERIAL_PORT_TASK_PRIORITY,
                    NULL) != pdPASS)
    {
        (void)usb_serial_jtag_driver_uninstall();
        vSemaphoreDelete(s_tx_lock);
        s_tx_lock = NULL;
        s_rx_cb = NULL;
        s_rx_context = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(tag, "USB-Serial-JTAG port ready");
    return ESP_OK;
}

esp_err_t usb_serial_port_write(const uint8_t *data, size_t length)
{
    if (data == NULL && length > 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized || !usb_serial_jtag_is_connected())
    {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    size_t written = 0U;
    while (written < length)
    {
        const int chunk = usb_serial_jtag_write_bytes(
            data + written,
            length - written,
            pdMS_TO_TICKS(USB_SERIAL_PORT_WRITE_TIMEOUT_MS));
        if (chunk <= 0)
        {
            break;
        }
        written += (size_t)chunk;
    }
    xSemaphoreGive(s_tx_lock);

    return written == length ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool usb_serial_port_is_connected(void)
{
    return s_initialized && usb_serial_jtag_is_connected();
}
