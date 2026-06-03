/**
 * @file buzzer.h
 * @brief Módulo de sinalização sonora via buzzer PWM (LEDC).
 *
 * Gerencia inicialização do PWM e a task de alarme sonoro
 * que responde aos estados de alerta e proteção do sistema.
 */

#ifndef BUZZER_H
#define BUZZER_H

#include "project_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Inicializa o timer e canal LEDC para o buzzer.
 */
void init_ledc_buzzer(void);

/**
 * @brief Task FreeRTOS de sinalização sonora.
 * Bipe rápido em estado PROTECTED/COOLDOWN, lento em ALERT.
 * Pode ser silenciada via botão PIN_BTN_SILENCE.
 */
void task_buzzer(void *pvParameters);

#ifdef __cplusplus
}
#endif

#endif // BUZZER_H
