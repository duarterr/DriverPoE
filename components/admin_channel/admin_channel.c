/** @file admin_channel.c
 * @brief Authenticated UDP admin channel implementation.
 *
 * See admin_channel.h and admin_protocol.h for the public API and wire
 * format. INFO is unauthenticated; every other command requires
 * HMAC-SHA256 with the active admin secret plus a single-use, IP-bound
 * nonce obtained via CHALLENGE (ADMIN_TYPE_OTA_CHUNK is HMAC-only, no
 * nonce). OTA_BEGIN/OTA_CHUNK/OTA_END/OTA_ABORT push a firmware image
 * over this same channel into the inactive OTA partition.
 */
#include "admin_channel.h"
#include "admin_protocol.h"
#include "devid.h"
#include "tps2378.h"
#include "hv9910.h"
#include "eth_init.h"
#include "voltage_sense.h"
#include "dmx_input.h"
#include "driver_config.h"
#include "power_manager.h"

#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "esp_ota_ops.h"

#include "lwip/sockets.h"

#include "psa/crypto.h"

static const char *TAG = "ADMIN_CH";

#define TASK_STACK_SIZE          6144
#define TASK_PRIORITY            (tskIDLE_PRIORITY + 5)

static admin_channel_config_t s_config;

#define NONCE_POOL_SIZE           8
#define NONCE_TTL_US              (5 * 1000000LL)

#define RATE_LIMIT_TABLE_SIZE     16
#define RATE_LIMIT_WINDOW_US      (1 * 1000000LL)
#define RATE_LIMIT_MAX_PER_WINDOW 40

#define OTA_IDLE_TIMEOUT_US       (30 * 1000000LL)

/**
 * @brief Writes a big-endian uint16 to a buffer.
 * @param p Destination.
 * @param v Value to write.
 * @return None.
 */
static void put_u16_be(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

/**
 * @brief Writes a big-endian uint32 to a buffer.
 * @param p Destination.
 * @param v Value to write.
 * @return None.
 */
static void put_u32_be(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

/**
 * @brief Reads a big-endian uint16 from a buffer.
 * @param p Source.
 * @return Decoded value.
 */
static uint16_t get_u16_be(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }

/**
 * @brief Reads a big-endian uint32 from a buffer.
 * @param p Source.
 * @return Decoded value.
 */
static uint32_t get_u32_be(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

/** @brief Parsed admin packet header. */
typedef struct {
    uint32_t magic;                    /**< Protocol magic. */
    uint8_t  version;                  /**< Protocol version. */
    uint8_t  type;                     /**< Packet type. */
    char     serial[ADMIN_SERIAL_LEN]; /**< Target serial. */
    uint8_t  nonce[ADMIN_NONCE_LEN];   /**< Nonce field. */
    uint16_t payload_len;              /**< Payload length. */
} parsed_header_t;

/**
 * @brief Parses the fixed-size admin packet header.
 * @param buf Raw packet buffer.
 * @param len Buffer length.
 * @param out Destination for the parsed header.
 * @return true if the buffer is at least one header long.
 */
static bool parse_header(const uint8_t *buf, size_t len, parsed_header_t *out)
{
    if (len < ADMIN_HEADER_WIRE_SIZE) {
        return false;
    }
    size_t off = 0;
    out->magic = get_u32_be(buf + off); off += 4;
    out->version = buf[off++];
    out->type = buf[off++];
    memcpy(out->serial, buf + off, ADMIN_SERIAL_LEN); off += ADMIN_SERIAL_LEN;
    memcpy(out->nonce, buf + off, ADMIN_NONCE_LEN); off += ADMIN_NONCE_LEN;
    out->payload_len = get_u16_be(buf + off); off += 2;
    return true;
}

/**
 * @brief Verifies an HMAC-SHA256 tag under the given key.
 * @param key HMAC key, DEVID_SECRET_LEN bytes.
 * @param data Data covered by the MAC.
 * @param data_len Length of data.
 * @param mac MAC to verify.
 * @param mac_len Length of mac.
 * @return PSA_SUCCESS if valid; a PSA error otherwise.
 */
static psa_status_t hmac_verify(const uint8_t *key, const uint8_t *data, size_t data_len,
                                 const uint8_t *mac, size_t mac_len)
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_VERIFY_MESSAGE);
    psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attr, DEVID_SECRET_LEN * 8);

    mbedtls_svc_key_id_t key_id;
    psa_status_t status = psa_import_key(&attr, key, DEVID_SECRET_LEN, &key_id);
    if (status != PSA_SUCCESS) {
        return status;
    }
    status = psa_mac_verify(key_id, PSA_ALG_HMAC(PSA_ALG_SHA_256), data, data_len, mac, mac_len);
    psa_destroy_key(key_id);
    return status;
}

/**
 * @brief Computes an HMAC-SHA256 tag under the given key.
 * @param key HMAC key, DEVID_SECRET_LEN bytes.
 * @param data Data to cover.
 * @param data_len Length of data.
 * @param mac_out Destination for the MAC.
 * @param mac_out_size Size of mac_out.
 * @param mac_len_out Actual MAC length written.
 * @return PSA_SUCCESS on success; a PSA error otherwise.
 */
static psa_status_t hmac_compute(const uint8_t *key, const uint8_t *data, size_t data_len,
                                  uint8_t *mac_out, size_t mac_out_size, size_t *mac_len_out)
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attr, DEVID_SECRET_LEN * 8);

    mbedtls_svc_key_id_t key_id;
    psa_status_t status = psa_import_key(&attr, key, DEVID_SECRET_LEN, &key_id);
    if (status != PSA_SUCCESS) {
        return status;
    }
    status = psa_mac_compute(key_id, PSA_ALG_HMAC(PSA_ALG_SHA_256), data, data_len,
                              mac_out, mac_out_size, mac_len_out);
    psa_destroy_key(key_id);
    return status;
}

/**
 * @brief Decrypts an AES-256-GCM ciphertext under the given key.
 * @param key AES key, DEVID_SECRET_LEN bytes.
 * @param nonce GCM nonce.
 * @param nonce_len Length of nonce.
 * @param aad Additional authenticated data.
 * @param aad_len Length of aad.
 * @param ciphertext Ciphertext with appended tag.
 * @param ciphertext_len Length of ciphertext.
 * @param plaintext_out Destination for the plaintext.
 * @param plaintext_size Size of plaintext_out.
 * @param plaintext_len_out Actual plaintext length written.
 * @return PSA_SUCCESS on success; a PSA error otherwise.
 */
