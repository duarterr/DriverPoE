/** @file tps2378.h
 * @brief PoE/AUX power monitoring and VBUS validation.
 */
#pragma once

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Detected power source. */
typedef enum {
    TPS2378_SOURCE_NONE = 0, /**< No valid source. */
    TPS2378_SOURCE_TYPE1,    /**< PoE IEEE 802.3af. */
    TPS2378_SOURCE_TYPE2,    /**< PoE IEEE 802.3at. */
    TPS2378_SOURCE_AUX,      /**< Auxiliary power supply. */
} tps2378_source_t;

/** @brief TPS2378 monitor configuration. */
typedef struct {
    gpio_num_t cdb_pin;      /**< CDB input. */
    gpio_num_t t2p_pin;      /**< T2P input. */
    int vbus_min_mv;         /**< Minimum valid VBUS. */
    int vbus_hysteresis_mv;  /**< VBUS hysteresis margin. */
    void (*on_power_ready)(tps2378_source_t source, void *ctx); /**< Power-ready notification. */
    void (*on_power_lost)(void *ctx);                            /**< Power-lost notification. */
    void (*on_source_changed)(tps2378_source_t source, void *ctx); /**< Fired whenever the debounced
                                 source class changes, including while power stays ready (e.g. a live
                                 Type-1 -> Type-2 renegotiation). May be NULL. */
    void *callback_ctx;      /**< Context passed to all notifications. */
} tps2378_config_t;

/**
 * @brief Initializes the power monitor.
 * @param config GPIOs, thresholds, and callbacks.
 * @return None.
 */
void tps2378_init(const tps2378_config_t *config);

/**
 * @brief Blocks until power is confirmed.
 * @param timeout Maximum ticks to wait, or portMAX_DELAY.
 * @return true if power is ready.
 */
bool tps2378_wait_ready(TickType_t timeout);

/**
 * @brief Queries the confirmed power state.
 * @return true if source and VBUS are both valid.
 */
bool tps2378_is_ready(void);

/**
 * @brief Gets the detected power source.
 * @return Current source.
 */
tps2378_source_t tps2378_get_source(void);

/**
 * @brief Queries the debounced CDB signal.
 * @return true if CDB is confirmed.
 */
bool tps2378_cdb_confirmed(void);

/**
 * @brief Queries the debounced T2P signal.
 * @return true if T2P is confirmed.
 */
bool tps2378_t2p_confirmed(void);

/**
 * @brief Queries the debounced VBUS-ok signal.
 * @return true if VBUS is confirmed.
 */
bool tps2378_vbus_confirmed(void);

/**
 * @brief Reads the instantaneous (non-debounced) CDB signal.
 * @return true if CDB is currently active.
 */
bool tps2378_cdb_raw(void);

/**
 * @brief Reads the instantaneous (non-debounced) T2P signal.
 * @return true if T2P is currently active.
 */
bool tps2378_t2p_raw(void);

/**
 * @brief Reads the instantaneous (non-debounced) VBUS-ok signal.
 * @return true if VBUS is currently above the minimum threshold.
 */
bool tps2378_vbus_raw(void);

/**
 * @brief Gets the last measured VBUS.
 * @return Voltage in millivolts.
 */
int tps2378_get_vbus_mv(void);

/**
 * @brief Gets a human-readable name for a source.
 * @param source Source to convert.
 * @return Source name.
 */
const char *tps2378_source_name(tps2378_source_t source);

/**
 * @brief Gets a short identifier for a source.
 * @param source Source to convert.
 * @return Identifier with no spaces.
 */
const char *tps2378_source_short_name(tps2378_source_t source);

/**
 * @brief Gets the nominal PoE power available for the current source.
 * @return Power in watts, or zero for AUX/no source.
 */
float tps2378_get_available_power_w(void);

#ifdef __cplusplus
}
#endif
