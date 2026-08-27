/** @file poe_luminaire_main.h
 * @brief Board pin mapping and product-wide configuration constants.
 */
#pragma once

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"

#define ADC_CH_VLED_P                  ADC_CHANNEL_6  /**< LED+ ADC channel. */
#define ADC_CH_VLED_N                  ADC_CHANNEL_7  /**< LED- ADC channel. */

#define PIN_HV9910_SHUTDOWN            GPIO_NUM_32    /**< HV9910 SHUTDOWN GPIO. */
#define PIN_HV9910_DIMMING             GPIO_NUM_33    /**< HV9910 DIM PWM GPIO. */
#define HV9910_SHUTDOWN_ACTIVE_HIGH    0               /**< SHUTDOWN polarity. */
#define HV9910_DIM_ACTIVE_HIGH         1               /**< DIM polarity. */
#define HV9910_PWM_FREQ_HZ             10000           /**< Dimming PWM frequency. */
#define HV9910_DEFAULT_RAMP_MS         250             /**< Default ramp duration. */
#define HV9910_MAX_RAMP_MS             10000           /**< Ramp duration ceiling. */

#define PIN_ETH_RXD0                   GPIO_NUM_25    /**< EMAC RXD0 (fixed in hardware). */
#define PIN_ETH_RXD1                   GPIO_NUM_26    /**< EMAC RXD1 (fixed in hardware). */
#define PIN_ETH_CRS_DV                 GPIO_NUM_27    /**< EMAC CRS_DV (fixed in hardware). */
#define PIN_ETH_TXD0                   GPIO_NUM_19    /**< EMAC TXD0 (fixed in hardware). */
#define PIN_ETH_TXD1                   GPIO_NUM_22    /**< EMAC TXD1 (fixed in hardware). */
#define PIN_ETH_TX_EN                  GPIO_NUM_21    /**< EMAC TX_EN (fixed in hardware). */
#define PIN_ETH_REF_CLK                GPIO_NUM_0     /**< RMII reference clock input. */
#define PIN_ETH_MDC                    GPIO_NUM_23    /**< MDC GPIO. */
#define PIN_ETH_MDIO                   GPIO_NUM_18    /**< MDIO GPIO. */
#define PIN_ETH_PHY_RESET              GPIO_NUM_5     /**< PHY reset GPIO. */
#define ETH_PHY_ADDR                   1               /**< PHY SMI address. */

#define PIN_POE_CDB                    GPIO_NUM_15    /**< TPS2378 CDB input. */
#define PIN_POE_T2P                    GPIO_NUM_2     /**< TPS2378 T2P input. */
#define PIN_LED_BLUE                   GPIO_NUM_12    /**< Power indicator LED. */
#define PIN_LED_RED                    GPIO_NUM_14    /**< Driver indicator LED. */
#define STATUS_LED_BLUE_ACTIVE_HIGH    0               /**< Blue LED polarity. */
#define STATUS_LED_RED_ACTIVE_HIGH     0               /**< Red LED polarity. */
#define POWERON_SETTLE_MS              2000            /**< Power-on settle delay before auto-resuming. */

#define VLED_DIVIDER_RUP_OHM           560000.0f
#define VLED_DIVIDER_RDOWN_OHM         22200.0f         /**< Adjusted. Was 22000 */
#define VLED_DIVIDER_RATIO             ((VLED_DIVIDER_RUP_OHM + VLED_DIVIDER_RDOWN_OHM) / VLED_DIVIDER_RDOWN_OHM) /**< Voltage divider scale factor. */
#define VBUS_MIN_MV                    40000           /**< Minimum valid VBUS. */
#define VBUS_HYSTERESIS_MV             2000            /**< VBUS hysteresis margin. */
#define DEVID_MODEL_PREFIX             "DriverPoE"     /**< Product name; prefixes the serial and the DHCP hostname. */
#define ADMIN_UDP_PORT                 5001            /**< Admin channel UDP port. */

/** Factory-default admin secret (see devid_get_admin_secret()/
 * devid_set_admin_secret()); written to NVS on first boot and again after
 * any FACTORY_RESET. ASCII "DriverPoE-default-admin-secret!!" (32 bytes),
 * spelled out as hex so the array length is unambiguous. */
#define ADMIN_DEFAULT_SECRET { \
    0x44, 0x72, 0x69, 0x76, 0x65, 0x72, 0x50, 0x6f, \
    0x45, 0x2d, 0x64, 0x65, 0x66, 0x61, 0x75, 0x6c, \
    0x74, 0x2d, 0x61, 0x64, 0x6d, 0x69, 0x6e, 0x2d, \
    0x73, 0x65, 0x63, 0x72, 0x65, 0x74, 0x21, 0x21, \
}

/* Extra connector (unused header on the PCB). */
#define PIN_EXTRA1                     GPIO_NUM_36  /**< ADC1_CH0, input-only, no internal pull resistor. */
#define PIN_EXTRA2                     GPIO_NUM_39  /**< ADC1_CH3, input-only, no internal pull resistor. */
#define PIN_EXTRA3                     GPIO_NUM_13  /**< JTAG MTCK, not a boot strapping pin. */
#define PIN_EXTRA4                     GPIO_NUM_4   /**< No special function, no strap, no JTAG. */
#define PIN_EXTRA5                     GPIO_NUM_16  /**< No special function; unavailable on PSRAM-equipped modules. */
#define PIN_EXTRA6                     GPIO_NUM_17  /**< No special function; unavailable on PSRAM-equipped modules. */