static psa_status_t gcm_decrypt(const uint8_t *key, const uint8_t *nonce, size_t nonce_len,
                                 const uint8_t *aad, size_t aad_len,
                                 const uint8_t *ciphertext, size_t ciphertext_len,
                                 uint8_t *plaintext_out, size_t plaintext_size, size_t *plaintext_len_out)
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attr, PSA_ALG_GCM);
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, DEVID_SECRET_LEN * 8);

    mbedtls_svc_key_id_t key_id;
    psa_status_t status = psa_import_key(&attr, key, DEVID_SECRET_LEN, &key_id);
    if (status != PSA_SUCCESS) {
        return status;
    }
    status = psa_aead_decrypt(key_id, PSA_ALG_GCM, nonce, nonce_len, aad, aad_len,
                               ciphertext, ciphertext_len, plaintext_out, plaintext_size, plaintext_len_out);
    psa_destroy_key(key_id);
    return status;
}

/** @brief One slot in the CHALLENGE nonce pool. */
typedef struct {
    bool used;                       /**< Whether this slot holds a live nonce. */
    uint32_t ip;                     /**< Source IP the nonce was issued to (host order). */
    uint8_t nonce[ADMIN_NONCE_LEN];  /**< Nonce value. */
    int64_t issued_at_us;            /**< Issue timestamp. */
} nonce_slot_t;

static nonce_slot_t s_nonces[NONCE_POOL_SIZE];

/**
 * @brief Issues a fresh nonce, bound to a source IP.
 * @param ip Source IP (host order) the nonce is bound to.
 * @param out Destination for the new nonce, ADMIN_NONCE_LEN bytes.
 * @return true on success; false if the RNG failed.
 */
static bool nonce_issue(uint32_t ip, uint8_t out[ADMIN_NONCE_LEN])
{
    psa_status_t rnd_status = psa_generate_random(out, ADMIN_NONCE_LEN);
    if (rnd_status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_generate_random() failed (%d) -- refusing to issue a nonce", (int)rnd_status);
        return false;
    }

    int64_t now = esp_timer_get_time();
    int victim = 0;
    int64_t oldest = INT64_MAX;
    for (int i = 0; i < NONCE_POOL_SIZE; i++) {
        if (!s_nonces[i].used) {
            victim = i;
            break;
        }
        if (s_nonces[i].issued_at_us < oldest) {
            oldest = s_nonces[i].issued_at_us;
            victim = i;
        }
    }
    s_nonces[victim].used = true;
    s_nonces[victim].ip = ip;
    s_nonces[victim].issued_at_us = now;
    memcpy(s_nonces[victim].nonce, out, ADMIN_NONCE_LEN);
    return true;
}

/**
 * @brief Consumes (invalidates) a nonce if valid, unexpired, and bound to the given IP.
 * @param ip Source IP (host order) of the request.
 * @param nonce Nonce to consume.
 * @return true if the nonce was valid and has been consumed.
 */
static bool nonce_consume(uint32_t ip, const uint8_t nonce[ADMIN_NONCE_LEN])
{
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < NONCE_POOL_SIZE; i++) {
        if (!s_nonces[i].used) {
            continue;
        }
        if ((now - s_nonces[i].issued_at_us) > NONCE_TTL_US) {
            s_nonces[i].used = false;
            continue;
        }
        if (s_nonces[i].ip == ip && memcmp(s_nonces[i].nonce, nonce, ADMIN_NONCE_LEN) == 0) {
            s_nonces[i].used = false;
            return true;
        }
    }
    return false;
}

/** @brief One slot in the per-source-IP rate limit table. */
typedef struct {
    uint32_t ip;              /**< Source IP; 0 = free slot. */
    int count;                /**< Requests seen in the current window. */
    int64_t window_start_us;  /**< Start of the current window. */
} rate_entry_t;

static rate_entry_t s_rate_table[RATE_LIMIT_TABLE_SIZE];

/**
 * @brief Checks and updates the rate limit for a source IP.
 * @param ip Source IP (host order).
 * @return true if the request is within the allowed rate.
 */
static bool rate_limit_check(uint32_t ip)
{
    int64_t now = esp_timer_get_time();
    int free_slot = -1;
    for (int i = 0; i < RATE_LIMIT_TABLE_SIZE; i++) {
        if (s_rate_table[i].ip == ip) {
            if ((now - s_rate_table[i].window_start_us) > RATE_LIMIT_WINDOW_US) {
                s_rate_table[i].window_start_us = now;
                s_rate_table[i].count = 0;
            }
            s_rate_table[i].count++;
            return s_rate_table[i].count <= RATE_LIMIT_MAX_PER_WINDOW;
        }
        if (s_rate_table[i].ip == 0 && free_slot < 0) {
            free_slot = i;
        }
    }
    int slot = (free_slot >= 0) ? free_slot : 0;
    s_rate_table[slot].ip = ip;
    s_rate_table[slot].window_start_us = now;
    s_rate_table[slot].count = 1;
    return true;
}

/**
 * @brief Serializes and sends one admin packet.
 * @param sock UDP socket.
 * @param dst Destination address.
 * @param type Packet type.
 * @param nonce Nonce to include, or NULL for an all-zero nonce field.
 * @param payload Payload bytes, or NULL if payload_len is 0.
 * @param payload_len Payload length.
 * @param hmac_key HMAC key, or NULL to send a zeroed (unauthenticated) HMAC.
 * @return None.
 */
