#include "hv9910.h"
#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "HV9910";

#define LEDC_SPEED_MODE   LEDC_LOW_SPEED_MODE
#define LEDC_TIMER        LEDC_TIMER_0
#define LEDC_CHANNEL      LEDC_CHANNEL_0
#define LEDC_DUTY_RES     LEDC_TIMER_10_BIT
#define LEDC_DUTY_MAX     ((1 << 10) - 1)

#define NVS_NAMESPACE     "hv9910"
#define NVS_KEY_DIM       "dim"
#define DEFAULT_RESTORE_PERCENT   100  /* used the very first time, if nothing was ever saved */

#if HV9910_SHUTDOWN_ACTIVE_HIGH
#define SHUTDOWN_ASSERT_LEVEL   1
#else
#define SHUTDOWN_ASSERT_LEVEL   0
#endif
#define SHUTDOWN_RUN_LEVEL       (!SHUTDOWN_ASSERT_LEVEL)

static bool s_enabled = false;
static uint8_t s_dim_percent = 0;          /* current live brightness, can be 0 while off */
static uint8_t s_last_nonzero_percent = 0; /* remembered resume level, persisted to NVS */
static bool s_ledc_started = false;
static nvs_handle_t s_nvs_handle;
static bool s_nvs_ok = false;

static void shutdown_write(int level)
{
    gpio_set_level(PIN_HV9910_SHUTDOWN, level);
}

static void nvs_init_and_load(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* NVS partition layout changed or is corrupt: erase and retry once. */
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS init failed (%s) — brightness won't persist across reboots", esp_err_to_name(err));
        return;
    }

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed (%s) — brightness won't persist across reboots", esp_err_to_name(err));
        return;
    }
    s_nvs_ok = true;

    uint8_t saved = 0;
    err = nvs_get_u8(s_nvs_handle, NVS_KEY_DIM, &saved);
    if (err == ESP_OK && saved > 0 && saved <= 100) {
        s_last_nonzero_percent = saved;
        ESP_LOGI(TAG, "Restored last brightness from NVS: %u%%", saved);
    } else {
        s_last_nonzero_percent = DEFAULT_RESTORE_PERCENT;
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "NVS read of '%s' failed (%s) — defaulting to %u%%",
                     NVS_KEY_DIM, esp_err_to_name(err), DEFAULT_RESTORE_PERCENT);
        }
    }
}

/* Persists 'percent' as the new remembered resume level, but only if it
 * actually differs from what's already stored — avoids a flash write on
 * every single hv9910_enable() re-applying the same value. */
static void persist_last_nonzero(uint8_t percent)
{
    if (percent == 0 || percent == s_last_nonzero_percent) {
        return;
    }
    s_last_nonzero_percent = percent;
    if (!s_nvs_ok) {
        return;
    }
    esp_err_t err = nvs_set_u8(s_nvs_handle, NVS_KEY_DIM, percent);
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs_handle);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist brightness to NVS (%s)", esp_err_to_name(err));
    }
}

void hv9910_init(void)
{
    /* --- 1) SHUTDOWN: the very first GPIO configured in the whole firmware. --- */
    gpio_config_t shutdown_cfg = {
        .pin_bit_mask = 1ULL << PIN_HV9910_SHUTDOWN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&shutdown_cfg);
    shutdown_write(SHUTDOWN_ASSERT_LEVEL); /* guarantee OFF before anything else */

    /* --- 2) DIMMING: while LEDC isn't configured yet, keep the pin as a
     * plain GPIO, forced to the electrical level that corresponds to "0%
     * brightness" on the HV9910 side (remember the logic is externally
     * inverted: 0% logical = ESP32 pin HIGH). This avoids any floating
     * state in the window between boot and LEDC configuration. */
    gpio_config_t dim_cfg = {
        .pin_bit_mask = 1ULL << PIN_HV9910_DIMMING,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&dim_cfg);
#if HV9910_DIM_ACTIVE_HIGH
    gpio_set_level(PIN_HV9910_DIMMING, 1); /* electrical HIGH == 0% logical (inverted) */
#else
    gpio_set_level(PIN_HV9910_DIMMING, 0);
#endif

    s_enabled = false;
    s_dim_percent = 0;
    s_ledc_started = false;

    /* --- 3) Load the remembered brightness from NVS (doesn't touch
     * hardware — the driver stays safely off until hv9910_enable()). --- */
    nvs_init_and_load();

    ESP_LOGI(TAG, "HV9910 initialized in safe mode: SHUTDOWN=%d (assert=%d), DIM=0%% (resume level: %u%%)",
             SHUTDOWN_ASSERT_LEVEL, SHUTDOWN_ASSERT_LEVEL, s_last_nonzero_percent);
}

static void ledc_start_if_needed(void)
{
    if (s_ledc_started) {
        return;
    }

    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_SPEED_MODE,
        .duty_resolution = LEDC_DUTY_RES,
        .timer_num = LEDC_TIMER,
        .freq_hz = HV9910_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    ledc_channel_config_t ch_cfg = {
        .gpio_num = PIN_HV9910_DIMMING,
        .speed_mode = LEDC_SPEED_MODE,
        .channel = LEDC_CHANNEL,
        .timer_sel = LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
        .flags.output_invert = HV9910_DIM_ACTIVE_HIGH ? 1 : 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch_cfg));

    s_ledc_started = true;
}

void hv9910_set_dim_percent(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    s_dim_percent = percent;

    ledc_start_if_needed();

    uint32_t duty = ((uint32_t)percent * LEDC_DUTY_MAX) / 100;
    ledc_set_duty(LEDC_SPEED_MODE, LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_SPEED_MODE, LEDC_CHANNEL);

    persist_last_nonzero(percent);

    ESP_LOGI(TAG, "Dimming set to %u%% (duty=%u/%u)", percent, (unsigned)duty, LEDC_DUTY_MAX);
}

uint8_t hv9910_get_dim_percent(void)
{
    return s_dim_percent;
}

uint8_t hv9910_get_last_nonzero_percent(void)
{
    return s_last_nonzero_percent;
}

void hv9910_enable(void)
{
    uint8_t restore_percent = (s_last_nonzero_percent > 0) ? s_last_nonzero_percent : DEFAULT_RESTORE_PERCENT;
    hv9910_set_dim_percent(restore_percent);

    shutdown_write(SHUTDOWN_RUN_LEVEL);
    s_enabled = true;
    ESP_LOGW(TAG, "Driver ENABLED (SHUTDOWN released), resumed at %u%%", restore_percent);
}

void hv9910_disable(void)
{
    shutdown_write(SHUTDOWN_ASSERT_LEVEL);
    s_enabled = false;
    /* also zero the duty as a second layer of safety */
    if (s_ledc_started) {
        ledc_set_duty(LEDC_SPEED_MODE, LEDC_CHANNEL, 0);
        ledc_update_duty(LEDC_SPEED_MODE, LEDC_CHANNEL);
    }
    s_dim_percent = 0;
    ESP_LOGI(TAG, "Driver DISABLED (SHUTDOWN asserted)");
}

bool hv9910_is_enabled(void)
{
    return s_enabled;
}
