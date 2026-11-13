/* SPDX-License-Identifier: Apache-2.0 */
/*
 * @file usb_serial_port.h
 * @brief Serial I/O over the chip's built-in USB-Serial-JTAG.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @brief Receive callback, called from the serial receive task.
 * @param  data: received data.
 * @param  length: data length in bytes.
 * @param  context: context given at registration.
 */
typedef void (*usb_serial_port_rx_cb_t)(const uint8_t *data,
                                        size_t length,
                                        void *context);

/**
 * @brief Install the USB-Serial-JTAG driver and start the receive task.
 * @param  rx_cb: receive callback, must not be NULL.
 * @param  context: receive callback context.
 * @return ESP_OK on success, otherwise invalid argument, already initialized or out of resources.
 */
esp_err_t usb_serial_port_init(usb_serial_port_rx_cb_t rx_cb, void *context);

/**
 * @brief Send data; concurrent callers never interleave within one call.
 *
 * If the host is not connected or stops reading, the rest is dropped after a timeout, so callers never block.
 *
 * @param  data: data to send.
 * @param  length: data length in bytes.
 * @return ESP_OK if all sent, ESP_ERR_TIMEOUT if some or all was dropped,
 *         ESP_ERR_INVALID_STATE if the host is not connected or not initialized.
 */
esp_err_t usb_serial_port_write(const uint8_t *data, size_t length);

/**
 * @brief Whether a USB host is connected (SOF seen; the port may still be closed).
 * @return true if connected.
 */
bool usb_serial_port_is_connected(void);

#ifdef __cplusplus
}
#endif