static void send_packet(int sock, const struct sockaddr_in *dst, admin_pkt_type_t type,
                         const uint8_t nonce[ADMIN_NONCE_LEN],
                         const uint8_t *payload, uint16_t payload_len,
                         const uint8_t *hmac_key)
{
    uint8_t buf[ADMIN_MAX_PACKET];
    size_t off = 0;

    put_u32_be(buf + off, ADMIN_PROTO_MAGIC); off += 4;
    buf[off++] = ADMIN_PROTO_VERSION;
    buf[off++] = (uint8_t)type;

    memset(buf + off, 0, ADMIN_SERIAL_LEN);
    strncpy((char *)buf + off, devid_get_serial(), ADMIN_SERIAL_LEN - 1);
    off += ADMIN_SERIAL_LEN;

    if (nonce) {
        memcpy(buf + off, nonce, ADMIN_NONCE_LEN);
    } else {
        memset(buf + off, 0, ADMIN_NONCE_LEN);
    }
    off += ADMIN_NONCE_LEN;

    put_u16_be(buf + off, payload_len); off += 2;
    if (payload_len > 0) {
        memcpy(buf + off, payload, payload_len);
        off += payload_len;
    }

    memset(buf + off, 0, ADMIN_HMAC_LEN);
    if (hmac_key) {
        size_t mac_len = 0;
        psa_status_t hmac_status = hmac_compute(hmac_key, buf, off, buf + off, ADMIN_HMAC_LEN, &mac_len);
        if (hmac_status != PSA_SUCCESS) {
            ESP_LOGE(TAG, "hmac_compute() failed (%d) -- sending type 0x%02x with a zeroed HMAC "
                          "(will fail verification on the client)", (int)hmac_status, (unsigned)type);
        }
    }
    off += ADMIN_HMAC_LEN;

    if (sendto(sock, buf, off, 0, (const struct sockaddr *)dst, sizeof(*dst)) < 0) {
        ESP_LOGE(TAG, "sendto() failed for type 0x%02x: errno %d", (unsigned)type, errno);
    }
}

/**
 * @brief Sends a one-byte status response.
 * @param sock UDP socket.
 * @param dst Destination address.
 * @param nonce Nonce to echo back.
 * @param type Response packet type.
 * @param status Status code.
 * @param hmac_key HMAC key.
 * @return None.
 */
static void send_status_resp(int sock, const struct sockaddr_in *dst, const uint8_t nonce[ADMIN_NONCE_LEN],
                              admin_pkt_type_t type, admin_status_t status, const uint8_t *hmac_key)
{
    uint8_t payload = (uint8_t)status;
    send_packet(sock, dst, type, nonce, &payload, 1, hmac_key);
}

/**
 * @brief Handles INFO: builds and sends the unauthenticated status payload.
 * @param sock UDP socket.
 * @param hdr Parsed request header.
 * @param src Source address.
 * @return None.
 */
static void handle_info(int sock, const parsed_header_t *hdr, const struct sockaddr_in *src)
{
    (void)hdr;

    vTaskDelay(pdMS_TO_TICKS(esp_random() % 50));

    uint8_t payload[84];
    size_t off = 0;

    memcpy(payload + off, devid_get_mac(), DEVID_MAC_LEN); off += DEVID_MAC_LEN;

    memset(payload + off, 0, 16);
    const esp_app_desc_t *app = esp_app_get_description();
    strncpy((char *)payload + off, app->version, 15);
    off += 16;

    esp_netif_ip_info_t ip_info;
    memset(payload + off, 0, 4);
    if (eth_get_ip_info(&ip_info)) {
        memcpy(payload + off, &ip_info.ip.addr, 4);
    }
    off += 4;

    put_u32_be(payload + off, (uint32_t)(esp_timer_get_time() / 1000000)); off += 4;
    payload[off++] = (uint8_t)esp_reset_reason();
    payload[off++] = tps2378_is_ready() ? 1 : 0;
    payload[off++] = (uint8_t)tps2378_get_source();
    payload[off++] = tps2378_cdb_confirmed() ? 1 : 0;
    payload[off++] = tps2378_t2p_confirmed() ? 1 : 0;
    payload[off++] = tps2378_vbus_confirmed() ? 1 : 0;
    payload[off++] = hv9910_is_enabled() ? 1 : 0;
    payload[off++] = hv9910_pending_on() ? 1 : 0;
    payload[off++] = hv9910_get_dim_percent();
    put_u32_be(payload + off, (uint32_t)tps2378_get_vbus_mv()); off += 4;

    voltage_reading_t v = {0};
    voltage_sense_read(&v);
    put_u32_be(payload + off, (uint32_t)v.led_voltage_mv); off += 4;

    /* --- DMX layer status block: 16 bytes appended after the 47-byte core
     * (payload = 63). Carries everything the unauthenticated demo UI needs
     * to build Art-Net frames without ever reading DMX_GET_CONFIG. --- */
    dmx_input_status_t dmx = {0};
    dmx_input_get_status(&dmx);
    payload[off++] = dmx.layer_enabled ? 1 : 0;
    payload[off++] = dmx.active_source;
    payload[off++] = dmx.merged_level_pct;
    payload[off++] = dmx.fps;
    put_u16_be(payload + off, dmx.artnet_port_address); off += 2;
    put_u16_be(payload + off, dmx.sacn_universe); off += 2;
    put_u32_be(payload + off, dmx.last_src_ip); off += 4;
    put_u16_be(payload + off, dmx.dmx_address); off += 2;
    payload[off++] = dmx.personality;
    payload[off++] = dmx.proto_mask;

    /* --- Dimming-mode block: 8 bytes appended after the DMX block
     * (payload = 71). Lets tools/webui show the current mode/params without
     * an authenticated DRIVER_GET_CONFIG. analog_freq_hz is sent in units
     * of 10 Hz so it fits a u16. --- */
    driver_config_t drv = {0};
    driver_config_get(&drv);
    payload[off++] = drv.mode;
    put_u16_be(payload + off, drv.pwm_freq_hz); off += 2;
    put_u16_be(payload + off, (uint16_t)(drv.analog_freq_hz / 10)); off += 2;
    put_u16_be(payload + off, drv.min_on_time_us); off += 2;
    payload[off++] = drv.crossover_pct;

    /* --- Power block: 13 bytes appended after the dimming block (payload =
     * 84). power_mode + poe_cap_pct mirror the driver-config fields;
     * power_state / effective_scale_pct / budget_cw are the live result of
     * the policy engine. The 7 trailing reserved bytes give room for future
     * power fields without another coordinated INFO size bump. --- */
    payload[off++] = drv.power_mode;
    payload[off++] = drv.poe_cap_pct;
    payload[off++] = (uint8_t)power_manager_get_state();
    payload[off++] = power_manager_effective_scale_pct();
    put_u16_be(payload + off, power_manager_budget_cw()); off += 2;
    memset(payload + off, 0, 7); off += 7;   /* reserved */

    send_packet(sock, src, ADMIN_TYPE_INFO_RESP, NULL, payload, (uint16_t)off, NULL);
}

