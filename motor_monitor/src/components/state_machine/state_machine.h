/**
 * @file state_machine.h
 * @brief Módulo da máquina de estados e proteção do motor AC.
 *
 * Gerencia transições de estado (IDLE→ARMED→ALERT→PROTECTED→COOLDOWN),
 * controle thread-safe de limiares dinâmicos, atuação do relé de segurança,
 * e a task de proteção que monitora violações de limiares.
 */

#ifndef STATE_MACHINE_H
#define STATE_MACHINE_H

#include "project_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Inicializa mutexes internos da máquina de estados.
 * Deve ser chamada antes de qualquer outra função deste módulo.
 */
void state_machine_init(void);

/**
 * @brief Configura os GPIOs de saída (relé) e entrada (botões).
 */
void init_gpios(void);

/**
 * @brief Altera o estado do sistema com thread-safety e atuação no relé.
 */
void set_system_state(motor_state_t eNewState);

/**
 * @brief Obtém o estado atual do sistema (thread-safe).
 */
motor_state_t get_system_state(void);

/**
 * @brief Copia os limiares ativos para o ponteiro de destino.
 */
void get_active_thresholds(motor_thresholds_t *pDest);

/**
 * @brief Atualiza os limiares de proteção do motor.
 */
void update_active_thresholds(const motor_thresholds_t *pNew);

/**
 * @brief Task FreeRTOS de proteção: monitora sensores e dispara
 * transições de estado ao detectar violação de limiares.
 */
void task_protection(void *pvParameters);

#ifdef __cplusplus
}
#endif

#endif // STATE_MACHINE_H
