/** @file devid.h
 * @brief Identidade, provisionamento e rotação de chaves do dispositivo.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEVID_KEY_LEN   32 /**< Tamanho da chave ativa, em bytes. */
#define DEVID_NONCE_LEN 16 /**< Tamanho do nonce de rotação, em bytes. */

/** @brief Configuração da identidade do equipamento. */
typedef struct {
    const char *model_prefix; /**< Prefixo usado no número de série. */
} devid_config_t;

/** @brief Inicializa a identidade e carrega a chave persistida.
 * @param config Configuração do modelo.
 * @return Nenhum.
 */
void devid_init(const devid_config_t *config);

/** @brief Verifica se há uma identidade provisionada.
 * @return true se chave e época estão disponíveis.
 */
bool devid_is_provisioned(void);

/** @brief Obtém o número de série do dispositivo.
 * @return String ASCII do número de série.
 */
const char *devid_get_serial(void);

/** @brief Obtém o prefixo do modelo.
 * @return Prefixo configurado ou NULL antes da inicialização.
 */
const char *devid_get_model_prefix(void);

/** @brief Obtém a chave ativa.
 * @return Ponteiro para DEVID_KEY_LEN bytes.
 */
const uint8_t *devid_get_key(void);

/** @brief Obtém a época da chave ativa.
 * @return Época atual.
 */
uint32_t devid_get_epoch(void);

/** @brief Provisiona a primeira identidade do dispositivo.
 * @param new_key Nova chave de DEVID_KEY_LEN bytes.
 * @param new_epoch Época da nova chave.
 * @return true se a identidade foi gravada; false caso contrário.
 */
bool devid_claim(const uint8_t new_key[DEVID_KEY_LEN], uint32_t new_epoch);

/** @brief Mantém uma nova chave em memória para confirmação.
 * @param new_key Nova chave de DEVID_KEY_LEN bytes.
 * @param new_epoch Época da nova chave.
 * @param nonce Nonce que vincula a confirmação à solicitação.
 * @return Nenhum.
 */
void devid_rotate_stage(const uint8_t new_key[DEVID_KEY_LEN], uint32_t new_epoch,
                        const uint8_t nonce[DEVID_NONCE_LEN]);

/** @brief Verifica se existe uma chave pendente válida.
 * @return true se há chave pendente.
 */
bool devid_rotate_has_staged(void);

/** @brief Obtém a chave pendente.
 * @return Ponteiro para a chave, ou NULL se não houver pendência.
 */
const uint8_t *devid_rotate_get_staged_key(void);

/** @brief Obtém o nonce da chave pendente.
 * @return Ponteiro para o nonce, ou NULL se não houver pendência.
 */
const uint8_t *devid_rotate_get_staged_nonce(void);

/** @brief Obtém a época da chave pendente.
 * @return Época pendente, ou 0 se não houver pendência.
 */
uint32_t devid_rotate_get_staged_epoch(void);

/** @brief Persiste e ativa a chave pendente.
 * @return true se a rotação foi concluída.
 */
bool devid_rotate_commit(void);

/** @brief Descarta a chave pendente.
 * @return Nenhum.
 */
void devid_rotate_discard(void);

#ifdef __cplusplus
}
#endif