/**
 * @brief Handles CHALLENGE: issues a new nonce for the requesting IP.
 * @param sock UDP socket.
 * @param hdr Parsed request header.
 * @param src Source address.
 * @return None.
 */
static void handle_challenge(int sock, const parsed_header_t *hdr, const struct sockaddr_in *src)
{
    (void)hdr;
    uint8_t new_nonce[ADMIN_NONCE_LEN];
    if (!nonce_issue(ntohl(src->sin_addr.s_addr), new_nonce)) {
        return;
    }
    send_packet(sock, src, ADMIN_TYPE_CHALLENGE_RESP, new_nonce, NULL, 0, devid_get_admin_secret());
}

/**
 * @brief Handles ON: enables the driver, or defers it if power isn't confirmed.
 *
 * Payload is 4 bytes (a legacy ramp duration) and is accepted but ignored
 * -- the driver applies every change at once; fades belong to the DMX
 * layer / lighting console.
 * @param sock UDP socket.
 * @param hdr Parsed request header.
 * @param payload Request payload (ignored ramp_ms).
 * @param payload_len Payload length.
 * @param src Source address.
 * @return None.
 */
static void handle_on(int sock, const parsed_header_t *hdr,
                       const uint8_t *payload, uint16_t payload_len,
                       const struct sockaddr_in *src)
{
    (void)payload;
    if (payload_len != 4) {
        send_status_resp(sock, src, hdr->nonce, ADMIN_TYPE_ON_RESP, ADMIN_STATUS_ERR_BAD_ARG, devid_get_admin_secret());
        return;
    }

    if (!tps2378_is_ready() || !power_manager_output_allowed()) {
        /* Power unconfirmed, or the power policy is holding the LED off
         * (PoE+ required on Type-1): record the desire so power_manager
         * lights it once the policy is satisfied. */
        hv9910_set_pending(true, 0);
        send_status_resp(sock, src, hdr->nonce, ADMIN_TYPE_ON_RESP, ADMIN_STATUS_ACCEPTED_PENDING, devid_get_admin_secret());
        return;
    }
    hv9910_enable();
    send_status_resp(sock, src, hdr->nonce, ADMIN_TYPE_ON_RESP, ADMIN_STATUS_OK, devid_get_admin_secret());
}

/**
 * @brief Handles OFF: disables the driver unconditionally. The 4-byte
 * payload (a legacy ramp duration) is accepted but ignored.
 * @param sock UDP socket.
 * @param hdr Parsed request header.
 * @param payload Request payload (ignored ramp_ms).
 * @param payload_len Payload length.
 * @param src Source address.
 * @return None.
 */
static void handle_off(int sock, const parsed_header_t *hdr,
                        const uint8_t *payload, uint16_t payload_len,
                        const struct sockaddr_in *src)
{
    (void)payload;
    if (payload_len != 4) {
        send_status_resp(sock, src, hdr->nonce, ADMIN_TYPE_OFF_RESP, ADMIN_STATUS_ERR_BAD_ARG, devid_get_admin_secret());
        return;
    }
    hv9910_disable();
    send_status_resp(sock, src, hdr->nonce, ADMIN_TYPE_OFF_RESP, ADMIN_STATUS_OK, devid_get_admin_secret());
}

/**
 * @brief Handles DIM: sets brightness, or defers turning on if power isn't
 * confirmed. Payload is percent (1) + a legacy ramp_ms (4, BE) that is
 * accepted but ignored -- the driver is instantaneous.
 * @param sock UDP socket.
 * @param hdr Parsed request header.
 * @param payload Request payload: percent (1) + ignored ramp_ms (4).
 * @param payload_len Payload length (5).
 * @param src Source address.
 * @return None.
 */
static void handle_dim(int sock, const parsed_header_t *hdr,
                        const uint8_t *payload, uint16_t payload_len,
                        const struct sockaddr_in *src)
{
    if (payload_len != 5 || payload[0] > 100) {
        send_status_resp(sock, src, hdr->nonce, ADMIN_TYPE_DIM_RESP, ADMIN_STATUS_ERR_BAD_ARG, devid_get_admin_secret());
        return;
    }
    uint8_t percent = payload[0];

    if (percent == 0) {
        hv9910_disable();
        send_status_resp(sock, src, hdr->nonce, ADMIN_TYPE_DIM_RESP, ADMIN_STATUS_OK, devid_get_admin_secret());
        return;
    }

    if (!tps2378_is_ready() || !power_manager_output_allowed()) {
        hv9910_set_pending(true, percent);   // remembers the level; the driver comes up once power/policy allows
        send_status_resp(sock, src, hdr->nonce, ADMIN_TYPE_DIM_RESP, ADMIN_STATUS_ACCEPTED_PENDING, devid_get_admin_secret());
        return;
    }

    // enable_at clears the power-cut latch, records the desire, lights the driver.
    hv9910_enable_at(percent);
    send_status_resp(sock, src, hdr->nonce, ADMIN_TYPE_DIM_RESP, ADMIN_STATUS_OK, devid_get_admin_secret());
}

/**
 * @brief Handles IDENTIFY: runs the visual identify blink if power is confirmed.
 * @param sock UDP socket.
 * @param hdr Parsed request header.
 * @param src Source address.
 * @return None.
 */
static void handle_identify(int sock, const parsed_header_t *hdr, const struct sockaddr_in *src)
{
    if (!tps2378_is_ready() || !power_manager_output_allowed()) {
        send_status_resp(sock, src, hdr->nonce, ADMIN_TYPE_IDENTIFY_RESP, ADMIN_STATUS_ERR_NOT_READY, devid_get_admin_secret());
        return;
    }

    send_status_resp(sock, src, hdr->nonce, ADMIN_TYPE_IDENTIFY_RESP, ADMIN_STATUS_OK, devid_get_admin_secret());
    hv9910_identify();
}

/**
 * @brief Formats a source IPv4 address for logging.
 * @param src Source address.
 * @param out Destination buffer.
 * @param out_size Size of out.
 * @return None.
 */
static void format_src_ip(const struct sockaddr_in *src, char *out, size_t out_size)
{
    uint32_t ip = ntohl(src->sin_addr.s_addr);
    snprintf(out, out_size, "%u.%u.%u.%u",
             (unsigned)(ip >> 24) & 0xFF, (unsigned)(ip >> 16) & 0xFF,
             (unsigned)(ip >> 8) & 0xFF, (unsigned)ip & 0xFF);
}

