#include "devid.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

static const char *TAG = "DEVID";

#define NVS_PARTITION     "idnvs"
#define NVS_NAMESPACE     "devid"
#define NVS_KEY_KEY       "key"
#define NVS_KEY_EPOCH     "epoch"

#define DEVID_ROTATE_TTL_US   (30 * 1000000LL) /* 30s to receive ROTATE_CONFIRM before discarding */

/* Model prefix, copied in from devid_init()'s config argument -- see
 * devid.h (this one field is retained by pointer, not copied by value:
 * callers must pass a string with static storage duration). NULL until
 * devid_init() runs; compute_serial() below falls back to a generic
 * placeholder in that case rather than risk passing NULL to snprintf. */
static const char *s_model_prefix;

static bool s_provisioned = false;
static uint8_t s_active_key[DEVID_KEY_LEN];
static uint32_t s_active_epoch = 0;
static nvs_handle_t s_nvs_handle;
static bool s_nvs_ok = false;

static char s_serial[32];
static bool s_serial_computed = false;

/* Key rotation staging — RAM only until devid_rotate_commit(). */
static bool s_staged_valid = false;
static uint8_t s_staged_key[DEVID_KEY_LEN];
static uint8_t s_staged_nonce[DEVID_NONCE_LEN];
static uint32_t s_staged_epoch = 0;
static int64_t s_staged_at_us = 0;

static void compute_serial(void)
{
    /* Falls back to a generic placeholder prefix if devid_init() somehow
     * hasn't run yet (s_model_prefix still NULL) -- keeps this function
     * safe to call defensively (see devid_get_serial()) without risking
     * a NULL passed to snprintf's %s. */
    const char *prefix = s_model_prefix ? s_model_prefix : "UNKNOWN";

    uint8_t mac[6] = {0};
    esp_err_t err = esp_efuse_mac_get_default(mac);
    if (err != ESP_OK) {
        /* Shouldn't happen on real silicon (the base MAC is always
         * factory-programmed), but never let devid_get_serial() return
         * something empty/undefined. */
        ESP_LOGE(TAG, "esp_efuse_mac_get_default failed (%s) — serial will be incomplete", esp_err_to_name(err));
        snprintf(s_serial, sizeof(s_serial), "%s-UNKNOWN000000", prefix);
    } else {
        snprintf(s_serial, sizeof(s_serial), "%s-%02X%02X%02X%02X%02X%02X",
                 prefix, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    s_serial_computed = true;
}

void devid_init(const devid_config_t *config)
{
    s_model_prefix = config->model_prefix;
    compute_serial();
    ESP_LOGI(TAG, "Serial: %s", s_serial);

    esp_err_t err = nvs_flash_init_partition(NVS_PARTITION);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* Would only happen if "idnvs" was never initialized as NVS yet
         * (e.g. a fresh chip that the production station hasn't flashed
         * yet) — erasing here is safe, there's nothing valid to lose in
         * that case. */
        ESP_ERROR_CHECK(nvs_flash_erase_partition(NVS_PARTITION));
        err = nvs_flash_init_partition(NVS_PARTITION);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init_partition('%s') failed (%s) — admin channel will stay disabled",
                 NVS_PARTITION, esp_err_to_name(err));
        return;
    }

    err = nvs_open_from_partition(NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &s_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open_from_partition failed (%s) — admin channel will stay disabled",
                 esp_err_to_name(err));
        return;
    }
    s_nvs_ok = true;

    size_t key_len = sizeof(s_active_key);
    err = nvs_get_blob(s_nvs_handle, NVS_KEY_KEY, s_active_key, &key_len);
    if (err != ESP_OK || key_len != DEVID_KEY_LEN) {
        ESP_LOGW(TAG, "Device NOT provisioned (key missing/invalid: %s) — "
                      "admin channel stays in DISCOVER-only mode",
                 esp_err_to_name(err));
        return;
    }

    err = nvs_get_u32(s_nvs_handle, NVS_KEY_EPOCH, &s_active_epoch);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Device NOT provisioned (epoch missing: %s) — "
                      "admin channel stays in DISCOVER-only mode",
                 esp_err_to_name(err));
        return;
    }

    s_provisioned = true;
    ESP_LOGI(TAG, "Identity provisioned: current epoch = %" PRIu32, s_active_epoch);
}

bool devid_is_provisioned(void)
{
    return s_provisioned;
}

const char *devid_get_serial(void)
{
    if (!s_serial_computed) {
        compute_serial();
    }
    return s_serial;
}

