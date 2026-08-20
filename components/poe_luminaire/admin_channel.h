/*
 * Authenticated UDP admin channel — network discovery and remote recovery
 * for the luminaire, independent of the application logic
 * (poe_negotiator / cmd_server). Full protocol in README.md.
 *
 * Own task, higher priority than cmd_server_task, with no dependency on
 * any lock/state from the other modules — keeps responding even if the
 * application hangs, as long as the network stack itself is up (see
 * poe_luminaire_main.c: eth_bringup() is still gated by PoE/AUX/VBUS, so
 * the channel stays quiet in low-power mode — it covers "the application
 * hung after being powered up", not "the luminaire never powered up").
 *
 * Requires devid_init() to have already run. If the device isn't
 * provisioned (devid_is_provisioned() == false), it only answers DISCOVER
 * (unauthenticated) — everything else is refused silently (fail-closed).
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Creates the admin channel task (UDP socket on ADMIN_UDP_PORT,
 * poe_luminaire.h). Call after eth_bringup(). */
void admin_channel_start(void);

#ifdef __cplusplus
}
#endif
