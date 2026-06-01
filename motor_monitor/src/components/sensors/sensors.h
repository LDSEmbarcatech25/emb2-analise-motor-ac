/**
 * @file sensors.h
 * @brief Módulo de leitura simulada dos sensores industriais.
 *
 * Mantém a simulação matemática (sinf, cosf, rand) dos sensores de
 * temperatura, umidade e vibração triaxial.
 */

#ifndef SENSORS_H
#define SENSORS_H

#include "project_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Task FreeRTOS de leitura periódica dos sensores.
 * Publica dados na xSensorQueue a cada 500ms.
 */
void task_sensors(void *pvParameters);

/**
 * @brief Obtém a última leitura dos sensores (thread-safe).
 * @param[out] pData Ponteiro para a estrutura de saída.
 */
void sensors_get_last_data(sensor_data_t *pData);

/**
 * @brief Ativa ou desativa a injeção de anomalia para testes end-to-end.
 * @param bEnable true para ativar, false para desativar.
 */
void sensors_inject_anomaly(bool bEnable);

/**
 * @brief Retorna o estado atual da flag de injeção de anomalia.
 */
bool sensors_get_inject_anomaly(void);

#ifdef __cplusplus
}
#endif

#endif // SENSORS_H
