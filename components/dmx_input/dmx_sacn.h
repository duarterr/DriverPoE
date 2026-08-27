/** @file dmx_sacn.h
 * @brief sACN (ANSI E1.31) receiver: multicast + unicast DMX data packets.
 * Internal to dmx_input.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SACN_PORT 5568

/**
 * @brief Opens and binds the sACN UDP socket (0.0.0.0:5568) and joins the
 * multicast group for the currently configured universe.
 * @return Socket fd, or -1 on failure.
 */
int dmx_sacn_open(void);

/**
 * @brief Closes the sACN socket (leaving any joined multicast group).
 * @param sock Socket fd (may be -1).
 * @return None.
 */
void dmx_sacn_close(int sock);

/**
 * @brief Re-evaluates multicast membership after a universe change.
 * @param sock Socket fd.
 * @return None.
 */
void dmx_sacn_rejoin(int sock);

/**
 * @brief Processes one received datagram (E1.31 data packet -> merge engine).
 * @param buf Datagram bytes (not retained).
 * @param len Datagram length.
 * @param src_ip Source IPv4 (host order).
 * @return None.
 */
void dmx_sacn_handle(uint8_t *buf, size_t len, uint32_t src_ip);

#ifdef __cplusplus
}
#endif
