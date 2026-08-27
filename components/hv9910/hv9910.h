/** @file hv9910.h
 * @brief HV9910 LED driver control.
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
 * @brief Initializes the driver in the disabled state.
 * @param config GPIOs, polarities, and limits.
 * @return None.
 */
void hv9910_init(const hv9910_config_t *config);

/**
 * @brief Enables the driver and restores the last brightness level.
 * @param ramp_ms Ramp duration in ms.
 * @param persist true to persist the enabled state.
 * @return None.
 */
void hv9910_enable(uint32_t ramp_ms, bool persist);

/**
 * @brief Disables the driver after ramping the brightness down.
 * @param ramp_ms Ramp duration in ms.
 * @param persist true to persist the disabled state.
 * @return None.
 */
void hv9910_disable(uint32_t ramp_ms, bool persist);

/**
 * @brief Disables the driver immediately, with priority over queued commands.
 * @return None.
 */
void hv9910_emergency_disable(void);

/**
 * @brief Reports the last persisted enabled/disabled state.
 * @return true if the persisted state is enabled.
 */
bool hv9910_was_last_on(void);

/**
 * @brief Reports whether the driver is currently enabled.
 * @return true if the driver is on.
 */
bool hv9910_is_enabled(void);

/**
 * @brief Sets the LED brightness.
 * @param percent Brightness, 0-100.
 * @param ramp_ms Ramp duration in ms.
 * @param persist true to remember this as the resume brightness (NVS write);
 * false for a transient/cosmetic dim (an effect frame, a music-reactive
 * update) that shouldn't touch flash at all.
 * @return None.
 */
void hv9910_set_dim(uint8_t percent, uint32_t ramp_ms, bool persist);

/**
 * @brief Runs the visual identify blink sequence.
 * @return None.
 */
void hv9910_identify(void);

/**
 * @brief Persists the desired enabled/disabled intent without touching hardware.
 * @param on Desired intent; does not change the current physical state.
 * @return None.
 */
void hv9910_persist_intent(bool on);

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
 * @brief Gets the last persisted non-zero brightness.
 * @return Brightness, 1-100.
 */
uint8_t hv9910_get_last_nonzero_percent(void);

#ifdef __cplusplus
}
#endif
