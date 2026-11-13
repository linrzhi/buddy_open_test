/* SPDX-License-Identifier: Apache-2.0 */
/*
 * @file buddy_cmd.h
 * @brief Buddy text command protocol (transport independent).
 *
 * Protocol summary (see docs/protocol.md):
 * - The host sends one command per line, ending with '\n' or '\r', at most 255 bytes.
 * - The device replies with exactly one line per command: "OK" or "OK {json}" on success,
 *   "ERR <code> <message>" on failure.
 * - Unsolicited events are "EVT {json}" and may appear between any lines.
 * - The host ignores any other line (e.g. boot output).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Prefix of the version string returned by info; the full form is "buddy-open <PROJECT_VER>". */
#define BUDDY_CMD_VERSION_PREFIX "buddy-open "

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @brief Output callback that sends one whole reply or event line without interleaving.
 * @param  data: data buffer.
 * @param  length: data length in bytes.
 * @param  context: context given at registration.
 * @return ESP_OK on success; failures are ignored by the command handler.
 */
typedef esp_err_t (*buddy_cmd_write_fn_t)(const uint8_t *data,
                                          size_t length,
                                          void *context);

/**
 * @brief Initialize the command parser and send a boot event.
 * @param  write_fn: output callback, must not be NULL.
 * @param  context: output callback context.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if the callback is NULL.
 */
esp_err_t buddy_cmd_init(buddy_cmd_write_fn_t write_fn, void *context);

/**
 * @brief Feed received bytes; each complete line is parsed and executed.
 *
 * Call from a single task only (normally the serial receive task).
 *
 * @param  data: received data.
 * @param  length: data length in bytes.
 */
void buddy_cmd_feed(const uint8_t *data, size_t length);

#ifdef __cplusplus
}
#endif
