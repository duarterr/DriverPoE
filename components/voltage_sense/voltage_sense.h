/** @file voltage_sense.h
 * @brief Leitura de VBUS e da tensão do LED por ADC.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "esp_adc/adc_oneshot.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Tensões medidas, em milivolts. */
typedef struct {
    int vbus_mv;         /**< Tensão do barramento. */
    int led_voltage_mv;  /**< Tensão direta do LED. */
} voltage_reading_t;

/** @brief Configuração dos canais ADC e divisor resistivo. */
typedef struct {
    adc_channel_t vled_p_channel; /**< Canal LED+. */
    adc_channel_t vled_n_channel; /**< Canal LED-. */
    float divider_ratio;           /**< Fator de escala do divisor. */
} voltage_sense_config_t;

/** @brief Inicializa o ADC para medição de tensão.
 * @param config Canais e fator do divisor.
 * @return Nenhum.
 */
void voltage_sense_init(const voltage_sense_config_t *config);

/** @brief Lê as tensões do barramento e do LED.
 * @param out Destino da leitura.
 * @return ESP_OK em sucesso; erro do ADC caso contrário.
 */
esp_err_t voltage_sense_read(voltage_reading_t *out);

#ifdef __cplusplus
}
#endif