/**
 * @brief Handles REBOOT: acknowledges and restarts the device.
 * @param sock UDP socket.
 * @param hdr Parsed request header.
 * @param src Source address.
 * @return Does not return.
 */
static void handle_reboot(int sock, const parsed_header_t *hdr, const struct sockaddr_in *src)
{
    char ip_str[16];
    format_src_ip(src, ip_str, sizeof(ip_str));
    send_status_resp(sock, src, hdr->nonce, ADMIN_TYPE_REBOOT_RESP, ADMIN_STATUS_OK, devid_get_admin_secret());
    ESP_LOGW(TAG, "REBOOT requested remotely by %s", ip_str);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

/**
 * @brief Handles FACTORY_RESET: acknowledges, erases the "nvs" partition, and restarts.
 * @param sock UDP socket.
 * @param hdr Parsed request header.
 * @param src Source address.
 * @return Does not return.
 */
static void handle_factory_reset(int sock, const parsed_header_t *hdr, const struct sockaddr_in *src)
{
    char ip_str[16];
    format_src_ip(src, ip_str, sizeof(ip_str));
    send_status_resp(sock, src, hdr->nonce, ADMIN_TYPE_FACTORY_RESET_RESP, ADMIN_STATUS_OK, devid_get_admin_secret());
    ESP_LOGW(TAG, "FACTORY_RESET requested remotely by %s — erasing the 'nvs' partition", ip_str);
    vTaskDelay(pdMS_TO_TICKS(200));

    esp_err_t err = nvs_flash_erase_partition("nvs");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_erase_partition('nvs') failed (%s) — restarting anyway", esp_err_to_name(err));
    }
    esp_restart();
}

/**
 * @brief Handles CHANGE_SECRET: decrypts and installs a new admin secret.
 * @param sock UDP socket.
 * @param hdr Parsed request header.
 * @param payload Request payload (AES-GCM ciphertext + tag).
 * @param payload_len Payload length.
 * @param src Source address.
 * @param raw_pkt Full raw request packet, used as AAD.
 * @param header_len Length of the header portion of raw_pkt.
 * @return None.
 */
static void handle_change_secret(int sock, const parsed_header_t *hdr,
                                  const uint8_t *payload, uint16_t payload_len,
                                  const struct sockaddr_in *src, const uint8_t *raw_pkt, size_t header_len)
{
    const size_t expected_len = DEVID_SECRET_LEN + ADMIN_GCM_TAG_LEN;
    if (payload_len != expected_len) {
        send_status_resp(sock, src, hdr->nonce, ADMIN_TYPE_CHANGE_SECRET_RESP, ADMIN_STATUS_ERR_BAD_ARG, devid_get_admin_secret());
        return;
    }

    uint8_t new_secret[DEVID_SECRET_LEN];
    size_t plaintext_len = 0;
    psa_status_t status = gcm_decrypt(devid_get_admin_secret(), hdr->nonce, ADMIN_NONCE_LEN,
                                       raw_pkt, header_len,
                                       payload, payload_len,
                                       new_secret, sizeof(new_secret), &plaintext_len);
    if (status != PSA_SUCCESS || plaintext_len != sizeof(new_secret)) {
        send_status_resp(sock, src, hdr->nonce, ADMIN_TYPE_CHANGE_SECRET_RESP, ADMIN_STATUS_ERR_BAD_ARG, devid_get_admin_secret());
        return;
    }

    if (!devid_set_admin_secret(new_secret)) {
        send_status_resp(sock, src, hdr->nonce, ADMIN_TYPE_CHANGE_SECRET_RESP, ADMIN_STATUS_ERR_INTERNAL, devid_get_admin_secret());
        return;
    }

    send_status_resp(sock, src, hdr->nonce, ADMIN_TYPE_CHANGE_SECRET_RESP, ADMIN_STATUS_OK, devid_get_admin_secret());
    ESP_LOGW(TAG, "Admin secret changed remotely");
}

/** @brief State of the single in-flight OTA session, if any. */
typedef struct {
    bool in_progress;                          /**< Whether a session is active. */
    esp_ota_handle_t handle;                   /**< esp_ota write handle. */
    const esp_partition_t *partition;          /**< Target OTA partition. */
    uint32_t total_size;                       /**< Announced total image size. */
    uint32_t bytes_written;                    /**< Bytes confirmed written so far. */
    uint8_t sha256_expected[ADMIN_OTA_SHA256_LEN]; /**< Expected full-image SHA-256. */
    psa_hash_operation_t hash_op;              /**< Incremental SHA-256 state. */
    int64_t last_activity_us;                  /**< Timestamp of the last OTA_CHUNK. */
} ota_state_t;

static ota_state_t s_ota;

/**
 * @brief Tears down the in-progress OTA session, if any.
 * @param abort_handle true to also call esp_ota_abort() on the OTA handle.
 * @return None.
 */
static void ota_reset(bool abort_handle)
{
    if (s_ota.in_progress) {
        if (abort_handle) {
            esp_ota_abort(s_ota.handle);
        }
        psa_hash_abort(&s_ota.hash_op);
    }
    memset(&s_ota, 0, sizeof(s_ota));
}

/**
 * @brief Sends an OTA_CHUNK_RESP with status and bytes-written.
 * @param sock UDP socket.
 * @param src Destination address.
 * @param nonce Nonce to echo back.
 * @param status Status code.
 * @param bytes_written Bytes confirmed written so far.
 * @return None.
 */
static void send_ota_chunk_resp(int sock, const struct sockaddr_in *src, const uint8_t nonce[ADMIN_NONCE_LEN],
                                 admin_status_t status, uint32_t bytes_written)
{
    uint8_t payload[5];
    payload[0] = (uint8_t)status;
    put_u32_be(payload + 1, bytes_written);
    send_packet(sock, src, ADMIN_TYPE_OTA_CHUNK_RESP, nonce, payload, sizeof(payload), devid_get_admin_secret());
}

/**
 * @brief Handles OTA_BEGIN: starts a new OTA session.
 * @param sock UDP socket.
 * @param hdr Parsed request header.
 * @param payload Request payload (total_size, sha256).
 * @param payload_len Payload length.
 * @param src Source address.
 * @return None.
 */
