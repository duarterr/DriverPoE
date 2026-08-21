/** @file tps2378.h
 * @brief Monitoramento de PoE/AUX e validação da tensão VBUS.
 */
#pragma once

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Fonte de alimentação detectada. */
typedef enum {
    TPS2378_SOURCE_NONE = 0, /**< Nenhuma fonte válida. */
    TPS2378_SOURCE_TYPE1,    /**< PoE IEEE 802.3af. */
    TPS2378_SOURCE_TYPE2,    /**< PoE IEEE 802.3at. */
    TPS2378_SOURCE_AUX,      /**< Alimentação auxiliar. */
} tps2378_source_t;

/** @brief Configuração do monitor TPS2378. */
typedef struct {
    gpio_num_t cdb_pin;     /**< Entrada CDB. */
    gpio_num_t t2p_pin;     /**< Entrada T2P. */
    int vbus_min_mv;        /**< VBUS mínima válida. */
    int vbus_hysteresis_mv; /**< Histerese de VBUS. */
    void (*on_power_ready)(tps2378_source_t source, void *ctx); /**< Notificação de alimentação pronta. */
    void (*on_power_lost)(void *ctx); /**< Notificação de perda de alimentação. */
    void *callback_ctx; /**< Contexto das notificações. */
} tps2378_config_t;

/** @brief Inicializa o monitor de alimentação.
 * @param config GPIOs, limites e callbacks.
 * @return Nenhum.
 */
void tps2378_init(const tps2378_config_t *config);

/** @brief Aguarda a alimentação ser confirmada.
 * @param timeout Tempo máximo em ticks, ou portMAX_DELAY.
 * @return true se a alimentação está pronta.
 */
bool tps2378_wait_ready(TickType_t timeout);

/** @brief Consulta o estado de alimentação confirmado.
 * @return true se fonte e VBUS são válidas.
 */
bool tps2378_is_ready(void);

/** @brief Obtém a fonte de alimentação detectada.
 * @return Fonte atual.
 */
tps2378_source_t tps2378_get_source(void);

/** @brief Consulta os sinais confirmados pelo debounce.
 * @return true quando o respectivo sinal está confirmado.
 */
bool tps2378_cdb_confirmed(void);
bool tps2378_t2p_confirmed(void);
bool tps2378_vbus_confirmed(void);

/** @brief Consulta os sinais instantâneos, sem debounce.
 * @return true quando o respectivo sinal está ativo.
 */
bool tps2378_cdb_raw(void);
bool tps2378_t2p_raw(void);
bool tps2378_vbus_raw(void);

/** @brief Obtém a última VBUS medida.
 * @return Tensão em milivolts.
 */
int tps2378_get_vbus_mv(void);

/** @brief Obtém o nome legível de uma fonte.
 * @param source Fonte a converter.
 * @return Nome da fonte.
 */
const char *tps2378_source_name(tps2378_source_t source);

/** @brief Obtém o identificador curto de uma fonte.
 * @param source Fonte a converter.
 * @return Identificador sem espaços.
 */
const char *tps2378_source_short_name(tps2378_source_t source);

/** @brief Obtém a potência PoE nominal disponível.
 * @return Potência em watts, ou zero para AUX/nenhuma fonte.
 */
float tps2378_get_available_power_w(void);

#ifdef __cplusplus
}
#endif
