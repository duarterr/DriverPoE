/** @file hv9910.h
 * @brief HV9910 LED driver control.
 *
 * This component only drives the LED -- it never touches NVS. The
 * luminaire always powers up with the LED off; the on-level comes from
 * the network (DMX or the admin channel). If a bare ON arrives with no
 * level, the driver comes up at the last level commanded THIS boot
 * (volatile, RAM only), or at 100% if nothing has set a level yet.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief HV9910 driver configuration. */
typedef struct {
    gpio_num_t shutdown_pin;   /**< Enable GPIO. */
    gpio_num_t dimming_pin;    /**< Dimming PWM GPIO. */
    bool shutdown_active_high; /**< Enable pin polarity. */
    bool dim_active_high;      /**< PWM pin polarity. */
    uint32_t pwm_freq_hz;      /**< PWM frequency. */
    uint32_t max_ramp_ms;      /**< Ramp duration ceiling, in ms. */
} hv9910_config_t;

/**
 * @brief Initializes the driver in the disabled state (LED off).
 * @param config GPIOs, polarities, and limits.
 * @return None.
 */
void hv9910_init(const hv9910_config_t *config);

/**
 * @brief Enables the driver at the last level commanded this boot, or at
 * 100% if no level has been set yet.
 * @param ramp_ms Ramp duration in ms.
 * @return None.
 */
void hv9910_enable(uint32_t ramp_ms);

/**
 * @brief Enables the driver, ramping to an explicit brightness.
 * @param percent Target brightness, 0-100.
 * @param ramp_ms Ramp duration in ms.
 * @return None.
 */
void hv9910_enable_at(uint8_t percent, uint32_t ramp_ms);

/**
 * @brief Disables the driver after ramping the brightness down.
 * @param ramp_ms Ramp duration in ms.
 * @return None.
 */
void hv9910_disable(uint32_t ramp_ms);

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
 * @brief Reports whether the driver is currently enabled.
 * @return true if the driver is on.
 */
bool hv9910_is_enabled(void);

/**
 * @brief Sets the LED brightness (never touches NVS). A nonzero value is
 * also remembered as the volatile "last level" for a later bare
 * hv9910_enable(). Does not itself release the SHUTDOWN gate -- pair with
 * hv9910_enable()/hv9910_enable_at() to actually light the LED.
 * @param percent Brightness, 0-100.
 * @param ramp_ms Ramp duration in ms.
 * @return None.
 */
void hv9910_set_dim(uint8_t percent, uint32_t ramp_ms);

/**
 * @brief Runs the visual identify blink sequence.
 * @return None.
 */
void hv9910_identify(void);

/**
 * @brief Records the desired on/off state without touching hardware --
 * used when a command arrives before power is confirmed, so the driver
 * comes up (or stays down) once it is.
 * @param on Desired state.
 * @return None.
 */
void hv9910_set_pending(bool on);

/**
 * @brief Reports whether a ramp or identify blink is currently in progress.
 * @return true if a deferred action is pending.
 */
bool hv9910_is_ramp_pending(void);

/**
 * @brief Gets the currently requested brightness.
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
