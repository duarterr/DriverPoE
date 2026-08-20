/*
 * Authenticated UDP admin channel — see admin_channel.h and
 * README.md for the full context/wire layout.
 *
 * All serialization is manual, byte by byte, big-endian — never a direct
 * struct cast over the network buffer (avoids depending on the
 * compiler's alignment/endianness/padding).
 */
#include "admin_channel.h"
#include "admin_protocol.h"
#include "poe_luminaire.h"
#include "devid.h"
#include "poe_negotiator.h"
#include "hv9910.h"
#include "eth_init.h"
#include "voltage_sense.h"

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
#include "nvs_flash.h"

#include "lwip/sockets.h"

#include "psa/crypto.h"

static const char *TAG = "ADMIN_CH";

/* ---------------------------------------------------------------------
 * Module tuning parameters (not "board config" — kept here, not in
 * poe_luminaire.h, same pattern as poe_negotiator.c).
 * --------------------------------------------------------------------- */
#define TASK_STACK_SIZE          4096
#define TASK_PRIORITY            (tskIDLE_PRIORITY + 5) /* higher than cmd_server_task (+3) and poe_monitor (+4) */

#define NONCE_POOL_SIZE          8
#define NONCE_TTL_US             (10 * 1000000LL)   /* a CHALLENGE nonce expires after 10s if unused */
/* ROTATE_CONFIRM doesn't use this pool — its nonce is fixed: the same one
 * from the ROTATE_KEY that originated the staging (see
 * devid_rotate_get_staged_nonce()). */

#define RATE_LIMIT_TABLE_SIZE    16
#define RATE_LIMIT_WINDOW_US     (1 * 1000000LL)
#define RATE_LIMIT_MAX_PER_WINDOW 20 /* per source IP, per 1s window */

#define IDENTIFY_TASK_STACK      2048
#define IDENTIFY_BLINK_CYCLES    6
#define IDENTIFY_BLINK_PERIOD_MS 400

/* ---------------------------------------------------------------------
 * Big-endian serialization
 * --------------------------------------------------------------------- */
static void put_u16_be(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put_u32_be(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint16_t get_u16_be(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
static uint32_t get_u32_be(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  type;
    char     serial[ADMIN_SERIAL_LEN];
    uint32_t epoch;
    uint8_t  nonce[ADMIN_NONCE_LEN];
    uint16_t payload_len;
} parsed_header_t;

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
    out->epoch = get_u32_be(buf + off); off += 4;
    memcpy(out->nonce, buf + off, ADMIN_NONCE_LEN); off += ADMIN_NONCE_LEN;
    out->payload_len = get_u16_be(buf + off); off += 2;
    return true;
}

/* ---------------------------------------------------------------------
 * PSA Crypto — imports/uses/destroys the key on every operation. Simple
 * and safe (no "live" key handle that would need to be re-synced whenever
 * the active key changes via rotation); the extra CPU cost per packet is
 * acceptable for a low-rate protocol like this one.
 * --------------------------------------------------------------------- */
static psa_status_t hmac_verify(const uint8_t *key, const uint8_t *data, size_t data_len,
                                 const uint8_t *mac, size_t mac_len)
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_VERIFY_MESSAGE);
    psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attr, DEVID_KEY_LEN * 8);

    mbedtls_svc_key_id_t key_id;
    psa_status_t status = psa_import_key(&attr, key, DEVID_KEY_LEN, &key_id);
    if (status != PSA_SUCCESS) {
        return status;
    }
    /* psa_mac_verify() is specified to run in constant time — no manual
     * comparison here. */
    status = psa_mac_verify(key_id, PSA_ALG_HMAC(PSA_ALG_SHA_256), data, data_len, mac, mac_len);
    psa_destroy_key(key_id);
    return status;
}

static psa_status_t hmac_compute(const uint8_t *key, const uint8_t *data, size_t data_len,
                                  uint8_t *mac_out, size_t mac_out_size, size_t *mac_len_out)
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attr, DEVID_KEY_LEN * 8);

    mbedtls_svc_key_id_t key_id;
    psa_status_t status = psa_import_key(&attr, key, DEVID_KEY_LEN, &key_id);
    if (status != PSA_SUCCESS) {
        return status;
    }
    status = psa_mac_compute(key_id, PSA_ALG_HMAC(PSA_ALG_SHA_256), data, data_len,
                              mac_out, mac_out_size, mac_len_out);
    psa_destroy_key(key_id);
    return status;
}

