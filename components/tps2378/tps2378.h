/** @file tps2378.h
 * @brief PoE/AUX power monitoring and VBUS validation.
 *
 * Ready is gated on VBUS alone (debounced, with hysteresis). CDB is the
 * hotswap power-good (inrush done) and is telemetry only; T2P is the
 * Type-1 vs Type-2/AUX class selector and never gates readiness. See
 * tps2378.c for the full pin semantics.
 */
#pragma once

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Power class (derived from the T2P selector; only meaningful while
 * ready). This board has no APD GPIO, so TPS2378_SOURCE_TYPE2 currently
 * covers both a real Type-2 PSE and an AUX bench supply -- both carry the
 * 25.5 W budget. TPS2378_SOURCE_AUX is reserved for when an APD input is
 * wired and can disambiguate. */
typedef enum {
    TPS2378_SOURCE_NONE = 0, /**< Not powered (VBUS below the operating threshold). */
    TPS2378_SOURCE_TYPE1,    /**< T2P clear: PoE IEEE 802.3af, 12.95 W. */
    TPS2378_SOURCE_TYPE2,    /**< T2P set: PoE IEEE 802.3at or AUX, 25.5 W. */
    TPS2378_SOURCE_AUX,      /**< AUX bench supply, 25.5 W (only emitted with an APD GPIO). */
} tps2378_source_t;

/** @brief TPS2378 monitor configuration. */
typedef struct {
    gpio_num_t cdb_pin;      /**< CDB input: hotswap power-good (inrush done). Telemetry only. */
    gpio_num_t t2p_pin;      /**< T2P input: the class selector (set = Type-2/AUX, clear = Type-1). */
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
 * @brief Blocks until power is confirmed (VBUS at operating level).
 * @param timeout Maximum ticks to wait, or portMAX_DELAY.
 * @return true if power is ready.
 */
bool tps2378_wait_ready(TickType_t timeout);

/**
 * @brief Queries the confirmed power state. Ready == VBUS at operating level
 * (debounced, with hysteresis). VBUS is the sole power gate -- CDB (inrush)
 * and T2P (class) do not gate readiness.
 * @return true if VBUS is confirmed.
 */
bool tps2378_is_ready(void);

/**
 * @brief Gets the power class. NONE unless ready; otherwise TYPE1 (T2P clear)
 * or TYPE2/AUX (T2P set).
 * @return Current class.
 */
tps2378_source_t tps2378_get_source(void);

/**
 * @brief Queries the debounced CDB signal (hotswap inrush complete).
 * Diagnostic only -- not a readiness or class signal.
 * @return true once the inrush is done / the bulk cap is charged.
 */
bool tps2378_cdb_confirmed(void);

/**
 * @brief Queries the debounced T2P class selector.
 * @return true for the Type-2/AUX budget, false for Type-1.
 */
bool tps2378_t2p_confirmed(void);

/**
 * @brief Queries the debounced VBUS-ok signal.
 * @return true if VBUS is confirmed.
 */
bool tps2378_vbus_confirmed(void);

/**
 * @brief Reads the instantaneous (non-debounced) CDB signal.
 * @return true once the hotswap inrush is complete (CDB HIGH).
 */
bool tps2378_cdb_raw(void);

/**
 * @brief Reads the instantaneous (non-debounced) T2P class selector.
 * @return true for the Type-2/AUX budget (T2P active/LOW), false for Type-1.
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
 * @brief Gets the nominal power budget for the current class: 12.95 W
 * (Type-1), 25.5 W (Type-2/AUX). Zero unless ready.
 * @return Power in watts.
 */
float tps2378_get_available_power_w(void);

#ifdef __cplusplus
}
#endif
