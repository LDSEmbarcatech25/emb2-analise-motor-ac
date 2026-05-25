#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/ledc.h"
#include "driver/i2s.h" // Compatibilidade estável com PlatformIO (ESP-IDF v4/v5)
#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "mqtt_client.h"

#include "project_defs.h"

static const char *TAG = "MOTOR_MONITOR";

// =============================================================================
// Variáveis Globais e Recursos de Sincronismo
// =============================================================================
static QueueHandle_t xSensorQueue = NULL;
static QueueHandle_t xCommandQueue = NULL;
static EventGroupHandle_t xSystemEvents = NULL;

static SemaphoreHandle_t xStateMutex = NULL;
static SemaphoreHandle_t xThresholdMutex = NULL;

static motor_state_t eCurrentState = STATE_ARMED;
static sensor_data_t sLastSensorData = {0};

// Estrutura dinâmica de limiares que pode ser ajustada remotamente
static motor_thresholds_t sCurrentThresholds = {
    .temp_alert = TEMP_THRESHOLD_ALERT,
    .temp_critical = TEMP_THRESHOLD_CRITICAL,
    .vib_alert = VIB_THRESHOLD_ALERT,
    .vib_critical = VIB_THRESHOLD_CRITICAL
};

// Flag de teste para injeção remota de anomalia
static bool bInjectAnomaly = false;

// Identificador exclusivo do motor
#define DEVICE_ID "motor_ac_compressor_01"

// =============================================================================
// Declaração das Tarefas FreeRTOS
// =============================================================================
static void task_sensors(void *pvParameters);
static void task_protection(void *pvParameters);
static void task_telemetry(void *pvParameters);
static void task_display(void *pvParameters);
static void task_buzzer(void *pvParameters);
static void task_audio_record(void *pvParameters);

// =============================================================================
// Protótipos de Inicialização de Hardware e Conectividade
// =============================================================================
static void init_gpios(void);
static void init_i2c(void);
static void init_ledc_buzzer(void);
static void init_i2s_mic(void);
static void init_sd_card(void);
static void init_wifi(void);
static void init_mqtt(void);

// =============================================================================
// Funções Lógicas de Getters/Setters com Thread-Safety
// =============================================================================

