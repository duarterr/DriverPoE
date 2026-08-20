#include "hv9910.h"
#include "poe_luminaire.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "HV9910";

#define LEDC_SPEED_MODE   LEDC_LOW_SPEED_MODE
#define LEDC_TIMER        LEDC_TIMER_0
#define LEDC_CHANNEL      LEDC_CHANNEL_0
#define LEDC_DUTY_RES     LEDC_TIMER_10_BIT
#define LEDC_DUTY_MAX     ((1 << 10) - 1)

#define NVS_NAMESPACE     "hv9910"
#define NVS_KEY_DIM       "dim"
#define NVS_KEY_ON        "on"
#define DEFAULT_RESTORE_PERCENT   100  /* used the very first time, if nothing was ever saved */

#define SHUTDOWN_OFF_TASK_STACK   2048

#if HV9910_SHUTDOWN_ACTIVE_HIGH
#define SHUTDOWN_ASSERT_LEVEL   1
#else
#define SHUTDOWN_ASSERT_LEVEL   0
#endif
#define SHUTDOWN_RUN_LEVEL       (!SHUTDOWN_ASSERT_LEVEL)

static bool s_enabled = false;
static uint8_t s_dim_percent = 0;          /* current live brightness, can be 0 while off */
static uint8_t s_last_nonzero_percent = 0; /* remembered resume level, persisted to NVS */
static bool s_last_enabled_persisted = false; /* remembered on/off state, persisted to NVS -- see hv9910_was_last_on() */
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

    uint8_t saved_on = 0;
    err = nvs_get_u8(s_nvs_handle, NVS_KEY_ON, &saved_on);
    if (err == ESP_OK) {
        s_last_enabled_persisted = (saved_on != 0);
        ESP_LOGI(TAG, "Restored last on/off state from NVS: %s", s_last_enabled_persisted ? "ON" : "off");
    } else {
        s_last_enabled_persisted = false; /* a fresh device never auto-resumes on its own */
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "NVS read of '%s' failed (%s) — defaulting to off", NVS_KEY_ON, esp_err_to_name(err));
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

/* Persists 'enabled' as the new remembered on/off state -- see
 * hv9910_enable()/hv9910_disable()/hv9910_was_last_on(). Only writes to
 * NVS if it actually changed. */
static void persist_enabled_state(bool enabled)
{
    if (enabled == s_last_enabled_persisted) {
        return;
    }
    s_last_enabled_persisted = enabled;
    if (!s_nvs_ok) {
        return;
    }
    esp_err_t err = nvs_set_u8(s_nvs_handle, NVS_KEY_ON, enabled ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs_handle);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist on/off state to NVS (%s)", esp_err_to_name(err));
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

    /* --- 3) Load the remembered brightness/on-off state from NVS (doesn't
     * touch hardware — the driver stays safely off until hv9910_enable()). --- */
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

    /* Installs the LEDC fade service (interrupt-driven hardware duty
     * ramp) -- required once before any ledc_set_fade_with_time()/
     * ledc_fade_start() call. Only ever reached once, guarded by
     * s_ledc_started above. */
    ESP_ERROR_CHECK(ledc_fade_func_install(0));

    s_ledc_started = true;
}

void hv9910_set_dim(uint8_t percent, uint32_t ramp_ms)
{
    if (percent > 100) {
        percent = 100;
    }
    s_dim_percent = percent;

    ledc_start_if_needed();

    /* NOTE: classic ESP32's LEDC has no ledc_fade_stop()
     * (SOC_LEDC_SUPPORT_FADE_STOP == 0) -- a fade already in flight
     * cannot be cancelled or retargeted at all. Calling
     * ledc_set_fade_with_time()/ledc_fade_start() again while a previous
     * LEDC_FADE_NO_WAIT fade on this same channel hasn't finished yet
     * BLOCKS this call until that fade completes (it holds a semaphore
     * the whole time), and by then the duty has usually already reached
     * that first fade's target -- so the second fade has nothing left to
     * do and looks like it never happened. Callers must never start two
     * fades back to back on this channel -- see hv9910_enable()'s comment
     * for how it avoids that with ramp_ms=0. */
    uint32_t duty = ((uint32_t)percent * LEDC_DUTY_MAX) / 100;

    if (ramp_ms == 0) {
        ledc_set_duty(LEDC_SPEED_MODE, LEDC_CHANNEL, duty);
        ledc_update_duty(LEDC_SPEED_MODE, LEDC_CHANNEL);
    } else {
        ledc_set_fade_with_time(LEDC_SPEED_MODE, LEDC_CHANNEL, duty, (int)ramp_ms);
        /* NO_WAIT: returns immediately, the fade runs in the background
         * via the LEDC peripheral's own hardware/interrupt-driven duty
         * ramp -- never blocks the calling task for the ramp duration. */
        ledc_fade_start(LEDC_SPEED_MODE, LEDC_CHANNEL, LEDC_FADE_NO_WAIT);
    }

    persist_last_nonzero(percent);

    ESP_LOGI(TAG, "Dimming set to %u%% over %ums (duty=%u/%u)", percent, (unsigned)ramp_ms, (unsigned)duty, LEDC_DUTY_MAX);
}

