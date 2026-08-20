/*
 * Monitors the TPS2378 PoE negotiator through the CDB and T2P pins, cross
 * -checks it against the actual measured DC bus voltage (VBUS), and
 * decides whether it's safe to leave low-power mode (i.e. allow the
 * HV9910 LED driver to be turned on and the rest of the application to
 * run normally).
 *
 * "Ready" requires BOTH of the following:
 *
 *  1) A digital source confirmed — EITHER of:
 *     - PoE OK: CDB (open-drain, active-low) stays LOW during "inrush
 *       current limiting" (bulk capacitor charging) and rises to HIGH
 *       once the inrush current falls ~10% below the limit — the point
 *       where it's safe to enable the downstream conversion/load. This is
 *       true for a real PoE source of EITHER class (Type-1/802.3af or
 *       Type-2/802.3at).
 *     - AUX present: this board has an auxiliary DC input that can power
 *       the downstream DC bus directly (bench testing, bypassing PoE
 *       entirely). A resistor divider feeds the TPS2378's APD pin high
 *       whenever that AUX voltage is above ~40V, which per the TPS2378
 *       datasheet forces T2P low (T2P is pulled to RTN "whenever type-2
 *       hardware classification has been observed OR the APD pin is
 *       pulled high"). So T2P asserted means either genuine Type-2
 *       classification or the AUX supply being present — the two can't be
 *       told apart from the pin state alone, which is fine: either one
 *       means it's safe to operate.
 *
 *  2) VBUS actually backs it up: the measured DC bus voltage (via
 *     voltage_sense.h, VBUS_MIN_MV in poe_luminaire.h) is above the
 *     configured minimum (default 40V, matching the AUX divider's own
 *     threshold). CDB/T2P are digital handshake signals — they can claim
 *     power is present even if the actual rail is sagging, shorted, or
 *     otherwise unhealthy, so VBUS is the final cross-check before the
 *     LED driver is allowed to run. voltage_sense_init() must be called
 *     before poe_negotiator_init() for this to work.
 *
 * None of CDB/T2P have an external pull-up resistor on this board — both
 * are pulled up by the ESP32's internal pull-up (enabled in
 * poe_negotiator_init()), so a genuinely unpowered TPS2378 leaves both
 * pins floating HIGH-ish/noisy rather than driven. That's why all three
 * signals (CDB, T2P, VBUS-above-threshold) are debounced independently
 * and continuously re-evaluated, instead of being latched once.
 *
 * This module runs a continuous monitoring task that:
 *  - Blinks indicator LED 2 (red) while in low-power mode (condition 1
 *    and/or 2 above not satisfied).
 *  - Cuts the HV9910 (hv9910_disable(0, false)) IMMEDIATELY if the
 *    combined verdict drops again (power loss, renegotiation, AUX
 *    removed, VBUS sagging below threshold, thermal overload, etc), even
 *    after the driver was already released once. persist=false on
 *    purpose: it doesn't erase the remembered on/off state, so if the
 *    verdict comes back to "ready" later (power restored), the ready
 *    branch checks hv9910_was_last_on() and calls hv9910_enable() again,
 *    bringing the driver back on by itself if it was on before the drop.
 *  - Logs an "EVENT: CDB ..." / "EVENT: T2P ..." / "EVENT: VBUS ..." line
 *    every time ANY individual signal's own debounced state changes,
 *    independently of whether that flips the combined ready/not-ready
 *    verdict (e.g. real PoE can be lost while AUX is still holding the
 *    system up, and that's still worth a log line). This is the hook to
 *    build "shut down anything non-essential" logic on top of later,
 *    if/when needed.
 *  - Logs a "READY: ..." / "LOW POWER MODE: ..." line on every combined
 *    verdict transition, plus a periodic reminder while stuck in
 *    low-power mode, so it's obvious from the console what's happening.
 */
#pragma once

#include <stdbool.h>
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    POE_SOURCE_NONE = 0,  /* low-power mode: no digital source confirmed, or VBUS too low */
    POE_SOURCE_TYPE1,      /* real PoE negotiated (CDB), T2P not asserted: IEEE 802.3af */
    POE_SOURCE_TYPE2,      /* real PoE negotiated (CDB) AND T2P asserted: IEEE 802.3at Type-2 (or Type-1 with AUX also present — can't be told apart) */
    POE_SOURCE_AUX,        /* T2P asserted (AUX >40V forcing APD) but CDB does NOT confirm real PoE: bench AUX supply, not a real PoE budget */
} poe_source_t;

/* Configures the GPIOs (CDB, T2P, wait indicator LED) and starts the
 * monitoring task. Call after hv9910_init() and voltage_sense_init(). */
void poe_negotiator_init(void);

/* Blocks until the combined condition is confirmed (debounced) or the
 * timeout expires. Use portMAX_DELAY to wait indefinitely. */
bool poe_negotiator_wait_ready(TickType_t timeout);

/* Current (debounced) readiness: true = (PoE OK or AUX present) AND VBUS
 * above VBUS_MIN_MV — safe to run normally. false = low-power mode. */
bool poe_negotiator_is_ready(void);

/* Which digital condition(s) are currently satisfied, independently of
 * VBUS. POE_SOURCE_NONE whenever neither CDB nor T2P is confirmed, OR
 * whenever VBUS is below threshold (see poe_negotiator_vbus_ok() to tell
 * the two apart). */
poe_source_t poe_negotiator_get_source(void);

/* Debounced (confirmed) state of each individual signal — what the
 * combined ready/not-ready decision and the "EVENT: ..." log lines are
 * based on. true = CDB confirms real PoE / T2P confirms AUX or Type-2 /
 * VBUS is above VBUS_MIN_MV. */
bool poe_negotiator_cdb_confirmed(void);
bool poe_negotiator_t2p_confirmed(void);
bool poe_negotiator_vbus_confirmed(void);

/* Raw, un-debounced instantaneous pin/ADC reads — same interpretation as
 * the confirmed getters above, but taken fresh every call instead of
 * relying on the monitoring task's debounce window. Useful to see
 * glitching that hasn't (yet) survived the debounce. Safe to call from
 * any task. */
bool poe_negotiator_cdb_raw(void);
bool poe_negotiator_t2p_raw(void);
bool poe_negotiator_vbus_raw(void);

/* Last measured VBUS, in mV (updated every monitoring cycle, ~20ms). */
int poe_negotiator_get_vbus_mv(void);

/* Human-readable name for a poe_source_t value, for log messages. */
const char *poe_negotiator_source_name(poe_source_t source);

/* Compact, single-token, space-free name for a poe_source_t value
 * ("none"/"type1"/"type2"/"aux"), for the STATUS wire protocol. */
const char *poe_negotiator_source_short_name(poe_source_t source);

/* Guaranteed PD power for the current source, in watts: 12.95W for
 * TYPE1 (802.3af), 25.5W for TYPE2 (802.3at/PoE+) — the standard IEEE
 * headline figures, not a live measurement. Returns 0.0f for
 * POE_SOURCE_AUX (bench power, not a standardized PoE budget) and for
 * POE_SOURCE_NONE. */
float poe_negotiator_get_available_power_w(void);

#ifdef __cplusplus
}
#endif
