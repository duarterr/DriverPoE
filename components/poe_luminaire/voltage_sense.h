/*
 * Reads the DC bus and LED voltages via ADC_VLED_P (GPIO34) and
 * ADC_VLED_N (GPIO35).
 *
 * LEDVP (ADC_VLED_P) is tapped directly off the downstream DC bus — the
 * same rail the TPS2378 releases to the load and that the AUX bench input
 * injects into — so it doubles as the bus voltage sense (VBUS). LEDVN is
 * tapped after the LED string, so VLED = LEDVP - LEDVN is the actual LED
 * forward voltage. The individual LEDVP/LEDVN pin readings aren't
 * meaningful on their own for reporting purposes — only VBUS and VLED
 * are (see voltage_reading_t below).
 *
 * Both channels go through the same resistor divider (Rup=560k, Rdown=22k,
 * see VLED_DIVIDER_RATIO in poe_luminaire.h) before reaching the ESP32 pin.
 *
 * GPIO34/GPIO35 are "input only" (no output driver, no internal pull) —
 * a good match for pure ADC1 input use.
 *
 * Note: the two readings (P and N) are taken sequentially by the ADC
 * oneshot peripheral, not simultaneously — there's a small time gap
 * between them (irrelevant for a DC/slowly-varying voltage like a bus or
 * LED driver's, but don't use this to capture fast ripple).
 *
 * VBUS also feeds directly into the "ready" decision in poe_negotiator.c
 * (VBUS_MIN_MV in poe_luminaire.h) — see poe_negotiator.h.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int vbus_mv;         /* DC bus voltage (= LEDVP, scaled by the resistor divider) */
    int led_voltage_mv;  /* LED forward voltage: LEDVP - LEDVN, both scaled */
} voltage_reading_t;

/* Initializes ADC1 oneshot on both channels and tries to enable
 * calibration ("line fitting" scheme, the only one supported on the
 * classic ESP32). If eFuse-based calibration isn't available, falls back
 * to an approximate conversion — logged as a warning. Must be called
 * before poe_negotiator_init(), since the "ready" decision reads VBUS. */
void voltage_sense_init(void);

/* Takes both readings and fills 'out' with vbus_mv/led_voltage_mv. */
esp_err_t voltage_sense_read(voltage_reading_t *out);

#ifdef __cplusplus
}
#endif
