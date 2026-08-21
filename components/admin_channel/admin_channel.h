/** @file admin_channel.h
 * @brief Canal UDP autenticado de administração.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Configuração do servidor administrativo. */
typedef struct {
    uint16_t port; /**< Porta UDP local. */
} admin_channel_config_t;

/** @brief Inicia a tarefa do canal administrativo.
 * @param config Configuração da porta UDP.
 * @return Nenhum.
 */
void admin_channel_start(const admin_channel_config_t *config);

#ifdef __cplusplus
}
#endif