static psa_status_t gcm_decrypt(const uint8_t *key, const uint8_t *nonce, size_t nonce_len,
                                 const uint8_t *aad, size_t aad_len,
                                 const uint8_t *ciphertext, size_t ciphertext_len,
                                 uint8_t *plaintext_out, size_t plaintext_size, size_t *plaintext_len_out)
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attr, PSA_ALG_GCM);
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, DEVID_KEY_LEN * 8);

    mbedtls_svc_key_id_t key_id;
    psa_status_t status = psa_import_key(&attr, key, DEVID_KEY_LEN, &key_id);
    if (status != PSA_SUCCESS) {
        return status;
    }
    status = psa_aead_decrypt(key_id, PSA_ALG_GCM, nonce, nonce_len, aad, aad_len,
                               ciphertext, ciphertext_len, plaintext_out, plaintext_size, plaintext_len_out);
    psa_destroy_key(key_id);
    return status;
}

/* ---------------------------------------------------------------------
 * Challenge-response nonce pool (CHALLENGE -> REBOOT/FACTORY_RESET/
 * ROTATE_KEY). Single use, self-expiring, never persisted (no RTC).
 * --------------------------------------------------------------------- */
typedef struct {
    bool used;
    uint8_t nonce[ADMIN_NONCE_LEN];
    int64_t issued_at_us;
} nonce_slot_t;

static nonce_slot_t s_nonces[NONCE_POOL_SIZE];

static void nonce_issue(uint8_t out[ADMIN_NONCE_LEN])
{
    psa_generate_random(out, ADMIN_NONCE_LEN);

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
    s_nonces[victim].issued_at_us = now;
    memcpy(s_nonces[victim].nonce, out, ADMIN_NONCE_LEN);
}

/* Consumes (invalidates) a nonce if it exists in the pool and hasn't
 * expired yet. Returns false (and consumes nothing) otherwise. */
static bool nonce_consume(const uint8_t nonce[ADMIN_NONCE_LEN])
{
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < NONCE_POOL_SIZE; i++) {
        if (!s_nonces[i].used) {
            continue;
        }
        if ((now - s_nonces[i].issued_at_us) > NONCE_TTL_US) {
            s_nonces[i].used = false; /* expired: free the slot */
            continue;
        }
        if (memcmp(s_nonces[i].nonce, nonce, ADMIN_NONCE_LEN) == 0) {
            s_nonces[i].used = false; /* single use */
            return true;
        }
    }
    return false;
}

/* ---------------------------------------------------------------------
 * Simple per-source-IP rate limiting — fixed table, no allocation.
 * --------------------------------------------------------------------- */
typedef struct {
    uint32_t ip;    /* 0 = free slot */
    int count;
    int64_t window_start_us;
} rate_entry_t;

static rate_entry_t s_rate_table[RATE_LIMIT_TABLE_SIZE];

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
    /* New IP: reuse a free slot, or the oldest one if the table is full
     * (acceptable: only slightly relaxes the limit in that rare case,
     * never lets traffic through with zero control at all). */
    int slot = (free_slot >= 0) ? free_slot : 0;
    s_rate_table[slot].ip = ip;
    s_rate_table[slot].window_start_us = now;
    s_rate_table[slot].count = 1;
    return true;
}

/* ---------------------------------------------------------------------
 * Sending packets
 * --------------------------------------------------------------------- */

/* hmac_key == NULL -> HMAC filled with zero (only used for DISCOVER_RESP,
 * which is unauthenticated by protocol definition). */
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

    put_u32_be(buf + off, devid_get_epoch()); off += 4;

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

    if (hmac_key) {
        size_t mac_len = 0;
        hmac_compute(hmac_key, buf, off, buf + off, ADMIN_HMAC_LEN, &mac_len);
    } else {
        memset(buf + off, 0, ADMIN_HMAC_LEN);
    }
    off += ADMIN_HMAC_LEN;

    sendto(sock, buf, off, 0, (const struct sockaddr *)dst, sizeof(*dst));
}

static void send_status_resp(int sock, const struct sockaddr_in *dst, const uint8_t nonce[ADMIN_NONCE_LEN],
                              admin_pkt_type_t type, admin_status_t status, const uint8_t *hmac_key)
{
    uint8_t payload = (uint8_t)status;
    send_packet(sock, dst, type, nonce, &payload, 1, hmac_key);
}

/* ---------------------------------------------------------------------
 * Command handlers
 * --------------------------------------------------------------------- */

