/** @file eth_init.c
 * @brief RMII Ethernet bring-up implementation with retry/backoff.
 */
#include "eth_init.h"
#include "esp_eth.h"
#include "esp_eth_mac_esp.h"
#include "esp_eth_phy_ip101.h"
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "ETH_INIT";

#define ETH_BRINGUP_MAX_RETRIES          5
#define ETH_BRINGUP_INITIAL_BACKOFF_MS   500
#define ETH_BRINGUP_MAX_BACKOFF_MS       8000

static bool s_got_ip = false;
static esp_netif_ip_info_t s_ip_info;
static eth_init_config_t s_config;

/**
 * @brief Handles ETH_EVENT notifications (link up/down, start/stop).
 * @param arg Unused.
 * @param event_base Unused.
 * @param event_id Event identifier.
 * @param event_data Event-specific data.
 * @return None.
 */
static void eth_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    uint8_t mac_addr[6] = {0};
    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
        ESP_LOGI(TAG, "Link up. MAC %02x:%02x:%02x:%02x:%02x:%02x",
                 mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Link down");
        s_got_ip = false;
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet started");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet stopped");
        break;
    default:
        break;
    }
}

/**
 * @brief Handles IP_EVENT_ETH_GOT_IP, caching the assigned IP configuration.
 * @param arg Unused.
 * @param event_base Unused.
 * @param event_id Unused.
 * @param event_data Event-specific data.
 * @return None.
 */
static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                  int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    s_ip_info = event->ip_info;
    s_got_ip = true;
    ESP_LOGI(TAG, "Got IP: " IPSTR "  mask: " IPSTR "  gw: " IPSTR,
             IP2STR(&s_ip_info.ip), IP2STR(&s_ip_info.netmask), IP2STR(&s_ip_info.gw));
}

/**
 * @brief One self-contained attempt to create and start the MAC/PHY/netif stack.
 * @return ESP_OK on success; the failing step's error otherwise.
 */
static esp_err_t try_create_eth(void)
{
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&netif_cfg);
    if (eth_netif == NULL) {
        ESP_LOGE(TAG, "esp_netif_new() failed (out of memory?)");
        return ESP_ERR_NO_MEM;
    }

    if (s_config.hostname != NULL) {
        esp_err_t hostname_err = esp_netif_set_hostname(eth_netif, s_config.hostname);
        if (hostname_err != ESP_OK) {
            ESP_LOGW(TAG, "esp_netif_set_hostname('%s') failed (%s)", s_config.hostname, esp_err_to_name(hostname_err));
        }
    }

    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_config.smi_gpio.mdc_num = s_config.mdc_pin;
    emac_config.smi_gpio.mdio_num = s_config.mdio_pin;
    emac_config.interface = EMAC_DATA_INTERFACE_RMII;
    emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac_config.clock_config.rmii.clock_gpio = s_config.ref_clk_pin;

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    if (mac == NULL) {
        ESP_LOGE(TAG, "Failed to create MAC instance");
        esp_netif_destroy(eth_netif);
        return ESP_FAIL;
    }

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = s_config.phy_addr;
    phy_config.reset_gpio_num = s_config.phy_reset_pin;

    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_config);
    if (phy == NULL) {
        ESP_LOGE(TAG, "Failed to create IP101G PHY instance");
        mac->del(mac);
        esp_netif_destroy(eth_netif);
        return ESP_FAIL;
    }

    esp_eth_handle_t eth_handle = NULL;
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_err_t err = esp_eth_driver_install(&eth_config, &eth_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install Ethernet driver (%s)", esp_err_to_name(err));
        mac->del(mac);
        phy->del(phy);
        esp_netif_destroy(eth_netif);
        return err;
    }

    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(eth_handle);
    if (glue == NULL) {
        ESP_LOGE(TAG, "esp_eth_new_netif_glue() failed");
        esp_eth_driver_uninstall(eth_handle);
        esp_netif_destroy(eth_netif);
        return ESP_FAIL;
    }

    err = esp_netif_attach(eth_netif, glue);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_attach() failed (%s)", esp_err_to_name(err));
        esp_eth_driver_uninstall(eth_handle);
        esp_netif_destroy(eth_netif);
        return err;
    }

    err = esp_eth_start(eth_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_eth_start() failed (%s)", esp_err_to_name(err));
        esp_eth_driver_uninstall(eth_handle);
        esp_netif_destroy(eth_netif);
        return err;
    }

    ESP_LOGI(TAG, "Ethernet (IP101G/RMII) installed, PHY addr=%d", s_config.phy_addr);
    return ESP_OK;
}

esp_err_t eth_bringup(const eth_init_config_t *config)
{
    s_config = *config;

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init() failed (%s)", esp_err_to_name(err));
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default() failed (%s)", esp_err_to_name(err));
        return err;
    }
    err = esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register ETH_EVENT handler (%s)", esp_err_to_name(err));
        return err;
    }
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register IP_EVENT handler (%s)", esp_err_to_name(err));
        return err;
    }

    uint32_t backoff_ms = ETH_BRINGUP_INITIAL_BACKOFF_MS;
    for (int attempt = 1; attempt <= ETH_BRINGUP_MAX_RETRIES; attempt++) {
        err = try_create_eth();
        if (err == ESP_OK) {
            return ESP_OK;
        }
        bool will_retry = (attempt < ETH_BRINGUP_MAX_RETRIES);
        ESP_LOGE(TAG, "Ethernet bring-up attempt %d/%d failed (%s)%s",
                 attempt, ETH_BRINGUP_MAX_RETRIES, esp_err_to_name(err),
                 will_retry ? ", retrying..." : " -- giving up");
        if (will_retry) {
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            backoff_ms = (backoff_ms * 2 > ETH_BRINGUP_MAX_BACKOFF_MS) ? ETH_BRINGUP_MAX_BACKOFF_MS : backoff_ms * 2;
        }
    }
    return err;
}

bool eth_has_ip(void)
{
    return s_got_ip;
}

bool eth_get_ip_info(esp_netif_ip_info_t *out)
{
    if (!s_got_ip || out == NULL) {
        return false;
    }
    memcpy(out, &s_ip_info, sizeof(s_ip_info));
    return true;
}
