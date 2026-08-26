/** @file admin_channel.h
 * @brief Authenticated UDP admin channel.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Admin server configuration. */
typedef struct {
    uint16_t port; /**< Local UDP port. */
} admin_channel_config_t;

/**
 * @brief Starts the admin channel task.
 * @param config UDP port configuration.
 * @return None.
 */
void admin_channel_start(const admin_channel_config_t *config);

#ifdef __cplusplus
}
#endif
