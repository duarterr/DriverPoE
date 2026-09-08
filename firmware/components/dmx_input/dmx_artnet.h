/** @file dmx_artnet.h
 * @brief Art-Net receiver: ArtDmx input, ArtPoll/ArtPollReply discovery,
 * optional ArtAddress remote configuration. Internal to dmx_input.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ARTNET_PORT 6454

/**
 * @brief Opens and binds the Art-Net UDP socket (0.0.0.0:6454, broadcast on).
 * @return Socket fd, or -1 on failure.
 */
int dmx_artnet_open(void);

/**
 * @brief Closes the Art-Net socket.
 * @param sock Socket fd (may be -1).
 * @return None.
 */
void dmx_artnet_close(int sock);

/**
 * @brief Processes one received datagram: ArtDmx -> dmx_merge_submit();
 * ArtPoll -> ArtPollReply; ArtAddress -> apply (if allowed).
 * @param sock Socket fd, used to send replies.
 * @param buf Datagram bytes (mutable; not retained).
 * @param len Datagram length.
 * @param src_ip Source IPv4 (host order).
 * @param src_port Source UDP port (host order).
 * @return None.
 */
void dmx_artnet_handle(int sock, uint8_t *buf, size_t len, uint32_t src_ip, uint16_t src_port);

#ifdef __cplusplus
}
#endif
