/**
 * @file state_machine.c
 * @brief Máquina de estados e proteção do motor AC Kiira.
 *
 * Extrai a lógica de proteção, transição de estados, atuação de relé,
 * e gerenciamento de limiares dinâmicos do main.c original.
 */

#include "state_machine.h"
#include "config.h"
#include "esp_log.h"
#include "driver/gpio.h"

static const char *TAG = "STATE_MACHINE";

// Estado interno do módulo
static motor_state_t eCurrentState = STATE_ARMED;
static motor_thresholds_t sCurrentThresholds = {
    .temp_alert    = TEMP_THRESHOLD_ALERT,
    .temp_critical = TEMP_THRESHOLD_CRITICAL,
    .vib_alert     = VIB_THRESHOLD_ALERT,
    .vib_critical  = VIB_THRESHOLD_CRITICAL
};

// Mutexes gerenciados internamente
static SemaphoreHandle_t xStateMutex     = NULL;
static SemaphoreHandle_t xThresholdMutex = NULL;

// Referências externas (criadas no main.c)
extern QueueHandle_t      xSensorQueue;
extern EventGroupHandle_t xSystemEvents;

// Declaração externa da função de auditoria (connectivity module)
extern void register_blockchain_audit_log(const char *event_type, const char *description);

// Declaração externa da função de injeção de anomalia (sensors module)
extern void sensors_inject_anomaly(bool bEnable);

// =============================================================================
// Inicialização
// =============================================================================

void state_machine_init(void) {
    xStateMutex     = xSemaphoreCreateMutex();
    xThresholdMutex = xSemaphoreCreateMutex();

    if (!xStateMutex || !xThresholdMutex) {
        ESP_LOGE(TAG, "Erro fatal: não foi possível criar mutexes da máquina de estados.");
    }
}

void init_gpios(void) {
    ESP_LOGI(TAG, "Configurando pinagem GPIO...");

    gpio_config_t io_conf_rele = {
        .pin_bit_mask  = (1ULL << PIN_RELE),
        .mode          = GPIO_MODE_OUTPUT,
        .pull_up_en    = GPIO_PULLUP_DISABLE,
        .pull_down_en  = GPIO_PULLDOWN_ENABLE,
        .intr_type     = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf_rele);
    gpio_set_level(PIN_RELE, 0); // Desligado no boot

    gpio_config_t io_conf_btn = {
        .pin_bit_mask = (1ULL << PIN_BTN_RESET) | (1ULL << PIN_BTN_SILENCE) |
                        (1ULL << PIN_BTN_DOWN)  | (1ULL << PIN_BTN_SELECT),
        .mode          = GPIO_MODE_INPUT,
        .pull_up_en    = GPIO_PULLUP_ENABLE,
        .pull_down_en  = GPIO_PULLDOWN_DISABLE,
        .intr_type     = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf_btn);
}

// =============================================================================
// Getters / Setters Thread-Safe
// =============================================================================

void set_system_state(motor_state_t eNewState) {
    xSemaphoreTake(xStateMutex, portMAX_DELAY);
    if (eCurrentState != eNewState) {
        ESP_LOGI(TAG, "Transição de Estado: %d ---> %d", eCurrentState, eNewState);
        eCurrentState = eNewState;

        // Atuação física no relé de isolamento de carga
        if (eNewState == STATE_PROTECTED || eNewState == STATE_IDLE ||
            eNewState == STATE_COOLDOWN) {
            gpio_set_level(PIN_RELE, 0); // Abre o relé (Desliga o motor por segurança)
            ESP_LOGW(TAG, "SEGURANÇA: Relé aberto. Motor DESLIGADO.");
        } else {
            gpio_set_level(PIN_RELE, 1); // Fecha o relé (Energiza o motor)
            ESP_LOGI(TAG, "SEGURANÇA: Relé fechado. Motor ENERGIZADO.");
        }
    }
    xSemaphoreGive(xStateMutex);
}

motor_state_t get_system_state(void) {
    motor_state_t eState;
    xSemaphoreTake(xStateMutex, portMAX_DELAY);
    eState = eCurrentState;
    xSemaphoreGive(xStateMutex);
    return eState;
}

