/** @file status_leds.h
 * @brief Power and driver-state indicator LEDs.
 */
#pragma once

#include <stdbool.h>
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief GPIOs, polarity, and status source callbacks for the indicator LEDs. */
typedef struct {
    gpio_num_t blue_pin;         /**< Power indicator LED. */
    gpio_num_t red_pin;          /**< Driver indicator LED. */
    bool blue_active_high;       /**< Blue LED polarity (true = high level lights it). */
    bool red_active_high;        /**< Red LED polarity (true = high level lights it). */
    bool (*power_ok_fn)(void);   /**< Returns true when VBUS is confirmed. NULL = always false. */
    bool (*driver_on_fn)(void);  /**< Returns true when the LED driver is enabled. NULL = always false. */
} status_leds_config_t;

/**
 * @brief Configures the indicator LEDs and starts their update task.
 * @param config Indicator GPIOs, polarity, and status callbacks.
 * @return None.
 */
void status_leds_init(const status_leds_config_t *config);

#ifdef __cplusplus
}
#endif
