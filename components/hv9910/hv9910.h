/** @file hv9910.h
 * @brief Controle do driver de LEDs HV9910.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Configuração do driver HV9910. */
typedef struct {
    gpio_num_t shutdown_pin; /**< GPIO de habilitação. */
    gpio_num_t dimming_pin;  /**< GPIO PWM de dimerização. */
    bool shutdown_active_high; /**< Polaridade de habilitação. */
    bool dim_active_high; /**< Polaridade do PWM. */
    uint32_t pwm_freq_hz; /**< Frequência do PWM. */
    uint32_t max_ramp_ms; /**< Limite de rampa em ms. */
} hv9910_config_t;

/** @brief Inicializa o driver no estado desligado.
 * @param config GPIOs, polaridades e limites.
 * @return Nenhum.
 */
void hv9910_init(const hv9910_config_t *config);

/** @brief Habilita o driver e restaura o último nível.
 * @param ramp_ms Duração da rampa em ms.
 * @param persist true para persistir o estado ligado.
 * @return Nenhum.
 */
void hv9910_enable(uint32_t ramp_ms, bool persist);

/** @brief Desabilita o driver após reduzir o brilho.
 * @param ramp_ms Duração da rampa em ms.
 * @param persist true para persistir o estado desligado.
 * @return Nenhum.
 */
void hv9910_disable(uint32_t ramp_ms, bool persist);

/** @brief Desabilita o driver imediatamente, com prioridade.
 * @return Nenhum.
 */
void hv9910_emergency_disable(void);

/** @brief Informa o último estado persistido.
 * @return true se o estado persistido é ligado.
 */
bool hv9910_was_last_on(void);

/** @brief Informa se o driver está habilitado.
 * @return true se o driver está ligado.
 */
bool hv9910_is_enabled(void);

/** @brief Define o brilho do LED.
 * @param percent Brilho de 0 a 100.
 * @param ramp_ms Duração da rampa em ms.
 * @return Nenhum.
 */
void hv9910_set_dim(uint8_t percent, uint32_t ramp_ms);

/** @brief Executa a sequência de identificação visual.
 * @return Nenhum.
 */
void hv9910_identify(void);

/** @brief Obtém o brilho atual solicitado.
 * @return Brilho de 0 a 100.
 */
uint8_t hv9910_get_dim_percent(void);

/** @brief Obtém o último brilho não nulo persistido.
 * @return Brilho de 1 a 100.
 */
uint8_t hv9910_get_last_nonzero_percent(void);

#ifdef __cplusplus
}
#endif
