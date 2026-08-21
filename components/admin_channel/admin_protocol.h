/** @file admin_protocol.h
 * @brief Formato binário do protocolo UDP administrativo.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ADMIN_PROTO_MAGIC      0x44504F45u /**< Identificador "DPOE". */
#define ADMIN_PROTO_VERSION    1          /**< Versão do protocolo. */
#define ADMIN_SERIAL_LEN       24         /**< Campo serial em bytes. */
#define ADMIN_NONCE_LEN        16         /**< Nonce em bytes. */
#define ADMIN_HMAC_LEN         32         /**< HMAC-SHA256 em bytes. */
#define ADMIN_GCM_TAG_LEN      16         /**< Tag AES-GCM em bytes. */
#define ADMIN_HEADER_WIRE_SIZE (4 + 1 + 1 + ADMIN_SERIAL_LEN + 4 + ADMIN_NONCE_LEN + 2) /**< Cabeçalho serializado. */
#define ADMIN_MAX_PAYLOAD      128        /**< Payload máximo. */
#define ADMIN_MAX_PACKET       (ADMIN_HEADER_WIRE_SIZE + ADMIN_MAX_PAYLOAD + ADMIN_HMAC_LEN) /**< Pacote máximo. */

/** @brief Tipos de pacote administrativo. */
typedef enum {
    ADMIN_TYPE_DISCOVER = 0x01, ADMIN_TYPE_DISCOVER_RESP = 0x81,
    ADMIN_TYPE_CHALLENGE = 0x02, ADMIN_TYPE_CHALLENGE_RESP = 0x82,
    ADMIN_TYPE_STATUS = 0x03, ADMIN_TYPE_STATUS_RESP = 0x83,
    ADMIN_TYPE_IDENTIFY = 0x04, ADMIN_TYPE_IDENTIFY_RESP = 0x84,
    ADMIN_TYPE_REBOOT = 0x05, ADMIN_TYPE_REBOOT_RESP = 0x85,
    ADMIN_TYPE_FACTORY_RESET = 0x06, ADMIN_TYPE_FACTORY_RESET_RESP = 0x86,
    ADMIN_TYPE_ROTATE_KEY = 0x07, ADMIN_TYPE_ROTATE_KEY_RESP = 0x87,
    ADMIN_TYPE_ROTATE_CONFIRM = 0x08, ADMIN_TYPE_ROTATE_CONFIRM_RESP = 0x88,
    ADMIN_TYPE_CLAIM = 0x09, ADMIN_TYPE_CLAIM_RESP = 0x89,
    ADMIN_TYPE_ON = 0x0A, ADMIN_TYPE_ON_RESP = 0x8A,
    ADMIN_TYPE_OFF = 0x0B, ADMIN_TYPE_OFF_RESP = 0x8B,
    ADMIN_TYPE_DIM = 0x0C, ADMIN_TYPE_DIM_RESP = 0x8C,
    ADMIN_TYPE_ERR_RESP = 0xFF,
} admin_pkt_type_t;

/** @brief Códigos de status de resposta. */
typedef enum {
    ADMIN_STATUS_OK = 0,
    ADMIN_STATUS_ERR_BAD_ARG = 1,
    ADMIN_STATUS_ERR_NOT_READY = 2,
    ADMIN_STATUS_ERR_NOT_PROVISIONED = 3,
    ADMIN_STATUS_ERR_NO_STAGED_KEY = 4,
    ADMIN_STATUS_ERR_INTERNAL = 5,
} admin_status_t;

#ifdef __cplusplus
}
#endif
