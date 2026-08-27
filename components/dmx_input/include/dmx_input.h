/** @file dmx_input.h
 * @brief DMX-over-Ethernet input layer: Art-Net + sACN (E1.31) reception.
 *
 * This is the "standard control layer" that sits alongside the
 * authenticated admin channel (components/admin_channel/). Once a fixture
 * is commissioned (universe + DMX start address + personality), any
 * lighting controller or show software that speaks Art-Net or sACN drives
 * its brightness directly -- no custom tooling.
 *
 * Trust model: Art-Net and sACN have no authentication of their own (by
 * design -- same posture as physical DMX). Keep the device on a trusted
 * management/entertainment network. The admin channel stays independent
 * and authenticated for maintenance/OTA/secret.
 *
 * Arbitration: while a valid DMX signal is present it drives the
 * brightness. Admin ON/OFF/DIM still work but are overridden by the next
 * DMX frame (~22 ms). When the DMX signal is lost, admin control resumes.
 *
 * All actuation goes through the hv9910 driver, which never touches NVS.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Protocol bitmask (dmx_input_config_t.proto_mask). */
#define DMX_PROTO_ARTNET   0x01
#define DMX_PROTO_SACN     0x02

/** @brief Serialized dmx_input_config_t: 1 version byte + 17 payload bytes.
 * Shared by NVS storage, the admin DMX_GET/SET_CONFIG commands, and
 * tools/device_api. Multi-byte fields are big-endian. */
#define DMX_CFG_WIRE_SIZE      18
#define DMX_CFG_LAYOUT_VERSION 1

/** @brief Personality: how many DMX slots the fixture occupies. */
typedef enum {
    DMX_PERSONALITY_1CH_8BIT = 0,  /**< 1 slot: intensity 0-255. */
    DMX_PERSONALITY_2CH_16BIT = 1, /**< 2 slots: intensity MSB + LSB (16-bit). */
} dmx_personality_t;

/** @brief What to do when every DMX source has timed out. */
typedef enum {
    DMX_LOSS_HOLD = 0,     /**< Keep the last commanded level. */
    DMX_LOSS_TO_BLACK = 1, /**< Fade to 0. */
    DMX_LOSS_TO_LEVEL = 2, /**< Fade to loss_level%. */
} dmx_loss_behavior_t;

/** @brief Merge strategy across simultaneously-active sources. */
typedef enum {
    DMX_MERGE_HTP = 0, /**< Highest-takes-precedence. */
    DMX_MERGE_LTP = 1, /**< Latest-takes-precedence. */
} dmx_merge_mode_t;

/** @brief Which source(s) are currently driving the output. */
typedef enum {
    DMX_SOURCE_NONE = 0,
    DMX_SOURCE_ARTNET = 1,
    DMX_SOURCE_SACN = 2,
    DMX_SOURCE_BOTH = 3,
} dmx_active_source_t;

/** @brief Persisted DMX layer configuration (NVS namespace "dmx"). */
typedef struct {
    uint8_t  layer_enabled;        /**< Master on/off for the whole DMX layer. */
    uint8_t  proto_mask;           /**< DMX_PROTO_ARTNET | DMX_PROTO_SACN. */
    uint16_t artnet_port_address;  /**< 15-bit (net<<8)|(subnet<<4)|universe. */
    uint16_t sacn_universe;        /**< 1..63999. */
    uint16_t dmx_address;          /**< Start channel, 1..512. */
    uint8_t  personality;          /**< dmx_personality_t. */
    uint8_t  merge_mode;           /**< dmx_merge_mode_t. */
    uint8_t  loss_behavior;        /**< dmx_loss_behavior_t. */
    uint8_t  loss_level;           /**< 0..100, used by DMX_LOSS_TO_LEVEL. */
    uint16_t loss_timeout_ms;      /**< Per-source inactivity before "lost". */
    uint16_t smoothing_ms;         /**< Inter-frame fade per update (default 25 ~= 1 frame; 0 = track every frame). */
    uint8_t  allow_artaddress;     /**< Honor Art-Net ArtAddress from the wire. */
} dmx_input_config_t;

/** @brief Live status for the admin INFO response / web UI. Carries enough
 * of the patch (universe, address, personality, protocols) that the
 * unauthenticated demo UI can build Art-Net frames from INFO alone. */
typedef struct {
    bool     layer_enabled;
    uint8_t  active_source;        /**< dmx_active_source_t. */
    uint8_t  merged_level_pct;     /**< 0..100 currently applied by the DMX layer. */
    uint8_t  fps;                  /**< Frames/s seen across sources (rolling). */
    uint16_t artnet_port_address;
    uint16_t sacn_universe;
    uint32_t last_src_ip;          /**< Host order; 0 if none. */
    uint16_t dmx_address;          /**< Start channel, 1..512. */
    uint8_t  personality;          /**< dmx_personality_t. */
    uint8_t  proto_mask;           /**< DMX_PROTO_ARTNET | DMX_PROTO_SACN. */
} dmx_input_status_t;

/**
 * @brief Starts the DMX input layer (loads config from NVS, spawns the task).
 *
 * Call after Ethernet is up. If the persisted config has layer_enabled=0
 * the task still runs but binds nothing until enabled via
 * dmx_input_set_config().
 * @return None.
 */
void dmx_input_start(void);

/**
 * @brief Copies the current (RAM) configuration.
 * @param out Destination.
 * @return true if the layer is initialized.
 */
bool dmx_input_get_config(dmx_input_config_t *out);

/**
 * @brief Validates, applies, and persists a new configuration.
 *
 * Re-binds the Art-Net/sACN sockets and multicast membership if the
 * relevant fields changed. Rejects out-of-range values.
 * @param cfg New configuration.
 * @return true if accepted and persisted.
 */
bool dmx_input_set_config(const dmx_input_config_t *cfg);

/**
 * @brief Copies the live status.
 * @param out Destination.
 * @return true if the layer is initialized.
 */
bool dmx_input_get_status(dmx_input_status_t *out);

/**
 * @brief Serializes a config to DMX_CFG_WIRE_SIZE bytes (big-endian).
 * @param cfg Source config.
 * @param out Destination, at least DMX_CFG_WIRE_SIZE bytes.
 * @return None.
 */
void dmx_config_pack(const dmx_input_config_t *cfg, uint8_t *out);

/**
 * @brief Parses DMX_CFG_WIRE_SIZE bytes into a config.
 * @param in Source bytes.
 * @param len Length of in.
 * @param out Destination config.
 * @return true if the layout version and length are recognized.
 */
bool dmx_config_unpack(const uint8_t *in, size_t len, dmx_input_config_t *out);

#ifdef __cplusplus
}
#endif
