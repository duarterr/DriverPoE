/** @file power_manager.c
 * @brief PoE-class -> power-policy -> HV9910 output decision engine.
 */
#include "power_manager.h"

#include <stdint.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "tps2378.h"
#include "hv9910.h"
#include "driver_config.h"

static const char *TAG = "PWR_MGR";

/* Settle delay before the LED auto-resumes after the first power-ready of a
 * session (matches the old POWERON_SETTLE_MS in main). */
#define SETTLE_MS 2000

#define BUDGET_CW_TYPE1 1295   /* 12.95 W */
#define BUDGET_CW_TYPE2 2550   /* 25.5 W  */

static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_settle_timer;
static bool s_first_ready_seen;
static bool s_started;

static volatile power_manager_state_t s_state = POWER_MANAGER_NO_POWER;
static volatile uint8_t s_scale_pct = 100;
static volatile uint16_t s_budget_cw;

/**
 * @brief Negotiated budget for a source class, in centiwatts (W*100).
 * @param src tps2378 source.
 * @return 0 / 1295 / 2550.
 */
static uint16_t budget_cw_for(tps2378_source_t src)
{
    switch (src) {
    case TPS2378_SOURCE_TYPE1: return BUDGET_CW_TYPE1;
    case TPS2378_SOURCE_TYPE2:
    case TPS2378_SOURCE_AUX:   return BUDGET_CW_TYPE2;
    default:                   return 0;
    }
}

/**
 * @brief Runs the policy decision against the live source + driver_config and
 * pushes the result to the HV9910 driver.
 * @return None.
 */
static void evaluate(void)
{
    tps2378_source_t src = tps2378_get_source();
    bool ready = tps2378_is_ready();

    driver_config_t cfg;
    if (!driver_config_get(&cfg)) {
        return;   /* driver_config not up yet -- a later reeval covers it */
    }

    s_budget_cw = budget_cw_for(src);

    if (!ready || src == TPS2378_SOURCE_NONE) {
        s_state = POWER_MANAGER_NO_POWER;
        s_scale_pct = 100;
        hv9910_emergency_disable();
        return;
    }

    bool full_capable = (src == TPS2378_SOURCE_TYPE2 || src == TPS2378_SOURCE_AUX);
    bool blocked = false;
    bool capped = false;   /* the Type-1-budget restriction is in force (even if poe_cap_pct is 100) */
    uint8_t scale = 100;

    switch (cfg.power_mode) {
    case DRV_POWER_POE_ONLY:
        /* AUX would stay at 100% here, but tps2378 has no APD GPIO yet, so a
         * bench supply reports as TYPE2 and gets capped like a real Type-2
         * PSE -- acceptable for a "force the PoE budget" mode. */
        if (src != TPS2378_SOURCE_AUX) {
            scale = cfg.poe_cap_pct;
            capped = true;
        }
        break;
    case DRV_POWER_POE_PLUS_REQUIRED:
        if (!full_capable) {
            blocked = true;
        }
        break;
    case DRV_POWER_AUTO:
    default:
        if (!full_capable) {
            scale = cfg.poe_cap_pct;
            capped = true;
        }
        break;
    }

    if (blocked) {
        s_state = POWER_MANAGER_BLOCKED_NEEDS_POE_PLUS;
        s_scale_pct = 100;
        hv9910_emergency_disable();
        ESP_LOGW(TAG, "Blocked: PoE+ required but only Type-1 negotiated -- LED held off");
        return;
    }

    s_scale_pct = scale;
    hv9910_set_output_scale(scale);
    /* CAPPED reflects "restricted to the Type-1 budget", not whether the
     * number happens to be < 100 -- an operator who sets poe_cap_pct = 100
     * on a Type-1 fixture is running over budget and should still see it. */
    s_state = capped ? POWER_MANAGER_ACTIVE_CAPPED : POWER_MANAGER_ACTIVE_FULL;
    ESP_LOGI(TAG, "Active: %s, LD scale %u%%%s (%u.%02u W budget)",
             tps2378_source_name(src), (unsigned)scale, capped ? " [Type-1 cap]" : "",
             (unsigned)(s_budget_cw / 100), (unsigned)(s_budget_cw % 100));

    /* Bring the LED up only if the network has asked for it this session. */
    if (!hv9910_pending_on()) {
        return;
    }
    if (!s_first_ready_seen) {
        s_first_ready_seen = true;
        if (s_settle_timer != NULL &&
            esp_timer_start_once(s_settle_timer, (uint64_t)SETTLE_MS * 1000) == ESP_OK) {
            return;   /* the settle callback re-enters evaluate() when it fires */
        }
        ESP_LOGW(TAG, "settle timer unavailable -- bringing the LED up immediately");
    }
    hv9910_enable();
}

/**
 * @brief One-shot settle timer: re-evaluates once the power-on delay elapses.
 * @param arg Unused.
 * @return None.
 */
static void settle_cb(void *arg)
{
    (void)arg;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (tps2378_is_ready()) {
        ESP_LOGI(TAG, "Power-on settle complete");
        evaluate();
    } else {
        ESP_LOGI(TAG, "Power-on settle: power dropped before the delay completed");
    }
    xSemaphoreGive(s_lock);
}

/**
 * @brief Locked wrapper around evaluate() for the callbacks / reeval.
 * @return None.
 */
static void locked_evaluate(void)
{
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    evaluate();
    xSemaphoreGive(s_lock);
}

/* --- tps2378 callbacks (wired from main into tps2378_config_t) ----------- */

void power_manager_tps_ready(tps2378_source_t source, void *ctx)
{
    (void)source;
    (void)ctx;
    locked_evaluate();
}

void power_manager_tps_lost(void *ctx)
{
    (void)ctx;
    if (s_lock == NULL) {
        return;   /* not started yet -- start() runs the first evaluate() */
    }
    /* A power drop ends the session: the next ready re-arms the settle delay. */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_first_ready_seen = false;
    evaluate();
    xSemaphoreGive(s_lock);
}

void power_manager_tps_source_changed(tps2378_source_t source, void *ctx)
{
    (void)source;
    (void)ctx;
    locked_evaluate();
}

void power_manager_start(void)
{
    if (s_started) {
        return;
    }
    s_lock = xSemaphoreCreateMutex();

    const esp_timer_create_args_t args = { .callback = settle_cb, .name = "pwr_settle" };
    if (esp_timer_create(&args, &s_settle_timer) != ESP_OK) {
        ESP_LOGW(TAG, "esp_timer_create failed -- power-on settle delay disabled");
        s_settle_timer = NULL;
    }

    s_started = true;
    locked_evaluate();
}

void power_manager_reeval(void)
{
    locked_evaluate();
}

power_manager_state_t power_manager_get_state(void)
{
    return s_state;
}

uint8_t power_manager_effective_scale_pct(void)
{
    return s_scale_pct;
}

uint16_t power_manager_budget_cw(void)
{
    return s_budget_cw;
}

bool power_manager_indicator_ok(void)
{
    return s_state == POWER_MANAGER_ACTIVE_FULL || s_state == POWER_MANAGER_ACTIVE_CAPPED;
}

bool power_manager_output_allowed(void)
{
    return s_state != POWER_MANAGER_BLOCKED_NEEDS_POE_PLUS;
}
