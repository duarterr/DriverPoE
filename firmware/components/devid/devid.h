/** @file devid.h
 * @brief Public device identity (serial, MAC) and the admin channel's secret.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEVID_MAC_LEN    6  /**< Base MAC length, in bytes. */
#define DEVID_SECRET_LEN 32 /**< Admin secret length, in bytes. */

/** @brief Device identity configuration. */
typedef struct {
    const char *model_prefix;              /**< Prefix used in the serial number. */
    const uint8_t *factory_default_secret; /**< DEVID_SECRET_LEN bytes; see ADMIN_DEFAULT_SECRET in poe_luminaire_main.h. */
} devid_config_t;

/**
 * @brief Initializes device identity: computes the serial/MAC and loads (or writes, if absent) the admin secret in NVS.
 * @param config Model prefix and factory-default secret.
 * @return None.
 */
void devid_init(const devid_config_t *config);

/**
 * @brief Gets the device serial number.
 * @return ASCII serial number string.
 */
const char *devid_get_serial(void);

/**
 * @brief Gets the model prefix.
 * @return Configured prefix, or NULL before initialization.
 */
const char *devid_get_model_prefix(void);

/**
 * @brief Gets the chip's raw base MAC, also used to compose the serial.
 * @return Pointer to DEVID_MAC_LEN bytes.
 */
const uint8_t *devid_get_mac(void);

/**
 * @brief Gets the active admin secret (factory default, or the last value written via devid_set_admin_secret()).
 * @return Pointer to DEVID_SECRET_LEN bytes.
 */
const uint8_t *devid_get_admin_secret(void);

/**
 * @brief Changes the active admin secret and persists it to NVS.
 * @param new_secret New secret, DEVID_SECRET_LEN bytes.
 * @return true if the write succeeded (the in-RAM secret only changes in that case).
 */
bool devid_set_admin_secret(const uint8_t new_secret[DEVID_SECRET_LEN]);

#ifdef __cplusplus
}
#endif
