/* SPDX-License-Identifier: Apache-2.0 */
/*
 * @file board_config.h
 * @brief Board GPIO and peripheral configuration.
 */

#pragma once

/* Board GPIOs. */
#define BOARD_LED_R_GPIO          6
#define BOARD_LED_G_GPIO          4
#define BOARD_LED_B_GPIO          5
#define BOARD_BOOST_ENABLE_GPIO   1
/* KEY1 (GPIO0) is the pinhole button: only used at power-up to enter ROM download mode. */
#define BOARD_KEY1_GPIO           0
#define BOARD_SOLENOID_GPIO       38
#define BOARD_HW_REV_GPIO         8
