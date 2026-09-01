/** @file driver_config.c
 * @brief HV9910 dimming config: NVS blob + wire (de)serialization +
 * validation, applied to the hv9910 driver. Mirrors the config block of
 * components/dmx_input/dmx_input.c.
 */
#include "driver_config.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "hv9910.h"
#include "hv9910_curve.h"

static const char *TAG = "DRV_CFG";

#define NVS_NAMESPACE "driver"
#define NVS_KEY_CFG   "cfg"

static SemaphoreHandle_t s_lock;
static driver_config_t s_cfg;
static bool s_started;
static nvs_handle_t s_nvs;
static bool s_nvs_ok;

/**
 * @brief Fills a config with the factory defaults (HYBRID mode).
 * @param c Destination.
 * @return None.
 */
static void config_defaults(driver_config_t *c)
{
    memset(c, 0, sizeof(*c));
    c->mode = HV9910_DIM_HYBRID;
    c->pwm_freq_hz = 2000;
    c->analog_freq_hz = 60000;
    c->min_on_time_us = 20;
    c->crossover_pct = 20;
    c->power_mode = DRV_POWER_AUTO;
    c->poe_cap_pct = 51;   /* 12.95 W / 25.5 W */
}

/**
 * @brief Clamps a config to valid ranges.
 * @param c Config to sanitize in place.
 * @return true if it was already valid.
 */
static bool config_sanitize(driver_config_t *c)
{
    bool ok = true;
    if (c->mode > HV9910_DIM_HYBRID) { c->mode = HV9910_DIM_HYBRID; ok = false; }
    if (c->pwm_freq_hz < 1000)  { c->pwm_freq_hz = 1000;  ok = false; }
    if (c->pwm_freq_hz > 5000)  { c->pwm_freq_hz = 5000;  ok = false; }
    if (c->analog_freq_hz < 40000) { c->analog_freq_hz = 40000; ok = false; }
    if (c->analog_freq_hz > 80000) { c->analog_freq_hz = 80000; ok = false; }
    if (c->min_on_time_us < 2)   { c->min_on_time_us = 2;   ok = false; }
    if (c->min_on_time_us > 200) { c->min_on_time_us = 200; ok = false; }
    if (c->crossover_pct < 10) { c->crossover_pct = 10; ok = false; }
    if (c->crossover_pct > 60) { c->crossover_pct = 60; ok = false; }
    if (c->power_mode > DRV_POWER_POE_PLUS_REQUIRED) { c->power_mode = DRV_POWER_AUTO; ok = false; }
    if (c->poe_cap_pct < 10)  { c->poe_cap_pct = 10;  ok = false; }
    if (c->poe_cap_pct > 100) { c->poe_cap_pct = 100; ok = false; }
    return ok;
}

void driver_config_pack(const driver_config_t *c, uint8_t *o)
{
    o[0]  = DRV_CFG_LAYOUT_VERSION;
    o[1]  = c->mode;
    o[2]  = (uint8_t)(c->pwm_freq_hz >> 8);
    o[3]  = (uint8_t)(c->pwm_freq_hz & 0xff);
    o[4]  = (uint8_t)(c->analog_freq_hz >> 24);
    o[5]  = (uint8_t)(c->analog_freq_hz >> 16);
    o[6]  = (uint8_t)(c->analog_freq_hz >> 8);
    o[7]  = (uint8_t)(c->analog_freq_hz & 0xff);
    o[8]  = (uint8_t)(c->min_on_time_us >> 8);
    o[9]  = (uint8_t)(c->min_on_time_us & 0xff);
    o[10] = c->crossover_pct;
    o[11] = c->power_mode;
    o[12] = c->poe_cap_pct;
}

