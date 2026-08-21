/** @file eth_init.h
 * @brief Inicialização Ethernet RMII com PHY IP101G.
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

/** @brief Configuração da interface Ethernet. */
typedef struct {
    gpio_num_t mdc_pin;       /**< GPIO MDC. */
    gpio_num_t mdio_pin;      /**< GPIO MDIO. */
    gpio_num_t ref_clk_pin;   /**< Entrada de clock RMII. */
    gpio_num_t phy_reset_pin; /**< Reset do PHY. */
    uint8_t phy_addr;         /**< Endereço SMI do PHY. */
} eth_init_config_t;

/** @brief Cria e inicia a interface Ethernet.
 * @param config Configuração do PHY e dos GPIOs.
 * @return ESP_OK em sucesso; último erro em falha.
 */
esp_err_t eth_bringup(const eth_init_config_t *config);

/** @brief Informa se a interface já recebeu IP.
 * @return true se há endereço IP; false caso contrário.
 */
bool eth_has_ip(void);

/** @brief Obtém a última configuração de IP.
 * @param out Destino da configuração.
 * @return true se há IP disponível; false caso contrário.
 */
bool eth_get_ip_info(esp_netif_ip_info_t *out);

#ifdef __cplusplus
}
#endif
