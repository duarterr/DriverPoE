/** @file poe_luminaire_main.c
 * @brief Application entry point: wires up all components and starts the firmware.
 */
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_ota_ops.h"

#include "poe_luminaire_main.h"
#include "hv9910.h"
#include "driver_config.h"
#include "tps2378.h"
#include "power_manager.h"
#include "voltage_sense.h"
#include "eth_init.h"
#include "devid.h"
#include "admin_channel.h"
#include "dmx_input.h"
#include "status_leds.h"

static const char *TAG = "MAIN";

/**
 * @brief Confirms the running OTA image if it's still pending verification.
 * @return None.
 */
static void confirm_app_if_pending_verify(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) != ESP_OK) {
        return;
    }
    if (ota_state != ESP_OTA_IMG_PENDING_VERIFY) {
        return;
    }
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA rollback cancelled -- '%s' is now the confirmed boot image", running->label);
    } else {
        ESP_LOGE(TAG, "esp_ota_mark_app_valid_cancel_rollback failed (%s) -- bootloader may roll back "
                      "to the other slot on the next unconfirmed reset", esp_err_to_name(err));
    }
}

/**
 * @brief Application entry point.
 * @return None.
 */
void app_main(void)
{
    esp_log_level_set("MAIN", ESP_LOG_INFO);
    esp_log_level_set("HV9910", ESP_LOG_INFO);
    esp_log_level_set("TPS2378", ESP_LOG_INFO);
    esp_log_level_set("VOLT_SENSE", ESP_LOG_INFO);
    esp_log_level_set("ETH_INIT", ESP_LOG_INFO);
    esp_log_level_set("DEVID", ESP_LOG_INFO);
    esp_log_level_set("ADMIN_CH", ESP_LOG_INFO);
    esp_log_level_set("DMX_IN", ESP_LOG_INFO);
    esp_log_level_set("DMX_ARTNET", ESP_LOG_INFO);
    esp_log_level_set("DMX_SACN", ESP_LOG_INFO);

    status_leds_config_t leds_cfg = {
        .blue_pin = PIN_LED_BLUE,
        .red_pin = PIN_LED_RED,
        .blue_active_high = STATUS_LED_BLUE_ACTIVE_HIGH,
        .red_active_high = STATUS_LED_RED_ACTIVE_HIGH,
        .power_ok_fn = power_manager_indicator_ok,
        .driver_on_fn = hv9910_is_enabled,
    };
    status_leds_init(&leds_cfg);

    confirm_app_if_pending_verify();

    hv9910_config_t hv9910_cfg = {
        .pwmd_pin = PIN_HV9910_PWMD,
        .ld_pin = PIN_HV9910_LD,
        .pwmd_invert = HV9910_PWMD_INVERT,
        .ld_invert = HV9910_LD_INVERT,
    };
    hv9910_init(&hv9910_cfg);
    driver_config_start();   /* loads the dimming mode from NVS and pushes it to hv9910 */

    static const uint8_t admin_default_secret[DEVID_SECRET_LEN] = ADMIN_DEFAULT_SECRET;
    devid_config_t devid_cfg = {
        .model_prefix = DEVID_MODEL_PREFIX,
        .factory_default_secret = admin_default_secret,
    };
    devid_init(&devid_cfg);

    voltage_sense_config_t vsense_cfg = {
        .vled_p_channel = ADC_CH_VLED_P,
        .vled_n_channel = ADC_CH_VLED_N,
        .divider_ratio = VLED_DIVIDER_RATIO,
    };
    voltage_sense_init(&vsense_cfg);

    tps2378_config_t tps2378_cfg = {
        .cdb_pin = PIN_POE_CDB,
        .t2p_pin = PIN_POE_T2P,
        .vbus_min_mv = VBUS_MIN_MV,
        .vbus_hysteresis_mv = VBUS_HYSTERESIS_MV,
        .on_power_ready = power_manager_tps_ready,
        .on_power_lost = power_manager_tps_lost,
        .on_source_changed = power_manager_tps_source_changed,
        .callback_ctx = NULL,
    };
    tps2378_init(&tps2378_cfg);

    tps2378_wait_ready(portMAX_DELAY);

    /* Source class + configured power policy -> HV9910 LD cap / LED gate. */
    power_manager_start();

    eth_init_config_t eth_cfg = {
        .mdc_pin = PIN_ETH_MDC,
        .mdio_pin = PIN_ETH_MDIO,
        .ref_clk_pin = PIN_ETH_REF_CLK,
        .phy_reset_pin = PIN_ETH_PHY_RESET,
        .phy_addr = ETH_PHY_ADDR,
        .hostname = devid_get_model_prefix(),
    };
    esp_err_t eth_err = eth_bringup(&eth_cfg);
    if (eth_err == ESP_OK) {
        /* DMX layer first: the admin INFO response reads its status. */
        dmx_input_start();

        admin_channel_config_t admin_cfg = {
            .port = ADMIN_UDP_PORT,
        };
        admin_channel_start(&admin_cfg);
    } else {
        ESP_LOGE(TAG, "Ethernet bring-up failed (%s)", esp_err_to_name(eth_err));
    }
}
