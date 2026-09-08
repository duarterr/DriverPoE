/** @file voltage_sense.c
 * @brief ADC-based VBUS/LED voltage sensing implementation.
 */
#include "voltage_sense.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"

static const char *TAG = "VOLT_SENSE";

#define ADC_UNIT_USED           ADC_UNIT_1
#define ADC_ATTEN_USED          ADC_ATTEN_DB_12
#define FALLBACK_FULL_SCALE_MV  2450

static voltage_sense_config_t s_config;

static adc_oneshot_unit_handle_t s_adc_handle;
static adc_cali_handle_t s_cali_handle;
static bool s_calibrated = false;

/**
 * @brief Converts a raw ADC reading to millivolts.
 * @param raw Raw ADC sample.
 * @return Voltage in millivolts.
 */
static int raw_to_mv(int raw)
{
    if (s_calibrated) {
        int mv = 0;
        adc_cali_raw_to_voltage(s_cali_handle, raw, &mv);
        return mv;
    }
    return (raw * FALLBACK_FULL_SCALE_MV) / 4095;
}

void voltage_sense_init(const voltage_sense_config_t *config)
{
    s_config = *config;

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ADC_UNIT_USED,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc_handle));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_USED,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, s_config.vled_p_channel, &chan_cfg));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, s_config.vled_n_channel, &chan_cfg));

    adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_USED,
        .atten = ADC_ATTEN_USED,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .default_vref = 1100,
    };
    esp_err_t err = adc_cali_create_scheme_line_fitting(&cali_cfg, &s_cali_handle);
    if (err == ESP_OK) {
        s_calibrated = true;
        ESP_LOGI(TAG, "ADC calibration (line fitting) enabled");
    } else {
        s_calibrated = false;
        ESP_LOGW(TAG, "ADC calibration unavailable (err=%d) — using approximate conversion", err);
    }
}

esp_err_t voltage_sense_read(voltage_reading_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int raw_p = 0, raw_n = 0;
    esp_err_t err = adc_oneshot_read(s_adc_handle, s_config.vled_p_channel, &raw_p);
    if (err != ESP_OK) {
        return err;
    }
    err = adc_oneshot_read(s_adc_handle, s_config.vled_n_channel, &raw_n);
    if (err != ESP_OK) {
        return err;
    }

    int pin_p_mv = raw_to_mv(raw_p);
    int pin_n_mv = raw_to_mv(raw_n);
    int vbus_mv = (int)(pin_p_mv * s_config.divider_ratio);
    int vled_n_mv = (int)(pin_n_mv * s_config.divider_ratio);

    out->vbus_mv = vbus_mv;
    out->led_voltage_mv = vbus_mv - vled_n_mv;

    return ESP_OK;
}
