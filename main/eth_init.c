#include "eth_init.h"
#include "board_pins.h"
#include "esp_eth.h"
#include "esp_eth_mac_esp.h"
#include "esp_eth_phy_ip101.h"
#include "esp_event.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include <string.h>

static const char *TAG = "ETH_INIT";

static bool s_got_ip = false;
static esp_netif_ip_info_t s_ip_info;

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
        gpio_set_level(PIN_LED_STATUS, 1);
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Link down");
        s_got_ip = false;
        gpio_set_level(PIN_LED_STATUS, 0);
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

void eth_bringup(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&netif_cfg);

    /* MAC: ESP32 internal EMAC in RMII. The data pins (TXD0/TXD1/TX_EN/
     * RXD0/RXD1/CRS_DV) are fixed in hardware on the classic ESP32 and
     * don't appear here — only MDC/MDIO/clock are configurable, all
     * sourced from board_pins.h. */
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_config.smi_gpio.mdc_num = PIN_ETH_MDC;
    emac_config.smi_gpio.mdio_num = PIN_ETH_MDIO;
    emac_config.interface = EMAC_DATA_INTERFACE_RMII;
    emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN; /* the IP101G generates the 50MHz clock */
    emac_config.clock_config.rmii.clock_gpio = PIN_ETH_REF_CLK;

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    if (mac == NULL) {
        ESP_LOGE(TAG, "Failed to create MAC instance");
        return;
    }

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = ETH_PHY_ADDR;
    phy_config.reset_gpio_num = PIN_ETH_PHY_RESET;

    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_config);
    if (phy == NULL) {
        ESP_LOGE(TAG, "Failed to create IP101G PHY instance");
        mac->del(mac);
        return;
    }

    esp_eth_handle_t eth_handle = NULL;
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_err_t err = esp_eth_driver_install(&eth_config, &eth_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install Ethernet driver (%d)", err);
        mac->del(mac);
        phy->del(phy);
        return;
    }

    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(eth_handle);
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, glue));

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL));

    ESP_ERROR_CHECK(esp_eth_start(eth_handle));

    ESP_LOGI(TAG, "Ethernet (IP101G/RMII) installed, PHY addr=%d", ETH_PHY_ADDR);
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
