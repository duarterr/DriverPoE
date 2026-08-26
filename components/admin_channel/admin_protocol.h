/** @file admin_protocol.h
 * @brief Binary wire format of the UDP admin protocol.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ADMIN_PROTO_MAGIC        0x44504F45u /**< "DPOE" identifier. */
#define ADMIN_PROTO_VERSION      4           /**< Protocol version. */
#define ADMIN_SERIAL_LEN         24          /**< Serial field, in bytes. */
#define ADMIN_NONCE_LEN          16          /**< Nonce, in bytes. */
#define ADMIN_HMAC_LEN           32          /**< HMAC-SHA256, in bytes. */
#define ADMIN_GCM_TAG_LEN        16          /**< AES-GCM tag, in bytes. */
#define ADMIN_HEADER_WIRE_SIZE   (4 + 1 + 1 + ADMIN_SERIAL_LEN + ADMIN_NONCE_LEN + 2) /**< Serialized header (48 bytes). */
#define ADMIN_OTA_CHUNK_MAX_DATA 1024         /**< Image bytes per ADMIN_TYPE_OTA_CHUNK. */
#define ADMIN_OTA_SHA256_LEN     32           /**< Full-image SHA-256 length, in ADMIN_TYPE_OTA_BEGIN. */
#define ADMIN_MAX_PAYLOAD        (4 + ADMIN_OTA_CHUNK_MAX_DATA) /**< Maximum payload size. */
#define ADMIN_MAX_PACKET         (ADMIN_HEADER_WIRE_SIZE + ADMIN_MAX_PAYLOAD + ADMIN_HMAC_LEN) /**< Maximum packet size. */

/** @brief Admin packet types. */
typedef enum {
    ADMIN_TYPE_INFO = 0x01,           ADMIN_TYPE_INFO_RESP = 0x81,           /**< Unauthenticated. */
    ADMIN_TYPE_CHALLENGE = 0x02,      ADMIN_TYPE_CHALLENGE_RESP = 0x82,      /**< HMAC; issues a nonce. */
    ADMIN_TYPE_ON = 0x03,             ADMIN_TYPE_ON_RESP = 0x83,             /**< HMAC + nonce. */
    ADMIN_TYPE_OFF = 0x04,            ADMIN_TYPE_OFF_RESP = 0x84,            /**< HMAC + nonce. */
    ADMIN_TYPE_DIM = 0x05,            ADMIN_TYPE_DIM_RESP = 0x85,            /**< HMAC + nonce. */
    ADMIN_TYPE_IDENTIFY = 0x06,       ADMIN_TYPE_IDENTIFY_RESP = 0x86,       /**< HMAC + nonce. */
    ADMIN_TYPE_REBOOT = 0x07,         ADMIN_TYPE_REBOOT_RESP = 0x87,         /**< HMAC + nonce. */
    ADMIN_TYPE_FACTORY_RESET = 0x08,  ADMIN_TYPE_FACTORY_RESET_RESP = 0x88,  /**< HMAC + nonce. */
    /** Changes the active admin secret (see devid_set_admin_secret()).
     * Payload = AES-256-GCM(new_secret[32]) under the current secret,
     * nonce = header nonce, AAD = whole header. HMAC + nonce. */
    ADMIN_TYPE_CHANGE_SECRET = 0x09,  ADMIN_TYPE_CHANGE_SECRET_RESP = 0x89,
    /** Starts an OTA session. Payload = total_size(4, BE) + sha256(32) of
     * the full image. HMAC + nonce. */
    ADMIN_TYPE_OTA_BEGIN = 0x0A,      ADMIN_TYPE_OTA_BEGIN_RESP = 0x8A,
    /** One image chunk. Payload = offset(4, BE) + up to
     * ADMIN_OTA_CHUNK_MAX_DATA bytes of data. Response payload =
     * status(1) + bytes_written(4, BE). HMAC only, no nonce. */
    ADMIN_TYPE_OTA_CHUNK = 0x0B,      ADMIN_TYPE_OTA_CHUNK_RESP = 0x8B,
    /** Finalizes an OTA session: validates size/hash/image and stages the
     * new boot partition. HMAC + nonce. */
    ADMIN_TYPE_OTA_END = 0x0C,        ADMIN_TYPE_OTA_END_RESP = 0x8C,
    /** Cancels an in-progress OTA session, if any (idempotent). HMAC + nonce. */
    ADMIN_TYPE_OTA_ABORT = 0x0D,      ADMIN_TYPE_OTA_ABORT_RESP = 0x8D,
    ADMIN_TYPE_ERR_RESP = 0xFF,
} admin_pkt_type_t;

/** @brief Response status codes. */
typedef enum {
    ADMIN_STATUS_OK = 0,               /**< Accepted and applied immediately. */
    ADMIN_STATUS_ERR_BAD_ARG = 1,      /**< Malformed or out-of-range argument. */
    ADMIN_STATUS_ERR_NOT_READY = 2,    /**< Refused because power isn't confirmed. */
    ADMIN_STATUS_ERR_INTERNAL = 3,     /**< Internal failure. */
    ADMIN_STATUS_ACCEPTED_PENDING = 4, /**< Accepted; deferred until power is confirmed. */
    ADMIN_STATUS_ERR_BUSY = 5,         /**< Another OTA session is already in progress. */
} admin_status_t;

#ifdef __cplusplus
}
#endif