const char *devid_get_model_prefix(void)
{
    return s_model_prefix;
}

const uint8_t *devid_get_key(void)
{
    return s_active_key;
}

uint32_t devid_get_epoch(void)
{
    return s_active_epoch;
}

bool devid_claim(const uint8_t new_key[DEVID_KEY_LEN], uint32_t new_epoch)
{
    if (s_provisioned) {
        /* CLAIM can only ever set the FIRST identity. A second CLAIM
         * (duplicate/retransmitted request, or a genuine attempt on an
         * already-claimed unit) is refused, never overwrites. */
        return false;
    }
    if (!s_nvs_ok) {
        ESP_LOGE(TAG, "Cannot claim: 'idnvs' partition isn't usable");
        return false;
    }

    esp_err_t err = nvs_set_blob(s_nvs_handle, NVS_KEY_KEY, new_key, DEVID_KEY_LEN);
    if (err == ESP_OK) {
        err = nvs_set_u32(s_nvs_handle, NVS_KEY_EPOCH, new_epoch);
    }
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs_handle);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write claimed identity to NVS (%s) — still unprovisioned, safe to retry",
                 esp_err_to_name(err));
        return false;
    }

    memcpy(s_active_key, new_key, DEVID_KEY_LEN);
    s_active_epoch = new_epoch;
    s_provisioned = true;
    ESP_LOGW(TAG, "Identity claimed remotely — now provisioned (epoch %" PRIu32 ")", new_epoch);
    return true;
}

void devid_rotate_stage(const uint8_t new_key[DEVID_KEY_LEN], uint32_t new_epoch,
                         const uint8_t nonce[DEVID_NONCE_LEN])
{
    memcpy(s_staged_key, new_key, DEVID_KEY_LEN);
    memcpy(s_staged_nonce, nonce, DEVID_NONCE_LEN);
    s_staged_epoch = new_epoch;
    s_staged_at_us = esp_timer_get_time();
    s_staged_valid = true;
    ESP_LOGW(TAG, "New key staged (epoch %" PRIu32 "), waiting for ROTATE_CONFIRM within %llds",
             new_epoch, (long long)(DEVID_ROTATE_TTL_US / 1000000));
}

static void rotate_expire_if_needed(void)
{
    if (s_staged_valid && (esp_timer_get_time() - s_staged_at_us) > DEVID_ROTATE_TTL_US) {
        ESP_LOGW(TAG, "Key staging expired without a ROTATE_CONFIRM — keeping the current key (epoch %" PRIu32 ")",
                 s_active_epoch);
        devid_rotate_discard();
    }
}

bool devid_rotate_has_staged(void)
{
    rotate_expire_if_needed();
    return s_staged_valid;
}

const uint8_t *devid_rotate_get_staged_key(void)
{
    rotate_expire_if_needed();
    return s_staged_valid ? s_staged_key : NULL;
}

const uint8_t *devid_rotate_get_staged_nonce(void)
{
    rotate_expire_if_needed();
    return s_staged_valid ? s_staged_nonce : NULL;
}

uint32_t devid_rotate_get_staged_epoch(void)
{
    rotate_expire_if_needed();
    return s_staged_epoch; /* 0 if there's no valid staging */
}

bool devid_rotate_commit(void)
{
    rotate_expire_if_needed();
    if (!s_staged_valid) {
        return false;
    }

    if (s_nvs_ok) {
        esp_err_t err = nvs_set_blob(s_nvs_handle, NVS_KEY_KEY, s_staged_key, DEVID_KEY_LEN);
        if (err == ESP_OK) {
            err = nvs_set_u32(s_nvs_handle, NVS_KEY_EPOCH, s_staged_epoch);
        }
        if (err == ESP_OK) {
            err = nvs_commit(s_nvs_handle);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write the new key to NVS (%s) — rotation aborted, current key kept",
                     esp_err_to_name(err));
            return false;
        }
    }

    memcpy(s_active_key, s_staged_key, DEVID_KEY_LEN);
    s_active_epoch = s_staged_epoch;
    ESP_LOGW(TAG, "Key rotation committed — new active epoch: %" PRIu32, s_active_epoch);
    devid_rotate_discard();
    return true;
}

void devid_rotate_discard(void)
{
    memset(s_staged_key, 0, sizeof(s_staged_key));
    memset(s_staged_nonce, 0, sizeof(s_staged_nonce));
    s_staged_epoch = 0;
    s_staged_at_us = 0;
    s_staged_valid = false;
}