bool driver_config_unpack(const uint8_t *in, size_t len, driver_config_t *c)
{
    /* v2 is the current layout; v1 (11 bytes, no power fields) is still
     * accepted so an OTA doesn't wipe a fixture's dimming config -- the
     * power fields take their defaults and the blob is rewritten as v2 on
     * the next DRIVER_SET_CONFIG. */
    bool v1 = (len == DRV_CFG_WIRE_SIZE_V1 && in[0] == 1);
    bool v2 = (len == DRV_CFG_WIRE_SIZE && in[0] == DRV_CFG_LAYOUT_VERSION);
    if (!v1 && !v2) {
        return false;
    }
    config_defaults(c);
    c->mode = in[1];
    c->pwm_freq_hz = ((uint16_t)in[2] << 8) | in[3];
    c->analog_freq_hz = ((uint32_t)in[4] << 24) | ((uint32_t)in[5] << 16) |
                        ((uint32_t)in[6] << 8) | in[7];
    c->min_on_time_us = ((uint16_t)in[8] << 8) | in[9];
    c->crossover_pct = in[10];
    if (v2) {
        c->power_mode = in[11];
        c->poe_cap_pct = in[12];
    }
    return true;
}

/**
 * @brief Persists the current config to NVS.
 * @return None.
 */
static void config_save(void)
{
    if (!s_nvs_ok) {
        return;
    }
    uint8_t buf[DRV_CFG_WIRE_SIZE];
    driver_config_pack(&s_cfg, buf);
    esp_err_t err = nvs_set_blob(s_nvs, NVS_KEY_CFG, buf, sizeof(buf));
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist driver config (%s)", esp_err_to_name(err));
    }
}

/**
 * @brief Opens NVS and loads the persisted config, or writes defaults.
 * @return None.
 */
static void config_load(void)
{
    config_defaults(&s_cfg);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS init failed (%s) -- driver config won't persist", esp_err_to_name(err));
        return;
    }
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_nvs) != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed -- driver config won't persist");
        return;
    }
    s_nvs_ok = true;

    uint8_t buf[DRV_CFG_WIRE_SIZE];
    size_t len = sizeof(buf);
    err = nvs_get_blob(s_nvs, NVS_KEY_CFG, buf, &len);
    if (err == ESP_OK && driver_config_unpack(buf, len, &s_cfg)) {
        config_sanitize(&s_cfg);
        ESP_LOGI(TAG, "Driver config loaded: mode %u, pwm %uHz, analog %uHz, min_on %uus, xover %u%%, "
                      "power_mode %u, poe_cap %u%%",
                 (unsigned)s_cfg.mode, (unsigned)s_cfg.pwm_freq_hz, (unsigned)s_cfg.analog_freq_hz,
                 (unsigned)s_cfg.min_on_time_us, (unsigned)s_cfg.crossover_pct,
                 (unsigned)s_cfg.power_mode, (unsigned)s_cfg.poe_cap_pct);
    } else {
        ESP_LOGI(TAG, "No valid driver config in NVS -- writing defaults (HYBRID)");
        config_save();
    }
}

/**
 * @brief Pushes the current config into the hv9910 driver.
 * @return None.
 */
static void apply_to_hv9910(void)
{
    hv9910_dimming_t d = {
        .mode = s_cfg.mode,
        .pwm_freq_hz = s_cfg.pwm_freq_hz,
        .analog_freq_hz = s_cfg.analog_freq_hz,
        .min_on_time_us = s_cfg.min_on_time_us,
        .crossover_pct = s_cfg.crossover_pct,
    };
    hv9910_set_dimming(&d);
}

void driver_config_start(void)
{
    if (s_started) {
        return;
    }
    s_lock = xSemaphoreCreateMutex();
    config_load();
    s_started = true;
    apply_to_hv9910();
}

bool driver_config_get(driver_config_t *out)
{
    if (!s_started) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_lock);
    return true;
}

bool driver_config_set(const driver_config_t *cfg)
{
    if (!s_started) {
        return false;
    }
    driver_config_t next = *cfg;
    config_sanitize(&next);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg = next;
    config_save();
    xSemaphoreGive(s_lock);

    apply_to_hv9910();
    ESP_LOGI(TAG, "Driver config updated: mode %u, pwm %uHz, analog %uHz, min_on %uus, xover %u%%, "
                  "power_mode %u, poe_cap %u%%",
             (unsigned)next.mode, (unsigned)next.pwm_freq_hz, (unsigned)next.analog_freq_hz,
             (unsigned)next.min_on_time_us, (unsigned)next.crossover_pct,
             (unsigned)next.power_mode, (unsigned)next.poe_cap_pct);
    return true;
}
