/*
 * HV9910 LED driver control (SHUTDOWN + DIMMING/PWM).
 *
 * POLARITY — confirmed on the bench (see poe_luminaire.h for the details and
 * the date), adjustable there if a different revision of the board turns
 * out to differ:
 *
 *  - SHUTDOWN (GPIO32, direct/non-inverted logic): a HIGH level on the GPIO
 *    = the HV9910's SHUTDOWN/enable pin HIGH = driver ENABLED; LOW =
 *    driver disabled. Set via poe_luminaire.h (HV9910_SHUTDOWN_ACTIVE_HIGH=0).
 *  - DIMMING (GPIO33, inverted logic): there's an inverting stage
 *    between the GPIO and the HV9910's PWMD pin. The LEDC driver is
 *    configured with flags.output_invert=1, so the duty value used by
 *    this API always represents the desired logical brightness
 *    (0=off, 100=max brightness), regardless of the electrical inversion.
 *    Set via poe_luminaire.h (HV9910_DIM_ACTIVE_HIGH=1).
 *
 * Safety strategy: hv9910_init() is the FIRST thing called in app_main(),
 * before any other initialization (network, ADC, etc), and it already
 * leaves the driver disabled. Every caller in this firmware decides both
 * WHETHER and WHEN to turn the driver on — nothing in this module does so
 * on its own initiative (see poe_negotiator.c for the one place that
 * decides "power is ready, check if it should come back on").
 *
 * Small API, three functions:
 *   - hv9910_enable(ramp_ms, persist) / hv9910_disable(ramp_ms, persist) --
 *     the only two ON/OFF actions. Both ramp (0 = instant). 'persist'
 *     controls whether the new on/off state is written to NVS (key "on"):
 *     pass true for genuine operator intent (ON, OFF, DIM turning the
 *     driver on/off) that should survive a reboot/power cycle; pass false
 *     for transient/internal changes that must NOT redefine "what the
 *     operator wants" -- the PoE-loss safety cutoff and the automatic
 *     resume in poe_negotiator.c, and IDENTIFY's temporary blink.
 *   - hv9910_set_dim(percent, ramp_ms) -- the only way to change
 *     brightness. Never touches SHUTDOWN by itself.
 *
 * Brightness + on/off persistence: the last non-zero brightness (NVS key
 * "dim") and the last on/off state set with persist=true (NVS key "on"),
 * both in namespace "hv9910", are saved every time either changes, and
 * reloaded at boot into hv9910_get_last_nonzero_percent() /
 * hv9910_was_last_on(). hv9910_enable() always reapplies the remembered
 * brightness, so turning the driver on — via ON, DIM > 0 on a
 * currently-off driver, or the automatic resume in poe_negotiator.c —
 * comes back at the last level instead of dark. A fresh device with
 * nothing ever saved defaults to 100% brightness and OFF (never
 * auto-resumes on its own on a brand new unit).
 *
 * Ramped transitions use the LEDC peripheral's own hardware fade
 * (ledc_set_fade_with_time()/ledc_fade_start(), no software polling task)
 * instead of snapping instantly.
 *
 * IMPORTANT hardware limitation: classic ESP32's LEDC cannot cancel or
 * retarget a fade that's already running (no ledc_fade_stop() on this
 * SoC) -- starting a second fade on the same channel before the first one
 * finishes just blocks until it does, and by then the duty already
 * reached the FIRST fade's target, silently defeating whatever the
 * second call actually asked for. This is why cmd_server.c's DIM command
 * turning the driver on from off calls hv9910_enable(0, ...) (ramp_ms=0,
 * so it takes the instant, non-fade code path and never touches the fade
 * hardware) before its own hv9910_set_dim(percent, ramp_ms) call -- that
 * way exactly one fade ever starts, using the ramp actually requested.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Configures the SHUTDOWN and DIMMING GPIOs, guarantees the driver is
 * OFF, and loads the last saved brightness/on-off state from NVS
 * (without applying anything to hardware yet — that only happens on
 * hv9910_enable()). Must be the first function called in app_main(). */
void hv9910_init(void);

/* Releases SHUTDOWN and ramps to the last remembered non-zero brightness
 * (100% if none was ever set) over 'ramp_ms' milliseconds (0 = instant).
 * If 'persist' is true, also writes "on" to NVS (key "on") as the new
 * remembered on/off state -- pass false for transient/internal turn-ons
 * that must not redefine operator intent (see the file header). */
void hv9910_enable(uint32_t ramp_ms, bool persist);

/* Ramps the brightness down to 0 over 'ramp_ms' milliseconds (0 =
 * instant), then asserts SHUTDOWN. If 'persist' is true, also writes
 * "off" to NVS as the new remembered on/off state -- pass false for the
 * PoE-loss safety cutoff and other transient/internal turn-offs (see the
 * file header). Safe to call from any task context. */
void hv9910_disable(uint32_t ramp_ms, bool persist);

/* true if the driver was last turned on/off with persist=true (persisted
 * across reboots via NVS, key "on") -- a fresh device with nothing ever
 * saved reads false. poe_negotiator.c checks this the moment PoE/AUX/VBUS
 * becomes ready and calls hv9910_enable() if true, so a unit that was on
 * when it lost power (or rebooted) comes back on by itself once power is
 * safe again -- but a unit that was off stays off. */
bool hv9910_was_last_on(void);

/* true if the driver is currently enabled (SHUTDOWN released). */
bool hv9910_is_enabled(void);

/* Sets the logical brightness from 0 (off) to 100 (max), fading to it
 * smoothly over 'ramp_ms' milliseconds (0 = instant, no fade). The single
 * function for every brightness change in the firmware — nothing else
 * touches the PWM duty directly. Doesn't turn SHUTDOWN on/off by itself —
 * that's hv9910_enable()/hv9910_disable()'s job. Non-zero values are
 * persisted to NVS as the new "remembered" brightness for future
 * hv9910_enable() calls, regardless of ramp time. */
void hv9910_set_dim(uint8_t percent, uint32_t ramp_ms);

/* Last dimming value that was set (0-100). Can be 0 while the driver is
 * disabled — see hv9910_get_last_nonzero_percent() for the remembered
 * resume level. */
uint8_t hv9910_get_dim_percent(void);

/* The remembered non-zero brightness (what hv9910_enable() will restore),
 * persisted across reboots via NVS. */
uint8_t hv9910_get_last_nonzero_percent(void);

#ifdef __cplusplus
}
#endif
