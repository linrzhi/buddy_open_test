/* SPDX-License-Identifier: Apache-2.0 */
/*
 * @file board.c
 * @brief Board hardware revision detection.
 */

#include "board.h"

#include <stddef.h>
#include <stdint.h>

#include "board_config.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"

#define BOARD_HW_REV_SAMPLE_COUNT    16U
#define BOARD_HW_REV_1_THRESHOLD_MV  2550U
#define BOARD_HW_REV_2_THRESHOLD_MV  2050U
#define BOARD_HW_REV_3_THRESHOLD_MV  1500U
#define BOARD_HW_REV_4_THRESHOLD_MV  800U

static struct board_info s_board_info;

/**
 * @brief Map the hardware revision voltage to a board revision.
 * @param  voltage_mv: hardware revision pin voltage in mV.
 * @return Board revision; invalid if the voltage is too low.
 */
static enum board_revision revision_from_voltage(uint32_t voltage_mv)
{
    if (voltage_mv > BOARD_HW_REV_1_THRESHOLD_MV)
    {
        return BOARD_REVISION_1;
    }
    if (voltage_mv > BOARD_HW_REV_2_THRESHOLD_MV)
    {
        return BOARD_REVISION_2;
    }
    if (voltage_mv > BOARD_HW_REV_3_THRESHOLD_MV)
    {
        return BOARD_REVISION_3;
    }
    if (voltage_mv > BOARD_HW_REV_4_THRESHOLD_MV)
    {
        return BOARD_REVISION_4;
    }
    return BOARD_REVISION_INVALID;
}

/**
 * @brief Sample the hardware revision pin several times with the calibrated ADC.
 * @param  voltage_mv: average voltage in mV (output).
 * @return ESP_OK on success, otherwise an ADC operation failed.
 */
static esp_err_t read_hardware_voltage(uint32_t *voltage_mv)
{
    if (voltage_mv == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    adc_unit_t unit_id;
    adc_channel_t channel;
    esp_err_t err = adc_oneshot_io_to_channel(BOARD_HW_REV_GPIO,
                                               &unit_id,
                                               &channel);
    if (err != ESP_OK)
    {
        return err;
    }

    adc_oneshot_unit_handle_t adc_handle = NULL;
    adc_cali_handle_t calibration_handle = NULL;
    const adc_oneshot_unit_init_cfg_t unit_config =
    {
        .unit_id = unit_id,
    };
    err = adc_oneshot_new_unit(&unit_config, &adc_handle);
    if (err != ESP_OK)
    {
        return err;
    }

    const adc_oneshot_chan_cfg_t channel_config =
    {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_oneshot_config_channel(adc_handle, channel, &channel_config);
    if (err != ESP_OK)
    {
        goto cleanup;
    }

    const adc_cali_curve_fitting_config_t calibration_config =
    {
        .unit_id = unit_id,
        .chan = channel,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_cali_create_scheme_curve_fitting(&calibration_config,
                                                &calibration_handle);
    if (err != ESP_OK)
    {
        goto cleanup;
    }

    uint32_t voltage_sum_mv = 0U;
    for (uint32_t i = 0U; i < BOARD_HW_REV_SAMPLE_COUNT; ++i)
    {
        int raw_value;
        int sample_voltage_mv;

        err = adc_oneshot_read(adc_handle, channel, &raw_value);
        if (err != ESP_OK)
        {
            goto cleanup;
        }
        err = adc_cali_raw_to_voltage(calibration_handle,
                                      raw_value,
                                      &sample_voltage_mv);
        if (err != ESP_OK)
        {
            goto cleanup;
        }
        voltage_sum_mv += (uint32_t)sample_voltage_mv;
    }
    *voltage_mv = voltage_sum_mv / BOARD_HW_REV_SAMPLE_COUNT;

cleanup:
    if (calibration_handle != NULL)
    {
        const esp_err_t cleanup_err =
            adc_cali_delete_scheme_curve_fitting(calibration_handle);
        if (err == ESP_OK)
        {
            err = cleanup_err;
        }
    }
    if (adc_handle != NULL)
    {
        const esp_err_t cleanup_err = adc_oneshot_del_unit(adc_handle);
        if (err == ESP_OK)
        {
            err = cleanup_err;
        }
    }
    return err;
}

esp_err_t board_init(void)
{
    uint32_t voltage_mv;
    const esp_err_t err = read_hardware_voltage(&voltage_mv);
    if (err != ESP_OK)
    {
        return err;
    }

    s_board_info.voltage_mv = voltage_mv;
    s_board_info.revision = revision_from_voltage(voltage_mv);
    return ESP_OK;
}

const struct board_info *board_get_info(void)
{
    return &s_board_info;
}

const char *board_revision_name(enum board_revision revision)
{
    switch (revision)
    {
    case BOARD_REVISION_1:
        return "REV1";
    case BOARD_REVISION_2:
        return "REV2";
    case BOARD_REVISION_3:
        return "REV3";
    case BOARD_REVISION_4:
        return "REV4";
    case BOARD_REVISION_INVALID:
    default:
        return "INVALID";
    }
}
