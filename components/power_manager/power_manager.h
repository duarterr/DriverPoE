/** @file power_manager.h
 * @brief Maps the negotiated PoE class + the configured power policy to the
 * HV9910 output.
 *
 * Owns the source -> policy -> driver decision that used to live inline in
 * main/poe_luminaire_main.c: it registers the tps2378 power callbacks, runs
 * the power-on settle delay, sets the HV9910 LD cap, and gates the LED when
 * the policy is not satisfied (Type-1 PoE under DRV_POWER_POE_PLUS_REQUIRED).
 *
 * The policy fields (power_mode, poe_cap_pct) come from driver_config; call
 * power_manager_reeval() after a DRIVER_SET_CONFIG so a live change takes
 * effect without a reboot.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "tps2378.h"

#ifdef __cplusplus
extern "C" {
#endif

/* tps2378 callbacks -- main wires these into tps2378_config_t so the policy
 * engine sees every power/source transition. They just trigger a re-evaluate;
 * all state is read live. */
void power_manager_tps_ready(tps2378_source_t source, void *ctx);
void power_manager_tps_lost(void *ctx);
void power_manager_tps_source_changed(tps2378_source_t source, void *ctx);

/** @brief Effective power state (surfaced in INFO and on the status LED). */
typedef enum {
    POWER_MANAGER_NO_POWER = 0,            /**< No valid source / VBUS. */
    POWER_MANAGER_ACTIVE_FULL = 1,         /**< Lit, LD at 100% (Type-2 / AUX, or policy allows). */
    POWER_MANAGER_ACTIVE_CAPPED = 2,       /**< Lit, LD capped to poe_cap_pct (Type-1 budget). */
    POWER_MANAGER_BLOCKED_NEEDS_POE_PLUS = 3, /**< Powered on Type-1 but DRV_POWER_POE_PLUS_REQUIRED. */
} power_manager_state_t;

/**
 * @brief Registers the tps2378 callbacks and starts the policy engine.
 * Call once, after tps2378_init(), driver_config_start() and hv9910_init().
 * @return None.
 */
void power_manager_start(void);

/**
 * @brief Re-runs the policy decision against the current source and the
 * current driver_config. Safe to call from any task.
 * @return None.
 */
void power_manager_reeval(void);

/**
 * @brief Current effective power state.
 * @return power_manager_state_t.
 */
power_manager_state_t power_manager_get_state(void);

/**
 * @brief LD scale currently applied to the driver.
 * @return Percent, 1-100 (100 = no cap).
 */
uint8_t power_manager_effective_scale_pct(void);

/**
 * @brief Negotiated power budget for the active source.
 * @return Centiwatts / W*100 (0, 1295 for Type-1, 2550 for Type-2/AUX).
 */
uint16_t power_manager_budget_cw(void);

/**
 * @brief Whether the power indicator LED should be solid-on. False for
 * NO_POWER and for BLOCKED_NEEDS_POE_PLUS, so both blink like today's
 * low-power state. Wire this to status_leds' power_ok_fn.
 * @return true if power is present and the policy is satisfied.
 */
bool power_manager_indicator_ok(void);

/**
 * @brief Whether the driver may be lit at all under the current policy.
 * False only in BLOCKED_NEEDS_POE_PLUS -- admin ON/DIM should then defer
 * (record the desire, answer ACCEPTED_PENDING) instead of lighting the LED,
 * the same way they defer when power isn't confirmed.
 * @return true unless the policy is holding the LED off.
 */
bool power_manager_output_allowed(void);

#ifdef __cplusplus
}
#endif
