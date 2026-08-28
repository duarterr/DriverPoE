/** @file driver_config.h
 * @brief Persisted HV9910 dimming configuration (NVS namespace "driver").
 *
 * The hv9910 component keeps nothing on flash; this module owns the NVS
 * blob, loads it at boot, pushes it into hv9910 via hv9910_set_dimming(),
 * and re-pushes on every admin DRIVER_SET_CONFIG. Mirrors the
 * self-loading pattern of components/dmx_input/.
 *
 * Wire format (DRV_CFG_WIRE_SIZE bytes, big-endian, leading layout-version
 * byte) -- shared by NVS storage, the admin DRIVER_GET/SET_CONFIG
 * commands, and tools/device_api. Must match driver_config_pack() /
 * driver_config_unpack() and DriverConfig in tools/device_api/protocol.py.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Layout: [0]=version [1]=mode [2..4)=pwm_freq_hz(u16) [4..8)=analog_freq_hz(u32)
 *          [8..10)=min_on_time_us(u16) [10]=crossover_pct */
#define DRV_CFG_WIRE_SIZE      11
#define DRV_CFG_LAYOUT_VERSION 1

/** @brief Persisted dimming configuration. */
typedef struct {
    uint8_t  mode;           /**< hv9910_dim_mode_t: 0 PWM, 1 ANALOG, 2 HYBRID. */
    uint16_t pwm_freq_hz;    /**< PWMD switching frequency, 1000..5000. */
    uint32_t analog_freq_hz; /**< LD (RC-fed) PWM frequency, 40000..80000. */
    uint16_t min_on_time_us; /**< PWMD minimum conduction burst, 2..200. */
    uint8_t  crossover_pct;  /**< HYBRID knee, 10..60. */
} driver_config_t;

/**
 * @brief Loads the config from NVS (or writes defaults) and applies it to
 * the hv9910 driver. Call once, right after hv9910_init().
 * @return None.
 */
void driver_config_start(void);

/**
 * @brief Copies the current (RAM) configuration.
 * @param out Destination.
 * @return true if the module has started.
 */
bool driver_config_get(driver_config_t *out);

/**
 * @brief Validates/clamps, applies to hv9910, and persists a new config.
 * @param cfg New configuration.
 * @return true if the module has started (values are clamped, never rejected).
 */
bool driver_config_set(const driver_config_t *cfg);

/**
 * @brief Serializes a config to DRV_CFG_WIRE_SIZE bytes (big-endian).
 * @param cfg Source config.
 * @param out Destination, at least DRV_CFG_WIRE_SIZE bytes.
 * @return None.
 */
void driver_config_pack(const driver_config_t *cfg, uint8_t *out);

/**
 * @brief Parses DRV_CFG_WIRE_SIZE bytes into a config.
 * @param in Source bytes.
 * @param len Length of in.
 * @param out Destination config.
 * @return true if the layout version and length are recognized.
 */
bool driver_config_unpack(const uint8_t *in, size_t len, driver_config_t *out);

#ifdef __cplusplus
}
#endif