void get_active_thresholds(motor_thresholds_t *pDest) {
    xSemaphoreTake(xThresholdMutex, portMAX_DELAY);
    *pDest = sCurrentThresholds;
    xSemaphoreGive(xThresholdMutex);
}

void update_active_thresholds(const motor_thresholds_t *pNew) {
    xSemaphoreTake(xThresholdMutex, portMAX_DELAY);
    sCurrentThresholds = *pNew;
    ESP_LOGW(TAG,
             "LIMIARES ATUALIZADOS: Temp Alerta: %.1f°C | Crítica: %.1f°C | Vib "
             "Alerta: %.2fg | Crítica: %.2fg",
             pNew->temp_alert, pNew->temp_critical, pNew->vib_alert,
             pNew->vib_critical);
    xSemaphoreGive(xThresholdMutex);
}

// =============================================================================
// Task de Proteção
// =============================================================================

void task_protection(void *pvParameters) {
    ESP_LOGI(TAG, "Iniciando Task de Proteção e Segurança Física...");
    sensor_data_t sData = {0};
    motor_thresholds_t sThresholds = {0};
    uint32_t cooldown_counter = 0;

    while (1) {
        if (xQueuePeek(xSensorQueue, &sData, pdMS_TO_TICKS(600)) == pdTRUE) {
            motor_state_t eState = get_system_state();
            get_active_thresholds(&sThresholds);

            // Cooldown de Segurança (1 minuto de trava mecânica para resfriamento)
            if (eState == STATE_COOLDOWN) {
                cooldown_counter++;
                if (cooldown_counter >= (COOLDOWN_DURATION_S * 2)) {
                    ESP_LOGI(TAG, "SEGURANÇA: Cooldown concluído. Rearme permitido.");
                    set_system_state(STATE_IDLE);
                    cooldown_counter = 0;
                    xEventGroupClearBits(xSystemEvents, EVENT_ALERT_ACTIVE);
                }
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }

            // Teste de Limiares Críticos (Abertura do Relé)
            if (sData.vib_resultant >= sThresholds.vib_critical ||
                sData.temperature >= sThresholds.temp_critical) {
                if (eState != STATE_PROTECTED && eState != STATE_COOLDOWN) {

                    char desc[128];
                    snprintf(desc, sizeof(desc),
                             "Desarme por limites violados! Vib: %.2fg (Limite: %.2fg), "
                             "Temp: %.1fC (Limite: %.1fC)",
                             sData.vib_resultant, sThresholds.vib_critical,
                             sData.temperature, sThresholds.temp_critical);

                    ESP_LOGE(TAG, "DESARME CRÍTICO: %s", desc);

                    // Transiciona de estado
                    set_system_state(STATE_PROTECTED);
                    sensors_inject_anomaly(false);
                    xEventGroupSetBits(xSystemEvents, EVENT_ALERT_ACTIVE);

                    // Auditoria Imutável: Eterniza o motivo do desligamento na Blockchain
                    register_blockchain_audit_log(AUDIT_EVENT_SHUTDOWN, desc);

                    // Entra imediatamente no estado de travamento térmico
                    set_system_state(STATE_COOLDOWN);
                    cooldown_counter = 0;
                }
            }
            // Teste de Limiares de Alerta Moderado
            else if (sData.vib_resultant >= sThresholds.vib_alert ||
                     sData.temperature >= sThresholds.temp_alert) {
                if (eState == STATE_ARMED) {
                    ESP_LOGW(TAG,
                             "ALERTA: Fadiga mecânica elevada. Vib: %.2fg, Temp: %.1f°C",
                             sData.vib_resultant, sData.temperature);
                    set_system_state(STATE_ALERT);
                    xEventGroupSetBits(xSystemEvents, EVENT_ALERT_ACTIVE);
                }
            }
            // Retorna ao estado normal caso os alarmes cessem
            else {
                if (eState == STATE_ALERT) {
                    set_system_state(STATE_ARMED);
                    xEventGroupClearBits(xSystemEvents, EVENT_ALERT_ACTIVE);
                }
            }

            // Rearme via botão físico local (somente em IDLE)
            if (gpio_get_level(PIN_BTN_RESET) == 0) {
                if (eState == STATE_IDLE) {
                    ESP_LOGI(TAG, "SEGURANÇA: Rearme físico via botão táctil.");
                    set_system_state(STATE_ARMED);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
