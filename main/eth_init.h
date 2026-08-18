/*
 * Ethernet bring-up (ESP32 internal EMAC + IP101G PHY, RMII), built
 * directly against esp_eth's C API — no Kconfig-driven wrapper. All the
 * fixed ESP32-classic RMII data pins (TXD0=19, TXD1=22, TX_EN=21,
 * RXD0=25, RXD1=26, CRS_DV=27) and the configurable ones (MDC=23,
 * MDIO=18, RESET=5, REF_CLK=0 as input) come from board_pins.h.
 *
 * Must only be called AFTER poe_negotiator confirms CDB=HIGH — the whole
 * application waits for PoE negotiation before bringing up the network
 * stack, per this project's explicit requirement.
 */
#pragma once

#include <stdbool.h>
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Brings up esp_netif and the Ethernet driver, and starts link
 * negotiation. Must be called exactly once. */
void eth_bringup(void);

/* true once IP_EVENT_ETH_GOT_IP has fired at least once. */
bool eth_has_ip(void);

/* Copies the last known esp_netif_ip_info_t into 'out'. Returns false if
 * there's no IP yet. */
bool eth_get_ip_info(esp_netif_ip_info_t *out);

#ifdef __cplusplus
}
#endif