static void handle_ota_begin(int sock, const parsed_header_t *hdr,
                              const uint8_t *payload, uint16_t payload_len,
                              const struct sockaddr_in *src)
{
    if (payload_len != 4 + ADMIN_OTA_SHA256_LEN) {
        send_status_resp(sock, src, hdr->nonce, ADMIN_TYPE_OTA_BEGIN_RESP, ADMIN_STATUS_ERR_BAD_ARG, devid_get_admin_secret());
        return;
    }

    int64_t now = esp_timer_get_time();
    if (s_ota.in_progress) {
        if ((now - s_ota.last_activity_us) < OTA_IDLE_TIMEOUT_US) {
            send_status_resp(sock, src, hdr->nonce, ADMIN_TYPE_OTA_BEGIN_RESP, ADMIN_STATUS_ERR_BUSY, devid_get_admin_secret());
            return;
        }
        ESP_LOGW(TAG, "Reclaiming a stale OTA session (idle for over %ds)", (int)(OTA_IDLE_TIMEOUT_US / 1000000));
        ota_reset(true);
    }

    uint32_t total_size = get_u32_be(payload);
    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (partition == NULL || total_size == 0 || total_size > partition->size) {
        send_status_resp(sock, src, hdr->nonce, ADMIN_TYPE_OTA_BEGIN_RESP, ADMIN_STATUS_ERR_BAD_ARG, devid_get_admin_secret());
        return;
    }

    esp_ota_handle_t handle;
    esp_err_t err = esp_ota_begin(partition, total_size, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin() failed (%s)", esp_err_to_name(err));
        send_status_resp(sock, src, hdr->nonce, ADMIN_TYPE_OTA_BEGIN_RESP, ADMIN_STATUS_ERR_INTERNAL, devid_get_admin_secret());
        return;
    }

    memset(&s_ota, 0, sizeof(s_ota));
    s_ota.in_progress = true;
    s_ota.handle = handle;
    s_ota.partition = partition;
    s_ota.total_size = total_size;
    memcpy(s_ota.sha256_expected, payload + 4, ADMIN_OTA_SHA256_LEN);
    s_ota.last_activity_us = now;
    psa_hash_setup(&s_ota.hash_op, PSA_ALG_SHA_256);

    ESP_LOGW(TAG, "OTA started: %" PRIu32 " bytes -> partition '%s'", total_size, partition->label);
    send_status_resp(sock, src, hdr->nonce, ADMIN_TYPE_OTA_BEGIN_RESP, ADMIN_STATUS_OK, devid_get_admin_secret());
}

/**
 * @brief Handles OTA_CHUNK: writes one chunk of image data.
 * @param sock UDP socket.
 * @param hdr Parsed request header.
 * @param payload Request payload (offset, data).
 * @param payload_len Payload length.
 * @param src Source address.
 * @return None.
 */
static void handle_ota_chunk(int sock, const parsed_header_t *hdr,
                              const uint8_t *payload, uint16_t payload_len,
                              const struct sockaddr_in *src)
{
    if (!s_ota.in_progress || payload_len < 4) {
        send_ota_chunk_resp(sock, src, hdr->nonce, ADMIN_STATUS_ERR_BAD_ARG, s_ota.bytes_written);
        return;
    }

    uint32_t offset = get_u32_be(payload);
    const uint8_t *data = payload + 4;
    uint16_t data_len = payload_len - 4;

    if (offset < s_ota.bytes_written) {
        send_ota_chunk_resp(sock, src, hdr->nonce, ADMIN_STATUS_OK, s_ota.bytes_written);
        return;
    }
    if (offset != s_ota.bytes_written || (uint64_t)offset + data_len > s_ota.total_size) {
        send_ota_chunk_resp(sock, src, hdr->nonce, ADMIN_STATUS_ERR_BAD_ARG, s_ota.bytes_written);
        return;
    }

    esp_err_t err = esp_ota_write(s_ota.handle, data, data_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write() failed (%s) -- aborting OTA", esp_err_to_name(err));
        ota_reset(true);
        send_ota_chunk_resp(sock, src, hdr->nonce, ADMIN_STATUS_ERR_INTERNAL, 0);
        return;
    }
    psa_hash_update(&s_ota.hash_op, data, data_len);
    s_ota.bytes_written += data_len;
    s_ota.last_activity_us = esp_timer_get_time();

    send_ota_chunk_resp(sock, src, hdr->nonce, ADMIN_STATUS_OK, s_ota.bytes_written);
}

/**
 * @brief Handles OTA_END: validates and finalizes the OTA session.
 * @param sock UDP socket.
 * @param hdr Parsed request header.
 * @param src Source address.
 * @return None.
 */
static void handle_ota_end(int sock, const parsed_header_t *hdr, const struct sockaddr_in *src)
{
    if (!s_ota.in_progress) {
        send_status_resp(sock, src, hdr->nonce, ADMIN_TYPE_OTA_END_RESP, ADMIN_STATUS_ERR_BAD_ARG, devid_get_admin_secret());
        return;
    }
    if (s_ota.bytes_written != s_ota.total_size) {
        ESP_LOGW(TAG, "OTA_END with an incomplete transfer (%" PRIu32 "/%" PRIu32 " bytes) -- aborting",
                 s_ota.bytes_written, s_ota.total_size);
        ota_reset(true);
        send_status_resp(sock, src, hdr->nonce, ADMIN_TYPE_OTA_END_RESP, ADMIN_STATUS_ERR_BAD_ARG, devid_get_admin_secret());
        return;
    }

    uint8_t digest[ADMIN_OTA_SHA256_LEN];
    size_t digest_len = 0;
    psa_status_t hash_status = psa_hash_finish(&s_ota.hash_op, digest, sizeof(digest), &digest_len);
    bool hash_ok = (hash_status == PSA_SUCCESS && digest_len == sizeof(digest) &&
                     memcmp(digest, s_ota.sha256_expected, sizeof(digest)) == 0);
    if (!hash_ok) {
        ESP_LOGE(TAG, "OTA image SHA-256 mismatch -- rejecting, boot partition unchanged");
        esp_ota_abort(s_ota.handle);
        memset(&s_ota, 0, sizeof(s_ota));
        send_status_resp(sock, src, hdr->nonce, ADMIN_TYPE_OTA_END_RESP, ADMIN_STATUS_ERR_BAD_ARG, devid_get_admin_secret());
        return;
    }

    esp_err_t err = esp_ota_end(s_ota.handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end() failed (%s) -- image rejected, boot partition unchanged", esp_err_to_name(err));
        memset(&s_ota, 0, sizeof(s_ota));
        send_status_resp(sock, src, hdr->nonce, ADMIN_TYPE_OTA_END_RESP, ADMIN_STATUS_ERR_INTERNAL, devid_get_admin_secret());
        return;
    }

    err = esp_ota_set_boot_partition(s_ota.partition);
    char partition_label[sizeof(s_ota.partition->label)];
    strncpy(partition_label, s_ota.partition->label, sizeof(partition_label));
    memset(&s_ota, 0, sizeof(s_ota));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition() failed (%s)", esp_err_to_name(err));
        send_status_resp(sock, src, hdr->nonce, ADMIN_TYPE_OTA_END_RESP, ADMIN_STATUS_ERR_INTERNAL, devid_get_admin_secret());
        return;
    }

    ESP_LOGW(TAG, "OTA image validated and staged on '%s' -- will boot into it on the next REBOOT", partition_label);
    send_status_resp(sock, src, hdr->nonce, ADMIN_TYPE_OTA_END_RESP, ADMIN_STATUS_OK, devid_get_admin_secret());
}