static void handle_discover(int sock, const parsed_header_t *hdr, const struct sockaddr_in *src)
{
    /* Short random delay to avoid a collision when several units answer
     * the same broadcast at once. */
    uint8_t r = 0;
    psa_generate_random(&r, 1);
    vTaskDelay(pdMS_TO_TICKS(r % 50));

    uint8_t payload[41];
    memset(payload, 0, sizeof(payload));
    strncpy((char *)payload, DEVID_MODEL_PREFIX, 16);
    put_u32_be(payload + 16, devid_get_epoch());

    const esp_app_desc_t *app = esp_app_get_description();
    strncpy((char *)payload + 20, app->version, 16);

    esp_netif_ip_info_t ip_info;
    if (eth_get_ip_info(&ip_info)) {
        memcpy(payload + 36, &ip_info.ip.addr, 4);
    }
    payload[40] = devid_is_provisioned() ? 1 : 0;

    send_packet(sock, src, ADMIN_TYPE_DISCOVER_RESP, hdr->nonce, payload, sizeof(payload), NULL);
}

static void handle_challenge(int sock, const parsed_header_t *hdr, const struct sockaddr_in *src)
{
    (void)hdr;
    uint8_t new_nonce[ADMIN_NONCE_LEN];
    nonce_issue(new_nonce);
    /* Only ever dispatched for a provisioned unit (CLAIM, the one
     * pre-provisioning command, doesn't need a nonce at all -- see
     * handle_packet()), so the active key is always the right one here. */
    send_packet(sock, src, ADMIN_TYPE_CHALLENGE_RESP, new_nonce, NULL, 0, devid_get_key());
}

static void handle_status(int sock, const parsed_header_t *hdr, const struct sockaddr_in *src)
{
    uint8_t payload[21];
    size_t off = 0;

    voltage_reading_t v = {0};
    voltage_sense_read(&v);

    put_u32_be(payload + off, (uint32_t)(esp_timer_get_time() / 1000000)); off += 4;
    payload[off++] = (uint8_t)esp_reset_reason();
    payload[off++] = poe_negotiator_is_ready() ? 1 : 0;
    payload[off++] = (uint8_t)poe_negotiator_get_source();
    payload[off++] = hv9910_is_enabled() ? 1 : 0;
    payload[off++] = hv9910_get_dim_percent();
    put_u32_be(payload + off, (uint32_t)poe_negotiator_get_vbus_mv()); off += 4;
    put_u32_be(payload + off, (uint32_t)v.led_voltage_mv); off += 4; /* bits reinterpreted as int32_t by the reader */

    esp_netif_ip_info_t ip_info;
    memset(payload + off, 0, 4);
    if (eth_get_ip_info(&ip_info)) {
        memcpy(payload + off, &ip_info.ip.addr, 4);
    }
    off += 4;

    send_packet(sock, src, ADMIN_TYPE_STATUS_RESP, hdr->nonce, payload, (uint16_t)off, devid_get_key());
}

/* Short, independent task for the IDENTIFY blink so it doesn't block the
 * admin channel task while it runs (a few seconds). Only uses hv9910's
 * existing public API — doesn't change any hardware control logic. */
static void identify_blink_task(void *arg)
{
    (void)arg;
    bool was_enabled = hv9910_is_enabled();
    uint8_t was_dim = hv9910_get_dim_percent();

    if (!was_enabled) {
        /* hv9910_set_dim() alone only changes the PWM duty (DIM pin) -- it
         * never touches SHUTDOWN. Without releasing it here, blinking
         * while the driver was off (the common case, e.g. right after
         * boot with nothing turned on yet) would silently produce no
         * visible light at all. ramp_ms=0 and persist=false -- this is a
         * transient blink, not the operator asking for the driver to stay
         * on, so it must NOT change the remembered on/off state, and
         * shouldn't fade in before the blink even starts. */
        hv9910_enable(0, false);
    }

    /* Ramp=0 (instant) on purpose here -- this is meant to read as a
     * sharp, unmistakable blink for physical identification, not a soft
     * fade like a normal dimming change. */
    for (int i = 0; i < IDENTIFY_BLINK_CYCLES; i++) {
        hv9910_set_dim(0, 0);
        vTaskDelay(pdMS_TO_TICKS(IDENTIFY_BLINK_PERIOD_MS));
        hv9910_set_dim(100, 0);
        vTaskDelay(pdMS_TO_TICKS(IDENTIFY_BLINK_PERIOD_MS));
    }

    /* Restore exactly the prior state -- persist=false, same reasoning as
     * above: this transient blink leaves the remembered on/off state
     * exactly as it was. */
    if (was_enabled) {
        hv9910_set_dim(was_dim, 0);
    } else {
        hv9910_disable(0, false);
    }

    vTaskDelete(NULL);
}