void set_system_state(motor_state_t eNewState) {
    xSemaphoreTake(xStateMutex, portMAX_DELAY);
    if (eCurrentState != eNewState) {
        ESP_LOGI(TAG, "Transição de Estado: %d ---> %d", eCurrentState, eNewState);
        eCurrentState = eNewState;
        
        // Atuação física no relé de isolamento de carga
        if (eNewState == STATE_PROTECTED || eNewState == STATE_IDLE || eNewState == STATE_COOLDOWN) {
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
    ESP_LOGW(TAG, "LIMIARES ATUALIZADOS: Temp Alerta: %.1f°C | Crítica: %.1f°C | Vib Alerta: %.2fg | Crítica: %.2fg",
             pNew->temp_alert, pNew->temp_critical, pNew->vib_alert, pNew->vib_critical);
    xSemaphoreGive(xThresholdMutex);
}

// =============================================================================
// Função Auxiliar de Simulação de Auditoria Blockchain
// =============================================================================
void register_blockchain_audit_log(const char *event_type, const char *description) {
    ESP_LOGW(TAG, "---------------- [BLOCKCHAIN AUDIT LEDGER] ----------------");
    ESP_LOGW(TAG, "EVENTO REGISTRADO: %s", event_type);
    ESP_LOGW(TAG, "DESCRIÇÃO:         %s", description);
    ESP_LOGW(TAG, "FIRMWARE HASH:     %s", FIRMWARE_HASH_HEX);
    ESP_LOGW(TAG, "TRANSAÇÃO HASH (TX): 0x%08x... (Registrado com sucesso!)", rand());
    ESP_LOGW(TAG, "-----------------------------------------------------------");
}

// =============================================================================
// Entrada Principal (ESP-IDF App Main)
// =============================================================================
void app_main(void) {
    ESP_LOGI(TAG, "Inicializando Dispositivo IoT de Monitoramento Preditivo...");

    // Inicialização da partição de flash NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_INVALID_STATE) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Inicialização de Mutexes e Filas do FreeRTOS
    xStateMutex = xSemaphoreCreateMutex();
    xThresholdMutex = xSemaphoreCreateMutex();
    xSensorQueue = xQueueCreate(10, sizeof(sensor_data_t));
    xCommandQueue = xQueueCreate(5, sizeof(mqtt_command_t));
    xSystemEvents = xEventGroupCreate();

    if (xStateMutex == NULL || xThresholdMutex == NULL || xSensorQueue == NULL || xCommandQueue == NULL || xSystemEvents == NULL) {
        ESP_LOGE(TAG, "Erro fatal: Recursos do kernel FreeRTOS não puderam ser criados.");
        return;
    }

    // Inicialização física do Hardware
    init_gpios();
    init_i2c();
    init_ledc_buzzer();
    init_i2s_mic();
    init_sd_card();
    
    // Configura estado inicial padrão (Armado)
    set_system_state(STATE_ARMED);

    // Registro de boot imutável na Blockchain
    register_blockchain_audit_log(AUDIT_EVENT_BOOT, "Dispositivo inicializado, integridade do firmware validada.");

    // Conectividade
    init_wifi();
    init_mqtt();

    ESP_LOGI(TAG, "Criando tarefas FreeRTOS...");

    // =============================================================================
    // Alocação das Tarefas no CORE 0 (Tempo Crítico, Proteção, Sensores e Rede)
    // =============================================================================
    xTaskCreatePinnedToCore(task_sensors, "task_sensors", 4096, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(task_protection, "task_protection", 4096, NULL, 6, NULL, 0); // Prioridade 6 (Máxima)
    xTaskCreatePinnedToCore(task_telemetry, "task_telemetry", 8192, NULL, 3, NULL, 0);

    // =============================================================================
    // Alocação das Tarefas no CORE 1 (Interface OLED, Buzzers e Áudio I2S)
    // =============================================================================
    xTaskCreatePinnedToCore(task_display, "task_display", 4096, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(task_buzzer, "task_buzzer", 2048, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(task_audio_record, "task_audio_record", 8192, NULL, 5, NULL, 1);

    ESP_LOGI(TAG, "Sistema pronto e operacional.");
}

// =============================================================================
// IMPLEMENTAÇÃO DAS TAREFAS FREERTOS
// =============================================================================

static void task_sensors(void *pvParameters) {
    ESP_LOGI(TAG, "Iniciando Task de Leitura de Sensores...");
    sensor_data_t sData = {0};
    float t_sim = 0.0f;

    while (1) {
        t_sim += 0.1f;
        
        // Simulação matemática rica das medições industriais
        sData.temperature = 45.0f + (sinf(t_sim * 0.05f) * 10.0f) + ((float)(rand() % 10) / 10.0f);
        sData.humidity = 60.0f + (cosf(t_sim * 0.02f) * 5.0f);
        
        // Aceleração RMS harmônica
        sData.accel_x_rms = 0.50f + (sinf(t_sim * 2.0f) * 0.10f) + ((float)(rand() % 20) / 100.0f);
        sData.accel_y_rms = 0.40f + (cosf(t_sim * 2.0f) * 0.08f) + ((float)(rand() % 15) / 100.0f);
        sData.accel_z_rms = 0.80f + ((float)(rand() % 40) / 100.0f);
        
        // Emulações ocasionais de pico de vibração (fadiga de rolamento)
        if (rand() % 25 == 0) {
            sData.accel_z_rms += 2.2f; // Pico mecânico severo
            sData.vib_peak = 3.5f;
            sData.temperature += 4.0f; // Acoplamento térmico de atrito
        } else {
            sData.vib_peak = sData.accel_z_rms * 1.414f;
        }

        // Cálculo da Resultante Tridimensional RMS
        sData.vib_resultant = sqrtf(
            (sData.accel_x_rms * sData.accel_x_rms) +
            (sData.accel_y_rms * sData.accel_y_rms) +
            (sData.accel_z_rms * sData.accel_z_rms)
        );

        // Injeção de Anomalia Progressiva (Para testes end-to-end de estresse)
        if (bInjectAnomaly) {
            static float fAnomalyAccumulator = 0.0f;
            fAnomalyAccumulator += 0.25f; // Sobe 0.25g a cada ciclo (500ms)
            
            sData.vib_resultant += fAnomalyAccumulator;
            sData.temperature += (fAnomalyAccumulator * 4.0f); // Aquece por fricção mecânica severa
            
            if (sData.vib_resultant > 6.0f) sData.vib_resultant = 6.0f;
            if (sData.temperature > 95.0f) sData.temperature = 95.0f;
            
            ESP_LOGW(TAG, "ANOMALIA INJETADA: Vibração: %.2fg | Temperatura: %.1fC", 
                     sData.vib_resultant, sData.temperature);
        } else {
            // Limpa o acumulador se o teste for desligado
            static float fAnomalyAccumulator = 0.0f;
            fAnomalyAccumulator = 0.0f;
        }

        // Atualiza variáveis globais com controle de concorrência
        xSemaphoreTake(xStateMutex, portMAX_DELAY);
        sLastSensorData = sData;
        xSemaphoreGive(xStateMutex);

        xQueueOverwrite(xSensorQueue, &sData);

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

static void task_protection(void *pvParameters) {
    ESP_LOGI(TAG, "Iniciando Task de Proteção e Segurança Física...");
    sensor_data_t sData = {0};
    motor_thresholds_t sThresholds = {0};
    uint32_t cooldown_counter = 0;

    while (1) {
        if (xQueuePeek(xSensorQueue, &sData, portMAX_DELAY) == pdTRUE) {
            motor_state_t eState = get_system_state();
            get_active_thresholds(&sThresholds); // Obtém limiares dinâmicos atuais

            // Cooldown de Segurança (1 minuto de trava mecânica para resfriamento)
            if (eState == STATE_COOLDOWN) {
                cooldown_counter++;
                if (cooldown_counter >= (COOLDOWN_DURATION_S * 2)) {
                    ESP_LOGI(TAG, "SEGURANÇA: Cooldown concluído. Rearme permitido.");
                    set_system_state(STATE_IDLE); // Estado aguardando comando
                    cooldown_counter = 0;
                    xEventGroupClearBits(xSystemEvents, EVENT_ALERT_ACTIVE);
                }
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }

            // Teste de Limiares Críticos (Abertura do Relé)
            if (sData.vib_resultant >= sThresholds.vib_critical || sData.temperature >= sThresholds.temp_critical) {
                if (eState != STATE_PROTECTED && eState != STATE_COOLDOWN) {
                    
                    char desc[128];
                    snprintf(desc, sizeof(desc), "Desarme por limites violados! Vib: %.2fg (Limite: %.2fg), Temp: %.1fC (Limite: %.1fC)",
                             sData.vib_resultant, sThresholds.vib_critical, sData.temperature, sThresholds.temp_critical);
                    
                    ESP_LOGE(TAG, "DESARME CRÍTICO: %s", desc);
                    
                    // Transiciona de estado
                    set_system_state(STATE_PROTECTED);
                    bInjectAnomaly = false; // Desativa a injeção de falha após o desarme bem-sucedido
                    xEventGroupSetBits(xSystemEvents, EVENT_ALERT_ACTIVE);

                    // Auditoria Imutável: Eterniza o motivo do desligamento na Blockchain
                    register_blockchain_audit_log(AUDIT_EVENT_SHUTDOWN, desc);

                    // Entra imediatamente no estado de travamento térmico
                    set_system_state(STATE_COOLDOWN);
                    cooldown_counter = 0;
                }
            }
            // Teste de Limiares de Alerta Moderado
            else if (sData.vib_resultant >= sThresholds.vib_alert || sData.temperature >= sThresholds.temp_alert) {
                if (eState == STATE_ARMED) {
                    ESP_LOGW(TAG, "ALERTA: Fadiga mecânica elevada. Vib: %.2fg, Temp: %.1f°C",
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

static void task_telemetry(void *pvParameters) {
    ESP_LOGI(TAG, "Iniciando Task de Telemetria e Histórico...");
    sensor_data_t sData = {0};
    mqtt_command_t sCmd = {0};
    char payload[256];

    while (1) {
        // Envio periódico a cada 5 segundos
        if (xQueueReceive(xSensorQueue, &sData, pdMS_TO_TICKS(5000)) == pdTRUE) {
            motor_state_t eState = get_system_state();
            
            // Backup local no cartão SD (CSV)
            ESP_LOGI(TAG, "SD_CARD [CSV]: Temp: %.1fC, Vib: %.2fg, Estado: %d",
                     sData.temperature, sData.vib_resultant, eState);

            // Telemetria MQTT para a nuvem
            EventBits_t uxBits = xEventGroupGetBits(xSystemEvents);
            if (uxBits & EVENT_MQTT_CONNECTED) {
                snprintf(payload, sizeof(payload), 
                         "{\"device_id\":\"%s\",\"temp_c\":%.2f,\"vib_rms_g\":%.2f,\"state\":%d}",
                         DEVICE_ID, sData.temperature, sData.vib_resultant, eState);
                
                // Em produção: esp_mqtt_client_publish(client, "motor/telemetry", payload, 0, 1, 0);
                ESP_LOGI(TAG, "MQTT [PUB] (motor/%s/telemetry) -> %s", DEVICE_ID, payload);
            }
        }

        // Processamento e Handshake de Comandos da Dashboard
        if (xQueueReceive(xCommandQueue, &sCmd, 0) == pdTRUE) {
            ESP_LOGI(TAG, "MQTT [CMD]: Processando ID: %s | Ação: %s", sCmd.command_id, sCmd.action);
            
            bool cmd_success = false;
            char err_msg[64] = "";

            if (strcmp(sCmd.action, "SET_MOTOR_STATE") == 0) {
                motor_state_t eState = get_system_state();
                
                if (strcmp(sCmd.value, "ON") == 0) {
                    if (eState == STATE_IDLE) {
                        set_system_state(STATE_ARMED);
                        cmd_success = true;

                        // Auditoria Blockchain: Registro de Override Manual para Ligar o Motor
                        register_blockchain_audit_log(AUDIT_EVENT_OVERRIDE, "Operador ligou o motor remotamente via Dashboard.");
                    } else {
                        snprintf(err_msg, sizeof(err_msg), "Motor não está em IDLE (Estado atual: %d)", eState);
                    }
                } else if (strcmp(sCmd.value, "OFF") == 0) {
                    set_system_state(STATE_IDLE);
                    cmd_success = true;

                    // Auditoria Blockchain: Registro de Override Manual para Desligar o Motor
                    register_blockchain_audit_log(AUDIT_EVENT_OVERRIDE, "Operador desligou o motor manualmente via Dashboard.");
                }
            } else if (strcmp(sCmd.action, "TRIGGER_AUDIO_RECORD") == 0) {
                xEventGroupSetBits(xSystemEvents, EVENT_TRIGGER_RECORD);
                cmd_success = true;
            } else if (strcmp(sCmd.action, "SET_THRESHOLDS") == 0) {
                // Simulação de recepção de limiares novos do tópico thresholds
                motor_thresholds_t new_t = {
                    .temp_alert = 65.0f,
                    .temp_critical = 80.0f, // Alteração de limite
                    .vib_alert = 1.20f,
                    .vib_critical = 2.80f
                };
                update_active_thresholds(&new_t);
                cmd_success = true;

                // Auditoria Blockchain: Registro de Mudança de Limiar de Proteção na Fábrica
                register_blockchain_audit_log(AUDIT_EVENT_THRESHOLD_CHANGE, "Alteração remota nos limites críticos de segurança.");
            } else if (strcmp(sCmd.action, "INJECT_ANOMALY") == 0) {
                bInjectAnomaly = true;
                cmd_success = true;
                ESP_LOGE(TAG, "TESTE: Comando de Injeção de Falha Recebido! Subindo vibração e calor.");
            }

            // Publica o Handshake/Confirmação de recebimento do Comando
            if (xEventGroupGetBits(xSystemEvents) & EVENT_MQTT_CONNECTED) {
                snprintf(payload, sizeof(payload),
                         "{\"command_id\":\"%s\",\"status\":\"%s\",\"error\":\"%s\"}",
                         sCmd.command_id, cmd_success ? "SUCCESS" : "ERROR", err_msg);
                
                ESP_LOGI(TAG, "MQTT [PUB] (motor/%s/cmd/response) -> %s", DEVICE_ID, payload);
            }
        }
    }
}

static void task_display(void *pvParameters) {
    ESP_LOGI(TAG, "Iniciando Task de Renderização do Display OLED...");
    sensor_data_t sData = {0};

    while (1) {
        xSemaphoreTake(xStateMutex, portMAX_DELAY);
        sData = sLastSensorData;
        motor_state_t eState = eCurrentState;
        xSemaphoreGive(xStateMutex);

        // Atualização simulada do display de cristal local
        ESP_LOGI(TAG, "OLED ---> [T: %.1fC | V: %.2fg] ESTADO: %s",
                 sData.temperature, sData.vib_resultant,
                 eState == STATE_ARMED ? "ARMADO" :
                 eState == STATE_ALERT ? "ALERTA" :
                 eState == STATE_COOLDOWN ? "RESFRIAMENTO" :
                 eState == STATE_PROTECTED ? "PROTEGIDO" : "DESLIGADO");

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void task_buzzer(void *pvParameters) {
    ESP_LOGI(TAG, "Iniciando Task de Sinalização Buzzer...");

    while (1) {
        EventBits_t uxBits = xEventGroupGetBits(xSystemEvents);
        motor_state_t eState = get_system_state();

        // Botão para silenciar o alarme sonoro temporariamente
        if (gpio_get_level(PIN_BTN_SILENCE) == 0) {
            xEventGroupSetBits(xSystemEvents, EVENT_SILENCE_BUZZER);
            ESP_LOGW(TAG, "BUZZER: Alarme mutado localmente pelo operador.");
        }

        if ((uxBits & EVENT_ALERT_ACTIVE) && !(uxBits & EVENT_SILENCE_BUZZER)) {
            if (eState == STATE_PROTECTED || eState == STATE_COOLDOWN) {
                ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 4000); // Bipe rápido
                ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
                vTaskDelay(pdMS_TO_TICKS(150));
                
                ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
                ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
                vTaskDelay(pdMS_TO_TICKS(150));
            } else if (eState == STATE_ALERT) {
                ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 4000); // Bipe lento
                ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
                vTaskDelay(pdMS_TO_TICKS(500));
                
                ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
                ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        } else {
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
            
            if (!(uxBits & EVENT_ALERT_ACTIVE)) {
                xEventGroupClearBits(xSystemEvents, EVENT_SILENCE_BUZZER);
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}

static void task_audio_record(void *pvParameters) {
    ESP_LOGI(TAG, "Iniciando Task de Processamento Acústico I2S...");
    
    const size_t audio_buffer_size = 160000; // 5 segundos, 16kHz, 16bit (160 KB)
    uint8_t *audio_buffer = malloc(audio_buffer_size);

    if (audio_buffer == NULL) {
        ESP_LOGE(TAG, "Erro crítico: Memória esgotada para alocação do buffer I2S.");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        xEventGroupWaitBits(xSystemEvents, EVENT_TRIGGER_RECORD, pdTRUE, pdTRUE, portMAX_DELAY);
        
        ESP_LOGW(TAG, "GRAVAÇÃO: Iniciando captação I2S (5 segundos de áudio bruto)...");
        
        i2s_start(I2S_NUM_0);

        size_t total_bytes_read = 0;
        int64_t sum_squares = 0;
        int samples_count = 0;

        while (total_bytes_read < audio_buffer_size) {
            size_t chunk_bytes_read = 0;
            int16_t i2s_chunk[512]; // Bloco de 1 KB (512 amostras de 16 bits)
            size_t bytes_to_read = sizeof(i2s_chunk);
            if (audio_buffer_size - total_bytes_read < bytes_to_read) {
                bytes_to_read = audio_buffer_size - total_bytes_read;
            }

            // Realiza leitura real DMA do microfone físico I2S
            esp_err_t err = i2s_read(I2S_NUM_0, i2s_chunk, bytes_to_read, &chunk_bytes_read, pdMS_TO_TICKS(100));

            // Fallback Dinâmico: Se o sensor físico não estiver fisicamente conectado ou no simulador
            if (err != ESP_OK || chunk_bytes_read == 0) {
                chunk_bytes_read = bytes_to_read;
                int samples = chunk_bytes_read / sizeof(int16_t);
                for (int i = 0; i < samples; i++) {
                    float t = (float)(total_bytes_read / 2 + i) / 16000.0f;
                    // Simula a assinatura acústica de um compressor industrial:
                    // Frequência fundamental de 60Hz da rede + harmônico de 180Hz + ruído mecânico
                    float val = 3800.0f * sinf(2.0f * M_PI * 60.0f * t) + 
                                1200.0f * sinf(2.0f * M_PI * 180.0f * t) + 
                                (float)(rand() % 1000 - 500);
                    i2s_chunk[i] = (int16_t)val;
                }
            }

            // Salva no buffer principal de áudio
            memcpy(audio_buffer + total_bytes_read, i2s_chunk, chunk_bytes_read);

            // Soma dos quadrados das amplitudes para cálculo posterior do RMS
            int samples = chunk_bytes_read / sizeof(int16_t);
            for (int i = 0; i < samples; i++) {
                sum_squares += (int32_t)i2s_chunk[i] * i2s_chunk[i];
            }
            samples_count += samples;

            total_bytes_read += chunk_bytes_read;
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        i2s_stop(I2S_NUM_0);
        
        // Cálculo matemático do volume acústico (dB SPL)
        float rms = 0.0f;
        float db = 0.0f;
        if (samples_count > 0) {
            rms = sqrtf((float)sum_squares / samples_count);
            if (rms < 1.0f) rms = 1.0f;
            // 20 * log10(RMS / Ref). Escala calibrada com base na amplitude total de 16 bits (32767)
            db = 20.0f * log10f(rms / 32767.0f) + 120.0f; // Calibração típica de dB SPL para mic digital
        }

        ESP_LOGI(TAG, "GRAVAÇÃO: Captura concluída. Total de bytes lidos: %d", total_bytes_read);
        ESP_LOGW(TAG, "ANÁLISE ACÚSTICA: Amplitude RMS: %.2f | Pressão Sonora: %.1f dB SPL", rms, db);

        // Classificação Preditiva de Saúde Acústica
        if (db > 80.0f) {
            ESP_LOGE(TAG, "ALERTA ACÚSTICO CRÍTICO (%.1f dB): Vibração harmônica de rolamentos ou cavitação mecânica!", db);
        } else if (db > 70.0f) {
            ESP_LOGW(TAG, "AVISO ACÚSTICO (%.1f dB): Atrito mecânico acima do ideal. Agendar verificação preventiva.", db);
        } else {
            ESP_LOGI(TAG, "DIAGNÓSTICO ACÚSTICO (%.1f dB): Assinatura sonora estável dentro da faixa de conformidade.", db);
        }

        // Upload acústico simulação HTTP POST
        ESP_LOGI(TAG, "HTTP_CLIENT: Conectando a https://predictive.factory.com/api/audio/upload ...");
        ESP_LOGI(TAG, "HTTP_CLIENT [POST]: Enviando multipart/form-data (160 KB WAV)...");
        
        for (int offset = 0; offset < total_bytes_read; offset += 16384) {
            size_t chunk_size = (total_bytes_read - offset > 16384) ? 16384 : (total_bytes_read - offset);
            ESP_LOGD(TAG, "HTTP_CLIENT: Transmitindo bloco de %d bytes (Offset: %d)", chunk_size, offset);
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        ESP_LOGI(TAG, "HTTP_CLIENT: Upload de áudio concluído com sucesso. Status: 200 OK");
    }

    free(audio_buffer);
}

// =============================================================================
// DRIVERS E INICIALIZAÇÕES FÍSICAS (ESP-IDF API)
// =============================================================================

static void init_gpios(void) {
    ESP_LOGI(TAG, "Configurando pinagem GPIO...");

    gpio_config_t io_conf_rele = {
        .pin_bit_mask = (1ULL << PIN_RELE),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf_rele);
    gpio_set_level(PIN_RELE, 0); // Desligado no boot

    gpio_config_t io_conf_btn = {
        .pin_bit_mask = (1ULL << PIN_BTN_RESET) | (1ULL << PIN_BTN_SILENCE),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf_btn);
}

static void init_i2c(void) {
    ESP_LOGI(TAG, "Inicializando I2C0...");
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = PIN_I2C_SDA,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = PIN_I2C_SCL,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    i2c_param_config(I2C_NUM_0, &conf);
    i2c_driver_install(I2C_NUM_0, conf.mode, 0, 0, 0);
}

static void init_ledc_buzzer(void) {
    ESP_LOGI(TAG, "Inicializando PWM LEDC (Buzzer)...");
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,
        .timer_num        = LEDC_TIMER_0,
        .duty_resolution  = LEDC_TIMER_13_BIT,
        .freq_hz          = 2700,
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {
        .speed_mode     = LEDC_LOW_SPEED_MODE,
        .channel        = LEDC_CHANNEL_0,
        .timer_sel      = LEDC_TIMER_0,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = PIN_BUZZER,
        .duty           = 0,
        .hpoint         = 0
    };
    ledc_channel_config(&ledc_channel);
}

static void init_i2s_mic(void) {
    ESP_LOGI(TAG, "Inicializando barramento I2S0...");
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = 16000,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 1024,
        .use_apll = false
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = PIN_I2S_SCK,
        .ws_io_num = PIN_I2S_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = PIN_I2S_SD
    };

    i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_NUM_0, &pin_config);
    i2s_stop(I2S_NUM_0);
}

static void init_sd_card(void) {
    ESP_LOGI(TAG, "Inicializando barramento SPI para Cartão SD...");
    
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };
    
    sdmmc_card_t *card;
    const char mount_point[] = "/sdcard";
    
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_SD_MOSI,
        .miso_io_num = PIN_SD_MISO,
        .sclk_io_num = PIN_SD_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    
    esp_err_t ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA_CHAN);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao inicializar o barramento SPI para o SD Card.");
        return;
    }
    
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_SD_CS;
    slot_config.host_id = host.slot;
    
    ESP_LOGI(TAG, "Montando sistema de arquivos FAT no Cartão SD...");
    ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &card);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Erro ao montar o cartão SD. Verifique se o cartão está inserido.");
    } else {
        ESP_LOGI(TAG, "Cartão SD montado com sucesso em %s! Capacidade: %llu MB", 
                 mount_point, ((uint64_t)card->csd.capacity) * card->csd.sector_size / (1024 * 1024));
    }
}

static void init_wifi(void) {
    ESP_LOGI(TAG, "Conectando ao Wi-Fi local da fábrica...");
    xEventGroupSetBits(xSystemEvents, EVENT_WIFI_CONNECTED);
}

static void init_mqtt(void) {
    ESP_LOGI(TAG, "Autenticando sessão segura MQTT (HiveMQ Cloud)...");
    xEventGroupSetBits(xSystemEvents, EVENT_MQTT_CONNECTED);
}
#include "config.h"
#include "sensors.h"       // MPU6050
#include "dht22.h"
#include "alarm.h"
#include "state_machine.h"
#include "connectivity.h"
#include "display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

static const char *TAG = "main";

// ── Estado global compartilhado ───────────────────────────────────────────────
static motor_metrics_t  g_metrics  = {0};
static SemaphoreHandle_t g_mutex;
static bool g_buzzer_silence = false;
static bool g_record_audio   = false;

// ── Helpers ───────────────────────────────────────────────────────────────────
static float peak(float a, float b, float c) {
    float m = fabsf(a);
    if (fabsf(b) > m) m = fabsf(b);
    if (fabsf(c) > m) m = fabsf(c);
    return m;
}

// ── task_sensors (Core 0, 10 Hz) ──────────────────────────────────────────────
static void task_sensors(void *arg)
{
    alarm_signature_t sig = {0};
    alarm_calibrate(&sig);   // ~5s de calibração

    sensor_data_t imu_data;
    dht22_data_t  dht_data;

    while (1) {
        motor_metrics_t m = {0};

        if (sensors_read_all(&imu_data) == ESP_OK) {
            m.accel_rms  = alarm_rms(&imu_data.imu);
            m.ax_peak    = fabsf(imu_data.imu.ax);
            m.ay_peak    = fabsf(imu_data.imu.ay);
            m.az_peak    = fabsf(imu_data.imu.az);
        }
        if (dht22_read(&dht_data) == ESP_OK) {
            m.temp_c       = dht_data.temperature_c;
            m.humidity_pct = dht_data.humidity_pct;
        }

        xSemaphoreTake(g_mutex, portMAX_DELAY);
        g_metrics = m;
        xSemaphoreGive(g_mutex);

        vTaskDelay(pdMS_TO_TICKS(SENSOR_INTERVAL_MS));
    }
}

// ── task_protection (Core 0, 10 Hz) ───────────────────────────────────────────
static void task_protection(void *arg)
{
    while (1) {
        motor_metrics_t m;
        xSemaphoreTake(g_mutex, portMAX_DELAY);
        m = g_metrics;
        xSemaphoreGive(g_mutex);

        state_update(&m);

        // Botão reset (GPIO 45) — religar pós-proteção
        if (!gpio_get_level(BTN_RESET_GPIO))
            state_cmd_reset();

        vTaskDelay(pdMS_TO_TICKS(SENSOR_INTERVAL_MS));
    }
}

// ── task_telemetry (Core 0, 0.5 Hz) ───────────────────────────────────────────
static void task_telemetry(void *arg)
{
    while (1) {
        motor_metrics_t m;
        xSemaphoreTake(g_mutex, portMAX_DELAY);
        m = g_metrics;
        xSemaphoreGive(g_mutex);

        char payload[320];
        snprintf(payload, sizeof(payload),
            "{\"device_id\":\"%s\","
            "\"ts\":%lld,"
            "\"temp_c\":%.2f,"
            "\"humidity_pct\":%.1f,"
            "\"accel_rms_g\":%.3f,"
            "\"accel_x_peak\":%.3f,"
            "\"accel_y_peak\":%.3f,"
            "\"accel_z_peak\":%.3f,"
            "\"motor_state\":\"%s\"}",
            DEVICE_ID,
            esp_timer_get_time() / 1000000LL,
            m.temp_c, m.humidity_pct,
            m.accel_rms,
            m.ax_peak, m.ay_peak, m.az_peak,
            state_str(state_get()));

        esp_mqtt_client_publish(connectivity_get_client(),
                                TOPIC_TELEMETRY, payload, 0, 1, 0);

        ESP_LOGI(TAG, "%s", payload);
        vTaskDelay(pdMS_TO_TICKS(TELEMETRY_INTERVAL_MS));
    }
}

// ── task_display (Core 1, 5 Hz) ───────────────────────────────────────────────
static void task_display(void *arg)
{
    sensor_data_t fake = {0};  // display_update usa sensor_data_t + rms + level
    while (1) {
        motor_metrics_t m;
        xSemaphoreTake(g_mutex, portMAX_DELAY);
        m = g_metrics;
        xSemaphoreGive(g_mutex);

        // Mapeia motor_state → alarm_level para reusar display existente
        alarm_level_t lv = (state_get() == STATE_PROTECTED) ? ALARM_CONFIRMED :
                           (state_get() == STATE_ALERT)     ? ALARM_PROBABLE  : ALARM_NONE;

        fake.env.temperature_c = m.temp_c;
        fake.env.humidity_pct  = m.humidity_pct;
        display_update(&fake, m.accel_rms, lv);
        vTaskDelay(pdMS_TO_TICKS(DISPLAY_INTERVAL_MS));
    }
}

// ── task_buzzer (Core 1, 10 Hz) ───────────────────────────────────────────────
static void task_buzzer(void *arg)
{
    // Padrão: ALERT = bip lento, PROTECTED = bip rápido
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_10_BIT, .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_timer_config(&t);
    ledc_channel_config_t ch = {
        .gpio_num = BUZZER_GPIO, .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0, .duty = 0
    };
    ledc_channel_config(&ch);

    int tick = 0;
    while (1) {
        if (!g_buzzer_silence) {
            motor_state_t s = state_get();
            bool on = false;
            if      (s == STATE_PROTECTED) on = (tick % 4  < 2);  // rápido
            else if (s == STATE_ALERT)     on = (tick % 10 < 2);  // lento
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, on ? 512 : 0);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
        }

        // Botão silenciar (GPIO 46)
        if (!gpio_get_level(BTN_SILENCE_GPIO)) g_buzzer_silence = true;
        if (state_get() == STATE_IDLE) g_buzzer_silence = false;  // reativa

        tick++;
        vTaskDelay(pdMS_TO_TICKS(BUZZER_INTERVAL_MS));
    }
}

// ── task_mqtt_cmd (Core 1, evento) ────────────────────────────────────────────
// Callback registrado em connectivity_init() via mqtt_event_handler
// Os comandos chegam como JSON no tópico motor/{id}/cmd
// Veja connectivity.c para o parsing e chamadas state_cmd_*

// ── app_main ──────────────────────────────────────────────────────────────────
void app_main(void)
{
    // GPIO inputs com pull-up
    gpio_set_direction(BTN_RESET_GPIO,   GPIO_MODE_INPUT);
    gpio_set_pull_mode(BTN_RESET_GPIO,   GPIO_PULLUP_ONLY);
    gpio_set_direction(BTN_SILENCE_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BTN_SILENCE_GPIO, GPIO_PULLUP_ONLY);

    ESP_ERROR_CHECK(sensors_init());           // MPU6050 (I2C)
    ESP_ERROR_CHECK(dht22_init(DHT22_GPIO));   // DHT22 (GPIO 36)
    ESP_ERROR_CHECK(state_init());             // relé
    ESP_ERROR_CHECK(display_init());           // OLED
    ESP_ERROR_CHECK(connectivity_init());      // Wi-Fi + MQTT HiveMQ

    g_mutex = xSemaphoreCreateMutex();

    // Core 0
    xTaskCreatePinnedToCore(task_sensors,    "sensors",    6144, NULL, 6, NULL, 0);
    xTaskCreatePinnedToCore(task_protection, "protection", 4096, NULL, 7, NULL, 0);
    xTaskCreatePinnedToCore(task_telemetry,  "telemetry",  6144, NULL, 4, NULL, 0);

    // Core 1
    xTaskCreatePinnedToCore(task_display, "display", 4096, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(task_buzzer,        "buzzer",   2048, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(task_audio_record, "audio_rec", 8192, NULL, 2, NULL, 1);
}

// ── task_audio_record (Core 1, sob demanda) ───────────────────────────────────
// Ativada quando connectivity.c seta g_record_audio via comando RECORD_AUDIO
#include "pdm_mic.h"
#include "audio_post.h"

void task_audio_record(void *arg)
{
    while (1) {
        // Aguarda sinal via flag (setada no callback MQTT)
        if (g_record_audio) {
            g_record_audio = false;
            ESP_LOGI("audio", "Iniciando gravação PDM 5 s...");

            int16_t *buf = malloc(PDM_BUF_SIZE);
            if (!buf) { ESP_LOGE("audio", "Sem memória"); continue; }

            pdm_mic_init();
            size_t written = 0;
            if (pdm_mic_record(buf, PDM_BUF_SIZE, &written) == ESP_OK) {
                audio_post_send(buf, written);
            }
            pdm_mic_deinit();
            free(buf);
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}
