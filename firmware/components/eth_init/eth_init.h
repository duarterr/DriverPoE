/** @file eth_init.h
 * @brief RMII Ethernet bring-up with the IP101G PHY.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_netif.h"
#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Ethernet interface configuration. */
typedef struct {
    gpio_num_t mdc_pin;       /**< MDC GPIO. */
    gpio_num_t mdio_pin;      /**< MDIO GPIO. */
    gpio_num_t ref_clk_pin;   /**< RMII clock input. */
    gpio_num_t phy_reset_pin; /**< PHY reset GPIO. */
    uint8_t phy_addr;         /**< PHY SMI address. */
    const char *hostname;     /**< DHCP hostname (option 12); NULL keeps the ESP-IDF default. */
} eth_init_config_t;

/**
 * @brief Creates and starts the Ethernet interface.
 * @param config PHY and GPIO configuration.
 * @return ESP_OK on success; the last error on failure.
 */
esp_err_t eth_bringup(const eth_init_config_t *config);

/**
 * @brief Reports whether the interface has received an IP address.
 * @return true if an IP address is assigned.
 */
bool eth_has_ip(void);

/**
 * @brief Gets the last known IP configuration.
 * @param out Destination for the configuration.
 * @return true if an IP address is available.
 */
bool eth_get_ip_info(esp_netif_ip_info_t *out);

#ifdef __cplusplus
}
#endif