static void handle_identify(int sock, const parsed_header_t *hdr, const struct sockaddr_in *src)
{
    /* Same safety rule as ON/DIM in cmd_server.c: never force the driver
     * to run outside the PoE/AUX/VBUS gate. */
    if (!poe_negotiator_is_ready()) {
        send_status_resp(sock, src, hdr->nonce, ADMIN_TYPE_IDENTIFY_RESP, ADMIN_STATUS_ERR_NOT_READY, devid_get_key());
        return;
    }

    send_status_resp(sock, src, hdr->nonce, ADMIN_TYPE_IDENTIFY_RESP, ADMIN_STATUS_OK, devid_get_key());
    xTaskCreate(identify_blink_task, "identify_blink", IDENTIFY_TASK_STACK, NULL, tskIDLE_PRIORITY + 2, NULL);
}

/* Formats the source IPv4 for logging without depending on inet_ntoa()
 * (avoids one more include/thread-safety concern from its static internal
 * buffer). */
static void format_src_ip(const struct sockaddr_in *src, char *out, size_t out_size)
{
    uint32_t ip = ntohl(src->sin_addr.s_addr);
    snprintf(out, out_size, "%u.%u.%u.%u",
             (unsigned)(ip >> 24) & 0xFF, (unsigned)(ip >> 16) & 0xFF,
             (unsigned)(ip >> 8) & 0xFF, (unsigned)ip & 0xFF);
}

static void handle_reboot(int sock, const parsed_header_t *hdr, const struct sockaddr_in *src)
{
    char ip_str[16];
    format_src_ip(src, ip_str, sizeof(ip_str));
    send_status_resp(sock, src, hdr->nonce, ADMIN_TYPE_REBOOT_RESP, ADMIN_STATUS_OK, devid_get_key());
    ESP_LOGW(TAG, "REBOOT requested remotely by %s", ip_str);
    vTaskDelay(pdMS_TO_TICKS(200)); /* give the UDP response time to go out before restarting */
    esp_restart();
}

static void handle_factory_reset(int sock, const parsed_header_t *hdr, const struct sockaddr_in *src)
{
    char ip_str[16];
    format_src_ip(src, ip_str, sizeof(ip_str));
    send_status_resp(sock, src, hdr->nonce, ADMIN_TYPE_FACTORY_RESET_RESP, ADMIN_STATUS_OK, devid_get_key());
    ESP_LOGW(TAG, "FACTORY_RESET requested remotely by %s — erasing the 'nvs' partition ('idnvs' preserved)",
             ip_str);
    vTaskDelay(pdMS_TO_TICKS(200));

    /* TODO(matter): once esp-matter is integrated, check where its fabric
     * data (should be wiped by a factory reset, same as here) and its
     * device attestation credentials/DAC (must NEVER be wiped — usually
     * factory-provisioned once, like our own "idnvs") actually live. If
     * esp-matter keeps both in the "nvs" partition without a clean
     * namespace split, prefer calling esp-matter's own factory-reset
     * entry point (e.g. esp_matter::factory_reset()) here instead of the
     * raw whole-partition erase below, so the Matter stack decides
     * exactly what to keep. */
    esp_err_t err = nvs_flash_erase_partition("nvs");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_erase_partition('nvs') failed (%s) — restarting anyway", esp_err_to_name(err));
    }
    esp_restart();
}

