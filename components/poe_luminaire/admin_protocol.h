/*
 * Wire format of the admin channel's binary UDP packet. Definitions only —
 * no logic, no I/O. Actual serialization (big-endian, not relying on the
 * compiler's struct layout/alignment) lives in admin_channel.c.
 *
 * Full byte-by-byte layout documented in README.md ("Canal de
 * administração (UDP, autenticado)") — that section and this header must
 * stay in sync.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ADMIN_PROTO_MAGIC        0x4C554D31u /* "LUM1" */
#define ADMIN_PROTO_VERSION      1

#define ADMIN_SERIAL_LEN         24  /* ASCII, zero-padded; the real serial is much shorter ("LUM1-" + 12 hex = 17) */
#define ADMIN_NONCE_LEN          16
#define ADMIN_HMAC_LEN           32  /* HMAC-SHA256 */
#define ADMIN_GCM_TAG_LEN        16  /* AES-GCM, 128-bit tag */

/* Header size on the wire, field by field (see README.md):
 * magic(4) + version(1) + type(1) + serial(24) + epoch(4) + nonce(16) +
 * payload_len(2) = 52 bytes, before the payload and the trailing HMAC. */
#define ADMIN_HEADER_WIRE_SIZE   (4 + 1 + 1 + ADMIN_SERIAL_LEN + 4 + ADMIN_NONCE_LEN + 2)

/* Largest payload any command uses today (ROTATE_KEY: 32B new key + 4B
 * new epoch + 16B GCM tag = 52B). Generous headroom to evolve without
 * breaking the device's fixed-size buffer. */
#define ADMIN_MAX_PAYLOAD        128

/* Largest UDP packet the device accepts/sends (header + payload + HMAC).
 * Well below typical Ethernet MTU — no IP fragmentation. */
#define ADMIN_MAX_PACKET         (ADMIN_HEADER_WIRE_SIZE + ADMIN_MAX_PAYLOAD + ADMIN_HMAC_LEN)

/*
 * Packet types. Bit 0x80 marks "this is a response" (REQ | 0x80 =
 * matching RESP) — simplifies validation and log reading.
 */
typedef enum {
    ADMIN_TYPE_DISCOVER            = 0x01, /* unauthenticated */
    ADMIN_TYPE_DISCOVER_RESP       = 0x81, /* unauthenticated */
    ADMIN_TYPE_CHALLENGE           = 0x02,
    ADMIN_TYPE_CHALLENGE_RESP      = 0x82,
    ADMIN_TYPE_STATUS              = 0x03,
    ADMIN_TYPE_STATUS_RESP         = 0x83,
    ADMIN_TYPE_IDENTIFY            = 0x04,
    ADMIN_TYPE_IDENTIFY_RESP       = 0x84,
    ADMIN_TYPE_REBOOT              = 0x05,
    ADMIN_TYPE_REBOOT_RESP         = 0x85,
    ADMIN_TYPE_FACTORY_RESET       = 0x06,
    ADMIN_TYPE_FACTORY_RESET_RESP  = 0x86,
    ADMIN_TYPE_ROTATE_KEY          = 0x07,
    ADMIN_TYPE_ROTATE_KEY_RESP     = 0x87,
    ADMIN_TYPE_ROTATE_CONFIRM      = 0x08,
    ADMIN_TYPE_ROTATE_CONFIRM_RESP = 0x88,
    ADMIN_TYPE_CLAIM               = 0x09, /* UNAUTHENTICATED, like DISCOVER; only accepted while NOT
                                             * provisioned -- see devid_claim() */
    ADMIN_TYPE_CLAIM_RESP          = 0x89,
    ADMIN_TYPE_ERR_RESP            = 0xFF, /* authenticated packet, but the command itself is invalid/malformed */
} admin_pkt_type_t;

/* Status code carried in the first payload byte of most responses
 * (except DISCOVER_RESP and STATUS_RESP, which have their own structured
 * payload — see README.md). */
typedef enum {
    ADMIN_STATUS_OK                  = 0,
    ADMIN_STATUS_ERR_BAD_ARG         = 1,
    ADMIN_STATUS_ERR_NOT_READY       = 2, /* e.g. IDENTIFY refused: PoE/AUX/VBUS not confirmed */
    ADMIN_STATUS_ERR_NOT_PROVISIONED = 3,
    ADMIN_STATUS_ERR_NO_STAGED_KEY   = 4, /* ROTATE_CONFIRM with no pending ROTATE_KEY (or it expired) */
    ADMIN_STATUS_ERR_INTERNAL        = 5,
} admin_status_t;

#ifdef __cplusplus
}
#endif
