/** @file hv9910.h
 * @brief HV9910 LED driver control.
 *
 * This component only drives the LED -- it never touches NVS. The
 * luminaire always powers up with the LED off; the on-level comes from
 * the network (DMX or the admin channel). If a bare ON arrives with no
 * level, the driver comes up at the last level commanded THIS boot
 * (volatile, RAM only), or at 100% if nothing has set a level yet.
 *
 * Two hardware signals, both PWM outputs:
 *   - LD   (GPIO to an RC/DAC on the PCB -> HV9910 linear-dimming input):
 *     analog current reference. Flicker-free; inaccurate below ~10%; never
 *     reaches true zero.
 *   - PWMD (logic line -> HV9910 digital-dimming input): gates the driver.
 *     Duty 0 is the real "off". Chopping it gives linear photometry and a
 *     stable colour temperature.
 *
 * Three runtime-selectable dimming modes (see hv9910_curve.h): PWM,
 * ANALOG, HYBRID. The mode and its parameters are supplied by another
 * module via hv9910_set_dimming() -- this component keeps nothing on
 * flash. Reaching level 0 in any mode drives PWMD to 0 (an RC-filtered LD
 * alone cannot extinguish the output).
 *
 * There is no fade/ramp engine: every level change is applied at once.
 * Smooth transitions are the job of whatever is driving (a DMX console,
 * the DMX layer). The TPS2378 power gate is the caller's job -- nothing
 * here checks it.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "hv9910_curve.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief HV9910 board wiring (static; set once at hv9910_init()). */
typedef struct {
    gpio_num_t pwmd_pin;   /**< HV9910 PWMD (digital dimming) GPIO. */
    gpio_num_t ld_pin;     /**< HV9910 LD (linear dimming, via RC) GPIO. */
    bool pwmd_invert;      /**< LEDC output_invert for PWMD. */
    bool ld_invert;        /**< LEDC output_invert for LD. */
} hv9910_config_t;

/** @brief Runtime dimming configuration (from driver_config; volatile here). */
typedef struct {
    uint8_t  mode;           /**< hv9910_dim_mode_t. */
    uint16_t pwm_freq_hz;    /**< PWMD switching frequency, 1000..5000. */
    uint32_t analog_freq_hz; /**< LD (RC-fed) PWM frequency, 40000..80000. */
    uint16_t min_on_time_us; /**< PWMD minimum conduction burst, 2..200. */
    uint8_t  crossover_pct;  /**< HYBRID knee, 10..60. */
    uint8_t  lin_enable;     /**< 0/1: linearize the LD (analog) output-power transfer. */
} hv9910_dimming_t;

/**
 * @brief Initializes the driver in the disabled state (LED off).
 * @param config Board wiring.
 * @return None.
 */
void hv9910_init(const hv9910_config_t *config);

/**
 * @brief Selects the dimming mode and its parameters. Reconfigures the
 * LEDC timers and re-renders the current level under the new curve. Safe
 * to call repeatedly; a brief (sub-ms) output glitch during the LEDC
 * reconfigure is accepted.
 * @param p Mode + frequencies + min-on-time + crossover. Copied.
 * @return None.
 */
void hv9910_set_dimming(const hv9910_dimming_t *p);

/**
 * @brief Caps the fixture's output power, applied under every level and every
 * dimming mode. Used by the power manager for the Type-1 PoE budget: the
 * commanded 0-100 scale is unchanged for the user, the ceiling drops. It
 * scales the commanded level before the dimming curve, so with output-power
 * linearization enabled the cap is an honest "% of max power"; PWMD keeps
 * its full range so dimming depth is preserved. 100 = no cap.
 * @param percent Power cap, 1-100 (clamped).
 * @return None.
 */
void hv9910_set_output_scale(uint8_t percent);

/**
 * @brief Gets the current output power cap.
 * @return Cap in percent, 1-100 (100 = no cap).
 */
uint8_t hv9910_get_output_scale_pct(void);

/**
 * @brief Enables the driver at the last level commanded this boot, or at
 * 100% if no level has been set yet.
 * @return None.
 */
void hv9910_enable(void);

/**
 * @brief Enables the driver at an explicit brightness.
 * @param percent Target brightness, 0-100.
 * @return None.
 */
void hv9910_enable_at(uint8_t percent);

/**
 * @brief Disables the driver (PWMD to 0).
 * @return None.
 */
void hv9910_disable(void);

/**
 * @brief Disables the driver immediately, with priority over queued
 * commands. Does NOT change the "should be on" desire, so the driver
 * comes back on by itself once power is reconfirmed (see
 * hv9910_pending_on()).
 * @return None.
 */
void hv9910_emergency_disable(void);

/**
 * @brief Reports the network's last expressed on/off desire (volatile;
 * false at boot, set by enable/disable/dim, NOT cleared by
 * hv9910_emergency_disable()).
 * @return true if the driver should be on once power allows.
 */
bool hv9910_pending_on(void);

/**
 * @brief Reports whether the LED is currently lit.
 * @return true if the LED is on.
 */
bool hv9910_is_enabled(void);

/**
 * @brief Sets the LED brightness (never touches NVS). Applied at once. A
 * nonzero value is also remembered as the volatile "last level" for a
 * later bare hv9910_enable(). Does NOT touch the network's on/off desire
 * (hv9910_pending_on()).
 * @param percent Brightness, 0-100.
 * @return None.
 */
void hv9910_set_dim(uint8_t percent);

/**
 * @brief Runs the visual identify blink sequence.
 * @return None.
 */
void hv9910_identify(void);

/**
 * @brief Records the desired on/off state (and, optionally, a level to
 * remember) without touching hardware -- used when a command arrives
 * before power is confirmed, so the driver comes up (or stays down) once
 * it is.
 * @param on Desired state.
 * @param remember_pct If nonzero, also becomes the volatile "last level"
 * a later hv9910_enable() ramps to; 0 leaves it unchanged.
 * @return None.
 */
void hv9910_set_pending(bool on, uint8_t remember_pct);

/**
 * @brief Gets the currently applied brightness.
 * @return Brightness, 0-100.
 */
uint8_t hv9910_get_dim_percent(void);

/**
 * @brief Gets the last nonzero brightness commanded this boot (volatile).
 * @return Brightness 1-100, or 0 if none has been set yet.
 */
uint8_t hv9910_get_last_nonzero_percent(void);

#ifdef __cplusplus
}
#endif