/**
 * @brief Handles OTA_ABORT: cancels the in-progress OTA session, if any.
 * @param sock UDP socket.
 * @param hdr Parsed request header.
 * @param src Source address.
 * @return None.
 */
static void handle_ota_abort(int sock, const parsed_header_t *hdr, const struct sockaddr_in *src)
{
    if (s_ota.in_progress) {
        ESP_LOGW(TAG, "OTA aborted by request (%" PRIu32 "/%" PRIu32 " bytes had arrived)",
                 s_ota.bytes_written, s_ota.total_size);
        ota_reset(true);
    }
    send_status_resp(sock, src, hdr->nonce, ADMIN_TYPE_OTA_ABORT_RESP, ADMIN_STATUS_OK, devid_get_admin_secret());
}

/**
 * @brief Handles DMX_GET_CONFIG: returns the serialized DMX layer config.
 * @param sock UDP socket.
 * @param hdr Parsed request header.
 * @param src Source address.
 * @return None.
 */
static void handle_dmx_get_config(int sock, const parsed_header_t *hdr, const struct sockaddr_in *src)
{
    dmx_input_config_t cfg;
    if (!dmx_input_get_config(&cfg)) {
        send_status_resp(sock, src, hdr->nonce, ADMIN_TYPE_DMX_GET_CONFIG_RESP, ADMIN_STATUS_ERR_INTERNAL, devid_get_admin_secret());
        return;
    }
    uint8_t buf[DMX_CFG_WIRE_SIZE];
    dmx_config_pack(&cfg, buf);
    send_packet(sock, src, ADMIN_TYPE_DMX_GET_CONFIG_RESP, hdr->nonce, buf, sizeof(buf), devid_get_admin_secret());
}

/**
 * @brief Handles DMX_SET_CONFIG: applies and persists a new DMX layer config.
 * @param sock UDP socket.
 * @param hdr Parsed request header.
 * @param payload Serialized config (DMX_CFG_WIRE_SIZE bytes).
 * @param payload_len Payload length.
 * @param src Source address.
 * @return None.
 */
static void handle_dmx_set_config(int sock, const parsed_header_t *hdr,
                                   const uint8_t *payload, uint16_t payload_len,
                                   const struct sockaddr_in *src)
{
    dmx_input_config_t cfg;
    if (payload_len != DMX_CFG_WIRE_SIZE || !dmx_config_unpack(payload, payload_len, &cfg)) {
        send_status_resp(sock, src, hdr->nonce, ADMIN_TYPE_DMX_SET_CONFIG_RESP, ADMIN_STATUS_ERR_BAD_ARG, devid_get_admin_secret());
        return;
    }
    if (!dmx_input_set_config(&cfg)) {
        send_status_resp(sock, src, hdr->nonce, ADMIN_TYPE_DMX_SET_CONFIG_RESP, ADMIN_STATUS_ERR_INTERNAL, devid_get_admin_secret());
        return;
    }
    send_status_resp(sock, src, hdr->nonce, ADMIN_TYPE_DMX_SET_CONFIG_RESP, ADMIN_STATUS_OK, devid_get_admin_secret());
}

/**
 * @brief Handles DRIVER_GET_CONFIG: returns the serialized dimming config.
 * @param sock UDP socket.
 * @param hdr Parsed request header.
 * @param src Source address.
 * @return None.
 */
static void handle_driver_get_config(int sock, const parsed_header_t *hdr, const struct sockaddr_in *src)
{
    driver_config_t cfg;
    if (!driver_config_get(&cfg)) {
        send_status_resp(sock, src, hdr->nonce, ADMIN_TYPE_DRIVER_GET_CONFIG_RESP, ADMIN_STATUS_ERR_INTERNAL, devid_get_admin_secret());
        return;
    }
    uint8_t buf[DRV_CFG_WIRE_SIZE];
    driver_config_pack(&cfg, buf);
    send_packet(sock, src, ADMIN_TYPE_DRIVER_GET_CONFIG_RESP, hdr->nonce, buf, sizeof(buf), devid_get_admin_secret());
}

/**
 * @brief Handles DRIVER_SET_CONFIG: applies and persists a new dimming config.
 * @param sock UDP socket.
 * @param hdr Parsed request header.
 * @param payload Serialized config (DRV_CFG_WIRE_SIZE bytes).
 * @param payload_len Payload length.
 * @param src Source address.
 * @return None.
 */
static void handle_driver_set_config(int sock, const parsed_header_t *hdr,
                                      const uint8_t *payload, uint16_t payload_len,
                                      const struct sockaddr_in *src)
{
    driver_config_t cfg;
    if (payload_len != DRV_CFG_WIRE_SIZE || !driver_config_unpack(payload, payload_len, &cfg)) {
        send_status_resp(sock, src, hdr->nonce, ADMIN_TYPE_DRIVER_SET_CONFIG_RESP, ADMIN_STATUS_ERR_BAD_ARG, devid_get_admin_secret());
        return;
    }
    if (!driver_config_set(&cfg)) {
        send_status_resp(sock, src, hdr->nonce, ADMIN_TYPE_DRIVER_SET_CONFIG_RESP, ADMIN_STATUS_ERR_INTERNAL, devid_get_admin_secret());
        return;
    }
    /* power_mode / poe_cap_pct may have changed -- re-run the policy now so a
     * live PoE+-required toggle or cap change takes effect without a reboot. */
    power_manager_reeval();
    send_status_resp(sock, src, hdr->nonce, ADMIN_TYPE_DRIVER_SET_CONFIG_RESP, ADMIN_STATUS_OK, devid_get_admin_secret());
}

