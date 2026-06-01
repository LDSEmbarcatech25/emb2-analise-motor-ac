/**
 * @file connectivity.h
 * @brief Módulo de conectividade WiFi, MQTT, telemetria e auditoria.
 *
 * Gerencia inicialização WiFi/MQTT, task de telemetria com backup em SD,
 * processamento de comandos remotos e publicação de logs de auditoria.
 */

#ifndef CONNECTIVITY_H
#define CONNECTIVITY_H

#include "project_defs.h"
#include "mqtt_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Inicializa WiFi Station mode e registra event handlers.
 */
void init_wifi(void);

/**
 * @brief Inicializa cliente MQTT. Aguarda WiFi antes de conectar.
 */
void init_mqtt(void);

/**
 * @brief Inicializa barramento SPI e monta o cartão SD.
 */
void init_sd_card(void);

/**
 * @brief Task FreeRTOS de telemetria: envia dados dos sensores via MQTT,
 * grava backup CSV no SD card e processa comandos remotos.
 */
void task_telemetry(void *pvParameters);

/**
 * @brief Publica um evento de auditoria via MQTT para a Dashboard Web3.
 * @param event_type Tipo de evento (BOOT, SHUTDOWN, OVERRIDE, THRESHOLD_CHANGE).
 * @param description Descrição legível do evento.
 */
void register_blockchain_audit_log(const char *event_type, const char *description);

/**
 * @brief Retorna o handle do cliente MQTT (para uso externo, se necessário).
 */
esp_mqtt_client_handle_t connectivity_get_mqtt_client(void);

#ifdef __cplusplus
}
#endif

#endif // CONNECTIVITY_H
