#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"

#include "poe_luminaire_main.h"
#include "hv9910.h"
#include "tps2378.h"
#include "voltage_sense.h"
#include "eth_init.h"
#include "devid.h"
#include "admin_channel.h"
#include "status_leds.h"

static const char *TAG = "MAIN";

static bool s_first_power_ready_seen;
static esp_timer_handle_t s_poweron_settle_timer;

static void poweron_settle_cb(void *arg)
{
    tps2378_source_t source = (tps2378_source_t)(intptr_t)arg;
    if (!tps2378_is_ready()) {
        ESP_LOGI(TAG, "Power-on settle: power dropped before delay completed");
        return;
    }

    ESP_LOGI(TAG, "Power-on settle complete: %s", tps2378_source_name(source));
    hv9910_enable(HV9910_DEFAULT_RAMP_MS, false);
}

static void on_poe_power_ready(tps2378_source_t source, void *ctx)
{
    (void)ctx;

    if (!hv9910_was_last_on()) {
        return;
    }

    if (!s_first_power_ready_seen) {
        s_first_power_ready_seen = true;
        const esp_timer_create_args_t timer_args = {
            .callback = poweron_settle_cb,
            .arg = (void *)(intptr_t)source,
            .name = "poweron_settle",
        };
        esp_err_t err = esp_timer_create(&timer_args, &s_poweron_settle_timer);
        if (err == ESP_OK) {
            err = esp_timer_start_once(s_poweron_settle_timer,
                                       (uint64_t)POWERON_SETTLE_MS * 1000);
        }
        if (err == ESP_OK) {
            return;
        }
        ESP_LOGW(TAG, "Power-on timer failed (%s)", esp_err_to_name(err));
    }

    hv9910_enable(HV9910_DEFAULT_RAMP_MS, false);
}

static void on_poe_power_lost(void *ctx)
{
    (void)ctx;
    hv9910_emergency_disable();
}

/* Completes the OTA rollback contract that CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
 * (sdkconfig.defaults) and the ota_0/ota_1 partition table (partitions.csv)
 * set up but don't finish on their own: whenever the running image is still
 * ESP_OTA_IMG_PENDING_VERIFY (the state the bootloader leaves it in right
 * after a boot-partition switch -- currently that only happens via
 * esptool/idf.py flash writing directly to a slot with rollback already
 * enabled, since no esp_ota_* / esp_https_ota update client exists yet), it
 * must be confirmed with esp_ota_mark_app_valid_cancel_rollback() or the
 * bootloader will roll back to the other slot the next time this one
 * resets before confirming. No self-test worth gating this on exists yet,
 * so this confirms unconditionally and early -- getting this far (past
 * app_main() being entered at all) is the only bar there currently is.
 * esp_ota_get_state_partition() returning anything other than ESP_OK means
 * the running partition isn't OTA-managed (e.g. a "factory" image from the
 * old single-slot partition table) -- nothing to confirm in that case. */
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

void app_main(void)
{
    esp_log_level_set("MAIN", ESP_LOG_INFO);
    esp_log_level_set("HV9910", ESP_LOG_INFO);
    esp_log_level_set("TPS2378", ESP_LOG_INFO);
    esp_log_level_set("VOLT_SENSE", ESP_LOG_INFO);
    esp_log_level_set("ETH_INIT", ESP_LOG_INFO);
    esp_log_level_set("DEVID", ESP_LOG_INFO);
    esp_log_level_set("ADMIN_CH", ESP_LOG_INFO);

    status_leds_config_t leds_cfg = {
        .blue_pin = PIN_LED_BLUE,
        .red_pin = PIN_LED_RED,
        .blue_active_high = STATUS_LED_BLUE_ACTIVE_HIGH,
        .red_active_high = STATUS_LED_RED_ACTIVE_HIGH,
        .power_ok_fn = tps2378_vbus_confirmed,
        .driver_on_fn = hv9910_is_enabled,
    };
    status_leds_init(&leds_cfg);

    confirm_app_if_pending_verify();

    hv9910_config_t hv9910_cfg = {
        .shutdown_pin = PIN_HV9910_SHUTDOWN,
        .dimming_pin = PIN_HV9910_DIMMING,
        .shutdown_active_high = HV9910_SHUTDOWN_ACTIVE_HIGH,
        .dim_active_high = HV9910_DIM_ACTIVE_HIGH,
        .pwm_freq_hz = HV9910_PWM_FREQ_HZ,
        .max_ramp_ms = HV9910_MAX_RAMP_MS,
    };
    hv9910_init(&hv9910_cfg);

    devid_config_t devid_cfg = {
        .model_prefix = DEVID_MODEL_PREFIX,
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
        .on_power_ready = on_poe_power_ready,
        .on_power_lost = on_poe_power_lost,
        .callback_ctx = NULL,
    };
    tps2378_init(&tps2378_cfg);

    tps2378_wait_ready(portMAX_DELAY);

    eth_init_config_t eth_cfg = {
        .mdc_pin = PIN_ETH_MDC,
        .mdio_pin = PIN_ETH_MDIO,
        .ref_clk_pin = PIN_ETH_REF_CLK,
        .phy_reset_pin = PIN_ETH_PHY_RESET,
        .phy_addr = ETH_PHY_ADDR,
    };
    esp_err_t eth_err = eth_bringup(&eth_cfg);
    if (eth_err == ESP_OK) {
        admin_channel_config_t admin_cfg = {
            .port = ADMIN_UDP_PORT,
        };
        admin_channel_start(&admin_cfg);
    } else {
        ESP_LOGE(TAG, "Ethernet bring-up failed (%s)", esp_err_to_name(eth_err));
    }
}