static void handle_rotate_key(int sock, const parsed_header_t *hdr,
                               const uint8_t *payload, uint16_t payload_len,
                               const struct sockaddr_in *src, const uint8_t *raw_pkt, size_t header_len)
{
    /* payload = AES-256-GCM(new_key[32] || new_epoch[4]) with a 16-byte
     * tag appended = 52 bytes. GCM nonce = the packet's own nonce field.
     * AAD = the entire header (binds the new key to this specific
     * serial/epoch/nonce). */
    const size_t expected_len = DEVID_KEY_LEN + 4 + ADMIN_GCM_TAG_LEN;
    if (payload_len != expected_len) {
        send_status_resp(sock, src, hdr->nonce, ADMIN_TYPE_ROTATE_KEY_RESP, ADMIN_STATUS_ERR_BAD_ARG, devid_get_key());
        return;
    }

    uint8_t plaintext[DEVID_KEY_LEN + 4];
    size_t plaintext_len = 0;
    psa_status_t status = gcm_decrypt(devid_get_key(), hdr->nonce, ADMIN_NONCE_LEN,
                                       raw_pkt, header_len,
                                       payload, payload_len,
                                       plaintext, sizeof(plaintext), &plaintext_len);
    if (status != PSA_SUCCESS || plaintext_len != sizeof(plaintext)) {
        send_status_resp(sock, src, hdr->nonce, ADMIN_TYPE_ROTATE_KEY_RESP, ADMIN_STATUS_ERR_BAD_ARG, devid_get_key());
        return;
    }

    uint32_t new_epoch = get_u32_be(plaintext + DEVID_KEY_LEN);
    devid_rotate_stage(plaintext, new_epoch, hdr->nonce);

    send_status_resp(sock, src, hdr->nonce, ADMIN_TYPE_ROTATE_KEY_RESP, ADMIN_STATUS_OK, devid_get_key());
}

static void handle_claim(int sock, const parsed_header_t *hdr,
                          const uint8_t *payload, uint16_t payload_len,
                          const struct sockaddr_in *src)
{
    /* UNAUTHENTICATED, same as DISCOVER (see handle_packet() -- CLAIM is
     * dispatched before the "requires provisioned" gate, with no HMAC
     * check at all). There's nothing to authenticate against yet: a unit
     * only ever accepts this while devid_is_provisioned() == false, i.e.
     * before it has a real identity. Payload is plaintext, no envelope:
     * new_key[32] || new_epoch[4]. */
    if (devid_is_provisioned()) {
        /* Raced with another CLAIM, or a stale retry after an already-
         * successful one -- refuse, never overwrite a real identity. */
        send_status_resp(sock, src, hdr->nonce, ADMIN_TYPE_CLAIM_RESP, ADMIN_STATUS_ERR_BAD_ARG, NULL);
        return;
    }

    if (payload_len != DEVID_KEY_LEN + 4) {
        send_status_resp(sock, src, hdr->nonce, ADMIN_TYPE_CLAIM_RESP, ADMIN_STATUS_ERR_BAD_ARG, NULL);
        return;
    }

    uint32_t new_epoch = get_u32_be(payload + DEVID_KEY_LEN);
    bool ok = devid_claim(payload, new_epoch);

    send_status_resp(sock, src, hdr->nonce, ADMIN_TYPE_CLAIM_RESP,
                      ok ? ADMIN_STATUS_OK : ADMIN_STATUS_ERR_INTERNAL, NULL);
}

static void handle_rotate_confirm(int sock, const parsed_header_t *hdr, const struct sockaddr_in *src)
{
    /* Only reaches here after HMAC verification with the staged key (see
     * the main dispatch) -- i.e. the operator already proved they can use
     * the new key. The only thing left to check is that the nonce is
     * exactly the one from the ROTATE_KEY that originated this staging
     * (not just any nonce from the pool). */
    const uint8_t *staged_nonce = devid_rotate_get_staged_nonce();
    if (!staged_nonce || memcmp(staged_nonce, hdr->nonce, ADMIN_NONCE_LEN) != 0) {
        send_status_resp(sock, src, hdr->nonce, ADMIN_TYPE_ROTATE_CONFIRM_RESP, ADMIN_STATUS_ERR_NO_STAGED_KEY, devid_get_key());
        return;
    }

    if (!devid_rotate_commit()) {
        send_status_resp(sock, src, hdr->nonce, ADMIN_TYPE_ROTATE_CONFIRM_RESP, ADMIN_STATUS_ERR_NO_STAGED_KEY, devid_get_key());
        return;
    }

    /* From here on devid_get_key() is already the new key (commit swapped
     * the active key) -- the response goes out authenticated with it,
     * normally. */
    send_status_resp(sock, src, hdr->nonce, ADMIN_TYPE_ROTATE_CONFIRM_RESP, ADMIN_STATUS_OK, devid_get_key());
}

/* ---------------------------------------------------------------------
 * Main dispatch
 * --------------------------------------------------------------------- */
