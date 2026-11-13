/* SPDX-License-Identifier: Apache-2.0 */
/*
 * @file board.h
 * @brief Board revision detection.
 */

#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* Board hardware revision. */
enum board_revision
{
    BOARD_REVISION_INVALID = 0,
    BOARD_REVISION_1,
    BOARD_REVISION_2,
    BOARD_REVISION_3,
    BOARD_REVISION_4,
};

/* Board detection result. */
struct board_info
{
    enum board_revision revision;
    uint32_t voltage_mv;
};

/**
 * @brief Read the hardware revision pin voltage and cache the board info.
 * @return ESP_OK on success, otherwise the ADC setup or read failed.
 */
esp_err_t board_init(void);

/**
 * @brief Get the cached board info.
 * @return Read-only pointer to the board info.
 */
const struct board_info *board_get_info(void);

/**
 * @brief Name of a board revision, e.g. "REV1".
 * @param  revision: board revision.
 * @return Revision name; "INVALID" for unknown values.
 */
const char *board_revision_name(enum board_revision revision);

#ifdef __cplusplus
}
#endif