/**
 * @brief Validates, authenticates, and dispatches one received packet.
 * @param sock UDP socket.
 * @param buf Raw packet buffer.
 * @param len Packet length.
 * @param src Source address.
 * @return None.
 */
static void handle_packet(int sock, uint8_t *buf, size_t len, const struct sockaddr_in *src)
{
    if (!rate_limit_check(ntohl(src->sin_addr.s_addr))) {
        return;
    }

    parsed_header_t hdr;
    if (!parse_header(buf, len, &hdr)) {
        return;
    }
    if (hdr.magic != ADMIN_PROTO_MAGIC || hdr.version != ADMIN_PROTO_VERSION) {
        return;
    }
    if (hdr.payload_len > ADMIN_MAX_PAYLOAD ||
        len != (ADMIN_HEADER_WIRE_SIZE + (size_t)hdr.payload_len + ADMIN_HMAC_LEN)) {
        return;
    }

    const uint8_t *payload = buf + ADMIN_HEADER_WIRE_SIZE;
    const uint8_t *hmac = buf + ADMIN_HEADER_WIRE_SIZE + hdr.payload_len;
    const size_t hmac_covered_len = ADMIN_HEADER_WIRE_SIZE + hdr.payload_len;

    if (hdr.type == ADMIN_TYPE_INFO) {
        handle_info(sock, &hdr, src);
        return;
    }

    char my_serial_padded[ADMIN_SERIAL_LEN];
    memset(my_serial_padded, 0, sizeof(my_serial_padded));
    strncpy(my_serial_padded, devid_get_serial(), sizeof(my_serial_padded) - 1);
    if (memcmp(hdr.serial, my_serial_padded, ADMIN_SERIAL_LEN) != 0) {
        return;
    }

    if (hmac_verify(devid_get_admin_secret(), buf, hmac_covered_len, hmac, ADMIN_HMAC_LEN) != PSA_SUCCESS) {
        return;
    }

    bool needs_pool_nonce = (hdr.type != ADMIN_TYPE_CHALLENGE && hdr.type != ADMIN_TYPE_OTA_CHUNK);
    if (needs_pool_nonce && !nonce_consume(ntohl(src->sin_addr.s_addr), hdr.nonce)) {
        return;
    }

    switch (hdr.type) {
    case ADMIN_TYPE_CHALLENGE:
        handle_challenge(sock, &hdr, src);
        break;
    case ADMIN_TYPE_ON:
        handle_on(sock, &hdr, payload, hdr.payload_len, src);
        break;
    case ADMIN_TYPE_OFF:
        handle_off(sock, &hdr, payload, hdr.payload_len, src);
        break;
    case ADMIN_TYPE_DIM:
        handle_dim(sock, &hdr, payload, hdr.payload_len, src);
        break;
    case ADMIN_TYPE_IDENTIFY:
        handle_identify(sock, &hdr, src);
        break;
    case ADMIN_TYPE_REBOOT:
        handle_reboot(sock, &hdr, src);
        break;
    case ADMIN_TYPE_OTA_BEGIN:
        handle_ota_begin(sock, &hdr, payload, hdr.payload_len, src);
        break;
    case ADMIN_TYPE_OTA_CHUNK:
        handle_ota_chunk(sock, &hdr, payload, hdr.payload_len, src);
        break;
    case ADMIN_TYPE_OTA_END:
        handle_ota_end(sock, &hdr, src);
        break;
    case ADMIN_TYPE_OTA_ABORT:
        handle_ota_abort(sock, &hdr, src);
        break;
    case ADMIN_TYPE_FACTORY_RESET:
        handle_factory_reset(sock, &hdr, src);
        break;
    case ADMIN_TYPE_CHANGE_SECRET:
        handle_change_secret(sock, &hdr, payload, hdr.payload_len, src, buf, ADMIN_HEADER_WIRE_SIZE);
        break;
    case ADMIN_TYPE_DMX_GET_CONFIG:
        handle_dmx_get_config(sock, &hdr, src);
        break;
    case ADMIN_TYPE_DMX_SET_CONFIG:
        handle_dmx_set_config(sock, &hdr, payload, hdr.payload_len, src);
        break;
    case ADMIN_TYPE_DRIVER_GET_CONFIG:
        handle_driver_get_config(sock, &hdr, src);
        break;
    case ADMIN_TYPE_DRIVER_SET_CONFIG:
        handle_driver_set_config(sock, &hdr, payload, hdr.payload_len, src);
        break;
    default:
        send_status_resp(sock, src, hdr.nonce, ADMIN_TYPE_ERR_RESP, ADMIN_STATUS_ERR_BAD_ARG, devid_get_admin_secret());
        break;
    }
}

/**
 * @brief Admin channel task: opens the UDP socket and dispatches received packets.
 * @param arg Unused.
 * @return Never returns.
 */
static void admin_channel_task(void *arg)
{
    (void)arg;

    psa_status_t psa_status = psa_crypto_init();
    if (psa_status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init failed (%d) — admin channel can't start", (int)psa_status);
        vTaskDelete(NULL);
        return;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create UDP socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(s_config.port);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind() failed: errno %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Admin channel listening on UDP:%u (serial %s)", s_config.port, devid_get_serial());

    static uint8_t rx_buf[ADMIN_MAX_PACKET];
    while (1) {
        struct sockaddr_in src_addr;
        socklen_t addr_len = sizeof(src_addr);
        int len = recvfrom(sock, rx_buf, sizeof(rx_buf), 0, (struct sockaddr *)&src_addr, &addr_len);
        if (len < 0) {
            ESP_LOGE(TAG, "recvfrom() failed: errno %d", errno);
            continue;
        }
        if (len == 0) {
            continue;
        }
        handle_packet(sock, rx_buf, (size_t)len, &src_addr);
    }
}

void admin_channel_start(const admin_channel_config_t *config)
{
    s_config = *config;

    BaseType_t ok = xTaskCreate(admin_channel_task, "admin_channel", TASK_STACK_SIZE, NULL, TASK_PRIORITY, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(admin_channel) failed -- the admin channel will not run");
    }
}