uint8_t hv9910_get_dim_percent(void)
{
    return s_dim_percent;
}

uint8_t hv9910_get_last_nonzero_percent(void)
{
    return s_last_nonzero_percent;
}

void hv9910_enable(uint32_t ramp_ms, bool persist)
{
    if (persist) {
        persist_enabled_state(true);
    }

    uint8_t restore_percent = (s_last_nonzero_percent > 0) ? s_last_nonzero_percent : DEFAULT_RESTORE_PERCENT;

    /* SHUTDOWN released FIRST, then the fade starts -- ramping the duty
     * before the driver chip is actually running would just mean the fade
     * plays out (in the background) while SHUTDOWN is still asserted and
     * nothing visible happens yet. */
    shutdown_write(SHUTDOWN_RUN_LEVEL);
    s_enabled = true;
    hv9910_set_dim(restore_percent, ramp_ms);
    ESP_LOGW(TAG, "Driver ENABLED (SHUTDOWN released), ramping to %u%% over %ums%s",
             restore_percent, (unsigned)ramp_ms, persist ? "" : " (transient, not persisted)");
}

bool hv9910_was_last_on(void)
{
    return s_last_enabled_persisted;
}

/* Short-lived task: waits for hv9910_disable()'s brightness ramp to
 * actually finish before cutting SHUTDOWN, so a graceful fade-out isn't
 * immediately overridden by an instant hard cutoff a moment after it
 * starts. Parameter is the delay in milliseconds, passed by value through
 * the pointer itself (like cmd_server_start()'s port) -- no heap
 * allocation needed. */
static void shutdown_after_ramp_task(void *arg)
{
    uint32_t delay_ms = (uint32_t)(uintptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    shutdown_write(SHUTDOWN_ASSERT_LEVEL);
    s_enabled = false;
    /* second layer of safety, same as the instant path below */
    if (s_ledc_started) {
        ledc_set_duty(LEDC_SPEED_MODE, LEDC_CHANNEL, 0);
        ledc_update_duty(LEDC_SPEED_MODE, LEDC_CHANNEL);
    }
    ESP_LOGI(TAG, "Driver DISABLED (SHUTDOWN asserted, after ramp)");
    vTaskDelete(NULL);
}

void hv9910_disable(uint32_t ramp_ms, bool persist)
{
    if (persist) {
        persist_enabled_state(false);
    }

    /* Ramp the brightness down first (or snap to 0 instantly if
     * ramp_ms==0) -- SHUTDOWN is only asserted once the driver is
     * actually visually off, so a ramped OFF looks like a fade, not an
     * abrupt cut. The PoE-loss safety cutoff always passes ramp_ms=0 (see
     * poe_negotiator.c) -- an emergency cutoff has no business waiting
     * around for a fade to finish. */
    hv9910_set_dim(0, ramp_ms);

    if (ramp_ms == 0) {
        shutdown_write(SHUTDOWN_ASSERT_LEVEL);
        s_enabled = false;
        ESP_LOGI(TAG, "Driver DISABLED (SHUTDOWN asserted)");
    } else {
        xTaskCreate(shutdown_after_ramp_task, "hv_off", SHUTDOWN_OFF_TASK_STACK,
                    (void *)(uintptr_t)ramp_ms, tskIDLE_PRIORITY + 2, NULL);
    }
}

bool hv9910_is_enabled(void)
{
    return s_enabled;
}
