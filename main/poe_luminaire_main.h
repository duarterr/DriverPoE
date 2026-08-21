#pragma once

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"

#define ADC_CH_VLED_P                  ADC_CHANNEL_6
#define ADC_CH_VLED_N                  ADC_CHANNEL_7

#define PIN_HV9910_SHUTDOWN            GPIO_NUM_32
#define PIN_HV9910_DIMMING             GPIO_NUM_33
#define HV9910_SHUTDOWN_ACTIVE_HIGH    0
#define HV9910_DIM_ACTIVE_HIGH         1
#define HV9910_PWM_FREQ_HZ             10000
#define HV9910_DEFAULT_RAMP_MS         250
#define HV9910_MAX_RAMP_MS             10000

#define PIN_ETH_RXD0                   GPIO_NUM_25
#define PIN_ETH_RXD1                   GPIO_NUM_26
#define PIN_ETH_CRS_DV                 GPIO_NUM_27
#define PIN_ETH_TXD0                   GPIO_NUM_19
#define PIN_ETH_TXD1                   GPIO_NUM_22
#define PIN_ETH_TX_EN                  GPIO_NUM_21
#define PIN_ETH_REF_CLK                GPIO_NUM_0
#define PIN_ETH_MDC                    GPIO_NUM_23
#define PIN_ETH_MDIO                   GPIO_NUM_18
#define PIN_ETH_PHY_RESET              GPIO_NUM_5
#define ETH_PHY_ADDR                   1

#define PIN_POE_CDB                    GPIO_NUM_15
#define PIN_POE_T2P                    GPIO_NUM_2
#define PIN_LED_BLUE                   GPIO_NUM_12
#define PIN_LED_RED                    GPIO_NUM_14
#define STATUS_LED_BLUE_ACTIVE_HIGH    0
#define STATUS_LED_RED_ACTIVE_HIGH     0
#define POWERON_SETTLE_MS              2000

#define VLED_DIVIDER_RUP_OHM           560000.0f
#define VLED_DIVIDER_RDOWN_OHM         22000.0f
#define VLED_DIVIDER_RATIO             ((VLED_DIVIDER_RUP_OHM + VLED_DIVIDER_RDOWN_OHM) / VLED_DIVIDER_RDOWN_OHM)
#define VBUS_MIN_MV                    40000
#define VBUS_HYSTERESIS_MV             2000
#define DEVID_MODEL_PREFIX             "DriverPoE"
#define ADMIN_UDP_PORT                 5001

// Extra connector (unused header on the PCB)
#define PIN_EXTRA1                     GPIO_NUM_36  /* ADC1_CH0, input-only -- no internal pull resistor */
#define PIN_EXTRA2                     GPIO_NUM_39  /* ADC1_CH3, input-only -- no internal pull resistor */
#define PIN_EXTRA3                     GPIO_NUM_13  /* JTAG MTCK -- not a boot strapping pin */
#define PIN_EXTRA4                     GPIO_NUM_4   /* No special function, no strap, no JTAG */
#define PIN_EXTRA5                     GPIO_NUM_16  /* No special function -- unavailable on PSRAM-equipped modules */
#define PIN_EXTRA6                     GPIO_NUM_17  /* No special function -- unavailable on PSRAM-equipped modules */
