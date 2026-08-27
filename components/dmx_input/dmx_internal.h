/** @file dmx_internal.h
 * @brief Shared internals between dmx_input.c and the protocol parsers
 * (dmx_artnet.c, dmx_sacn.c). Not part of the public API.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dmx_input.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Which parser produced a frame. */
typedef enum {
    DMX_FRAME_ARTNET = 0,
    DMX_FRAME_SACN = 1,
} dmx_frame_kind_t;

/**
 * @brief One decoded DMX frame handed from a parser to the merge engine.
 *
 * The parser has already checked that the frame targets this fixture's
 * configured universe; it has NOT applied the DMX start address -- slots[]
 * is the raw universe (index 0 == DMX channel 1).
 */
typedef struct {
    dmx_frame_kind_t kind;
    uint32_t src_ip;             /**< Host order. */
    uint8_t  source_id[16];      /**< sACN CID; Art-Net: src_ip in the low 4 bytes, rest 0. */
    uint8_t  priority;           /**< sACN priority (0-200); Art-Net: fixed 100. */
    uint8_t  sequence;           /**< Frame sequence counter (0 = ordering disabled). */
    bool     terminated;         /**< sACN stream_terminated option -> drop this source now. */
    uint16_t nslots;             /**< Valid entries in slots[] (<= 512). */
    const uint8_t *slots;        /**< Pointer into the parser's rx buffer, valid for the call only. */
} dmx_frame_t;

/**
 * @brief Merge-engine entry point. Called by the parsers from their own
 * task context (the shared dmx_input task -- see dmx_input.c).
 * @param frame Decoded frame; not retained after the call returns.
 * @return None.
 */
void dmx_merge_submit(const dmx_frame_t *frame);

/**
 * @brief Drops all tracked sources except the one that submitted most
 * recently (Art-Net ArtAddress "AcCancelMerge").
 * @return None.
 */
void dmx_merge_cancel(void);

/* --- Big/little-endian helpers (same style as admin_channel.c) --- */

static inline uint16_t dmx_get_u16_be(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static inline uint16_t dmx_get_u16_le(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[1] << 8) | p[0]);
}

static inline void dmx_put_u16_be(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

/* --- Config snapshot the parsers read (updated by dmx_input.c under lock) --- */

/**
 * @brief Thread-safe snapshot of the fields the parsers need on the hot path.
 * @param out Destination.
 * @return None.
 */
void dmx_cfg_snapshot(dmx_input_config_t *out);

#ifdef __cplusplus
}
#endif
