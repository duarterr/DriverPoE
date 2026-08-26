/** @file voltage_sense.h
 * @brief VBUS and LED forward-voltage sensing via ADC.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "esp_adc/adc_oneshot.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Measured voltages, in millivolts. */
typedef struct {
    int vbus_mv;         /**< Bus voltage. */
    int led_voltage_mv;  /**< LED forward voltage. */
} voltage_reading_t;

/** @brief ADC channel and divider configuration. */
typedef struct {
    adc_channel_t vled_p_channel; /**< LED+ channel. */
    adc_channel_t vled_n_channel; /**< LED- channel. */
    float divider_ratio;          /**< Resistive divider scale factor. */
} voltage_sense_config_t;

/**
 * @brief Initializes the ADC for voltage sensing.
 * @param config Channels and divider ratio.
 * @return None.
 */
void voltage_sense_init(const voltage_sense_config_t *config);

/**
 * @brief Reads the bus and LED voltages.
 * @param out Destination for the reading.
 * @return ESP_OK on success; the underlying ADC error otherwise.
 */
esp_err_t voltage_sense_read(voltage_reading_t *out);

#ifdef __cplusplus
}
#endif
