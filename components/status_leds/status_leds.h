/** @file status_leds.h
 * @brief Indicadores de alimentação e estado do driver.
 */
#pragma once

#include <stdbool.h>
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief GPIOs, polaridade e fontes de estado dos LEDs.
 *
 * Este componente não inclui os headers de outros componentes -- quem
 * chama status_leds_init() é quem sabe o que "alimentação ok" e "driver
 * ligado" significam, e passa isso como ponteiros de função. Isso
 * também é o que permite status_leds_init() ser chamado bem no início
 * do main(), antes desses outros módulos existirem: os ponteiros só
 * precisam ser válidos quando a tarefa de polling os chamar, não no
 * momento do init.
 */
typedef struct {
    gpio_num_t blue_pin; /**< LED de alimentação. */
    gpio_num_t red_pin;  /**< LED do driver. */
    bool blue_active_high; /**< Polaridade do LED azul (true = GPIO em nível alto acende). */
    bool red_active_high;  /**< Polaridade do LED vermelho (true = GPIO em nível alto acende). */
    bool (*power_ok_fn)(void); /**< Retorna true quando a alimentação (VBUS) está confirmada. NULL = considera sempre false. */
    bool (*driver_on_fn)(void); /**< Retorna true quando o driver de LED está habilitado. NULL = considera sempre false. */
} status_leds_config_t;

/** @brief Configura os LEDs e inicia sua atualização.
 * @param config GPIOs dos indicadores.
 * @return Nenhum.
 */
void status_leds_init(const status_leds_config_t *config);

#ifdef __cplusplus
}
#endif
