/*
 * HV9910 LED driver control (SHUTDOWN + DIMMING/PWM).
 *
 * POLARITY — confirmed on the bench (see board_pins.h for the details and
 * the date), adjustable there if a different revision of the board turns
 * out to differ:
 *
 *  - SHUTDOWN (GPIO32, direct/non-inverted logic): a HIGH level on the GPIO
 *    = the HV9910's SHUTDOWN/enable pin HIGH = driver ENABLED; LOW =
 *    driver disabled. Set via board_pins.h (HV9910_SHUTDOWN_ACTIVE_HIGH=0).
 *  - DIMMING (GPIO33, inverted logic): there's an inverting stage
 *    between the GPIO and the HV9910's PWMD pin. The LEDC driver is
 *    configured with flags.output_invert=1, so the duty value used by
 *    this API always represents the desired logical brightness
 *    (0=off, 100=max brightness), regardless of the electrical inversion.
 *    Set via board_pins.h (HV9910_DIM_ACTIVE_HIGH=1).
 *
 * Safety strategy: hv9910_init() is the FIRST thing called in app_main(),
 * before any other initialization (network, ADC, etc), and it already
 * leaves the driver disabled. No other function in this module turns the
 * driver on by itself — that decision belongs to the caller (the PoE
 * negotiation layer for the initial gate, the command server for ON/DIM).
 *
 * Brightness persistence: the last non-zero brightness is saved to NVS
 * (namespace "hv9910", key "dim") every time it changes, and reloaded at
 * boot. hv9910_enable() always reapplies that remembered brightness, so
 * turning the driver on — whether via the ON command or via a DIM > 0
 * command on a currently-off driver — resumes the last level instead of
 * coming up dark. A fresh device with nothing ever saved defaults to
 * 100%.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Configures the SHUTDOWN and DIMMING GPIOs, guarantees the driver is
 * OFF, and loads the last saved brightness from NVS (without applying it
 * to hardware yet — that only happens on hv9910_enable()). Must be the
 * first function called in app_main(). */
void hv9910_init(void);

/* Disables the driver immediately (asserts SHUTDOWN and zeroes the PWM
 * duty). Safe to call from any task context, including in response to a
 * loss of PoE negotiation (CDB watchdog). Does not touch the remembered
 * brightness. */
void hv9910_disable(void);

/* Releases SHUTDOWN (driver starts running) and reapplies the last
 * remembered non-zero brightness (100% if none was ever set). */
void hv9910_enable(void);

/* true if the driver is currently enabled (SHUTDOWN released). */
bool hv9910_is_enabled(void);

/* Sets the logical brightness from 0 (off) to 100 (max) and applies it to
 * hardware. Doesn't turn SHUTDOWN on/off by itself — that policy (e.g.
 * "DIM 0 turns the driver off", "DIM > 0 turns it on") lives in the
 * caller (see cmd_server.c). Non-zero values are persisted to NVS as the
 * new "remembered" brightness for future hv9910_enable() calls. */
void hv9910_set_dim_percent(uint8_t percent);

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