static void handle_packet(int sock, uint8_t *buf, size_t len, const struct sockaddr_in *src)
{
    if (!rate_limit_check(ntohl(src->sin_addr.s_addr))) {
        return; /* silent, on purpose */
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

    if (hdr.type == ADMIN_TYPE_DISCOVER) {
        handle_discover(sock, &hdr, src);
        return;
    }

    /* CLAIM is the other unauthenticated command, alongside DISCOVER --
     * it's how a fresh, unprovisioned unit gets its real identity
     * entirely over the network, with no serial/debugger connection at
     * all (see handle_claim()). There is no per-unit secret yet to check
     * anything against, so it skips the serial/epoch/HMAC checks below
     * completely -- the handler itself is what refuses to do anything if
     * the unit turns out to already be provisioned. Every other command
     * still requires a provisioned unit. */
    if (hdr.type == ADMIN_TYPE_CLAIM) {
        handle_claim(sock, &hdr, payload, hdr.payload_len, src);
        return;
    }

    if (!devid_is_provisioned()) {
        return;
    }

    /* Serial must match — addressing, not a secret, an ordinary
     * comparison is enough. */
    char my_serial_padded[ADMIN_SERIAL_LEN];
    memset(my_serial_padded, 0, sizeof(my_serial_padded));
    strncpy(my_serial_padded, devid_get_serial(), sizeof(my_serial_padded) - 1);
    if (memcmp(hdr.serial, my_serial_padded, ADMIN_SERIAL_LEN) != 0) {
        return;
    }

    /* ROTATE_CONFIRM is authenticated with the STAGED key (proves the
     * operator already operates with the new key) -- everything else
     * uses the active key. */
    bool using_staged_key = (hdr.type == ADMIN_TYPE_ROTATE_CONFIRM);
    const uint8_t *auth_key;
    uint32_t expected_epoch;
    if (using_staged_key) {
        if (!devid_rotate_has_staged()) {
            return; /* nothing staged (or it expired): drop silently */
        }
        auth_key = devid_rotate_get_staged_key();
        expected_epoch = devid_rotate_get_staged_epoch();
    } else {
        auth_key = devid_get_key();
        expected_epoch = devid_get_epoch();
    }

    if (hdr.epoch != expected_epoch) {
        return;
    }

    if (hmac_verify(auth_key, buf, hmac_covered_len, hmac, ADMIN_HMAC_LEN) != PSA_SUCCESS) {
        return;
    }

    /* Destructive commands require a single-use nonce obtained via
     * CHALLENGE beforehand. ROTATE_CONFIRM has its own nonce check inside
     * its handler (needs to be the specific nonce from the ROTATE_KEY
     * that originated the staging, not just any nonce from the pool). */
    bool needs_pool_nonce = (hdr.type == ADMIN_TYPE_REBOOT ||
                              hdr.type == ADMIN_TYPE_FACTORY_RESET ||
                              hdr.type == ADMIN_TYPE_ROTATE_KEY);
    if (needs_pool_nonce && !nonce_consume(hdr.nonce)) {
        return;
    }

    switch (hdr.type) {
    case ADMIN_TYPE_CHALLENGE:
        handle_challenge(sock, &hdr, src);
        break;
    case ADMIN_TYPE_STATUS:
        handle_status(sock, &hdr, src);
        break;
    case ADMIN_TYPE_IDENTIFY:
        handle_identify(sock, &hdr, src);
        break;
    case ADMIN_TYPE_REBOOT:
        handle_reboot(sock, &hdr, src);
        break;
    case ADMIN_TYPE_FACTORY_RESET:
        handle_factory_reset(sock, &hdr, src);
        break;
    case ADMIN_TYPE_ROTATE_KEY:
        handle_rotate_key(sock, &hdr, payload, hdr.payload_len, src, buf, ADMIN_HEADER_WIRE_SIZE);
        break;
    case ADMIN_TYPE_ROTATE_CONFIRM:
        handle_rotate_confirm(sock, &hdr, src);
        break;
    default:
        send_status_resp(sock, src, hdr.nonce, ADMIN_TYPE_ERR_RESP, ADMIN_STATUS_ERR_BAD_ARG, devid_get_key());
        break;
    }
}

/* ---------------------------------------------------------------------
 * Task / socket
 * --------------------------------------------------------------------- */
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
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt)); /* also receive on the broadcast address */

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(ADMIN_UDP_PORT);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind() failed: errno %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Admin channel listening on UDP:%u (serial %s, provisioned=%d)",
             ADMIN_UDP_PORT, devid_get_serial(), devid_is_provisioned());

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

void admin_channel_start(void)
{
    xTaskCreate(admin_channel_task, "admin_channel", TASK_STACK_SIZE, NULL, TASK_PRIORITY, NULL);
}
