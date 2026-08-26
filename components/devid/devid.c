/** @file devid.c
 * @brief Device identity and admin secret storage implementation.
 */
#include "devid.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "psa/crypto.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

static const char *TAG = "DEVID";

#define NVS_NAMESPACE   "devid"
#define NVS_KEY_SECRET  "secret"

static const char *s_model_prefix;

static char s_serial[32];
static bool s_serial_computed = false;

static uint8_t s_mac[DEVID_MAC_LEN];
static bool s_mac_computed = false;

static nvs_handle_t s_nvs_handle;
static bool s_nvs_ok = false;
static uint8_t s_active_secret[DEVID_SECRET_LEN];

/**
 * @brief Computes the device serial number from the model prefix and base MAC.
 * @return None.
 */
static void compute_serial(void)
{
    const char *prefix = s_model_prefix ? s_model_prefix : "UNKNOWN";

    uint8_t mac[6] = {0};
    esp_err_t err = esp_efuse_mac_get_default(mac);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_efuse_mac_get_default failed (%s) — serial will be incomplete", esp_err_to_name(err));
        snprintf(s_serial, sizeof(s_serial), "%s-UNKNOWN000000", prefix);
    } else {
        snprintf(s_serial, sizeof(s_serial), "%s-%02X%02X%02X%02X%02X%02X",
                 prefix, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    s_serial_computed = true;
}

/**
 * @brief Reads and caches the chip's base MAC.
 * @return None.
 */
static void compute_mac(void)
{
    if (esp_efuse_mac_get_default(s_mac) != ESP_OK) {
        memset(s_mac, 0, sizeof(s_mac));
    }
    s_mac_computed = true;
}

void devid_init(const devid_config_t *config)
{
    s_model_prefix = config->model_prefix;
    compute_serial();
    ESP_LOGI(TAG, "Serial: %s", s_serial);

    psa_status_t pst = psa_crypto_init();
    if (pst != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init failed (%d) — admin secret HMAC/AEAD unavailable", (int)pst);
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed (%s) — admin secret won't persist", esp_err_to_name(err));
        return;
    }

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open('%s') failed (%s) — admin secret won't persist", NVS_NAMESPACE, esp_err_to_name(err));
        return;
    }
    s_nvs_ok = true;

    size_t secret_len = sizeof(s_active_secret);
    err = nvs_get_blob(s_nvs_handle, NVS_KEY_SECRET, s_active_secret, &secret_len);
    if (err == ESP_ERR_NVS_NOT_FOUND || (err == ESP_OK && secret_len != DEVID_SECRET_LEN)) {
        ESP_LOGW(TAG, "No admin secret stored yet — writing the factory default");
        memcpy(s_active_secret, config->factory_default_secret, DEVID_SECRET_LEN);
        err = nvs_set_blob(s_nvs_handle, NVS_KEY_SECRET, s_active_secret, DEVID_SECRET_LEN);
        if (err == ESP_OK) {
            err = nvs_commit(s_nvs_handle);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write factory-default admin secret to NVS (%s) — "
                          "using it in RAM only this boot", esp_err_to_name(err));
        }
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_get_blob('%s') failed (%s) — falling back to the factory default in RAM only",
                 NVS_KEY_SECRET, esp_err_to_name(err));
        memcpy(s_active_secret, config->factory_default_secret, DEVID_SECRET_LEN);
    } else {
        ESP_LOGI(TAG, "Admin secret loaded from NVS");
    }
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

const uint8_t *devid_get_mac(void)
{
    if (!s_mac_computed) {
        compute_mac();
    }
    return s_mac;
}

const uint8_t *devid_get_admin_secret(void)
{
    return s_active_secret;
}

bool devid_set_admin_secret(const uint8_t new_secret[DEVID_SECRET_LEN])
{
    if (!s_nvs_ok) {
        ESP_LOGE(TAG, "Cannot change admin secret: NVS isn't usable");
        return false;
    }

    esp_err_t err = nvs_set_blob(s_nvs_handle, NVS_KEY_SECRET, new_secret, DEVID_SECRET_LEN);
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs_handle);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write new admin secret to NVS (%s) — keeping the previous one active",
                 esp_err_to_name(err));
        return false;
    }

    memcpy(s_active_secret, new_secret, DEVID_SECRET_LEN);
    ESP_LOGW(TAG, "Admin secret changed");
    return true;
}
