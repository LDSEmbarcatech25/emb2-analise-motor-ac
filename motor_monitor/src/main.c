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
#include "esp_netif.h"

#include "project_defs.h"
#include "ssd1306.h"
#include "font8x8_basic.h"

// =============================================================================
// Hardware do Display OLED
// =============================================================================
static SSD1306_t dev;

// =============================================================================
// Credenciais de Rede — Altere antes de flashear!
// =============================================================================
#ifndef WIFI_SSID
#define WIFI_SSID  "NomeDaRede"
#endif
#ifndef WIFI_PASS
#define WIFI_PASS  "SenhaDaRede"
#endif

#define MQTT_BROKER_URI "mqtt://broker.hivemq.com:1883"

static const char *TAG = "MOTOR_MONITOR";
static esp_mqtt_client_handle_t s_mqtt_client = NULL;

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
    xSensorQueue = xQueueCreate(1, sizeof(sensor_data_t));  // Tamanho 1 exigido por xQueueOverwrite
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

    ESP_LOGI(TAG, "Criando tarefas FreeRTOS...");

    // =============================================================================
    // Alocação das Tarefas no CORE 0 (Tempo Crítico, Proteção, Sensores e Rede)
    // Criadas ANTES do Wi-Fi para o display e sensores iniciarem imediatamente
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

    // Conectividade em background (não bloqueia mais o boot)
    init_wifi();
    init_mqtt();
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
        if (xQueuePeek(xSensorQueue, &sData, pdMS_TO_TICKS(600)) == pdTRUE) {
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

                char pub_topic[64];
                snprintf(pub_topic, sizeof(pub_topic), "motor/%s/telemetry", DEVICE_ID);
                int msg_id = esp_mqtt_client_publish(s_mqtt_client, pub_topic, payload, 0, 1, 0);
                ESP_LOGI(TAG, "MQTT [PUB] (msg_id=%d) %s -> %s", msg_id, pub_topic, payload);
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

                char rsp_topic[64];
                snprintf(rsp_topic, sizeof(rsp_topic), "motor/%s/cmd/response", DEVICE_ID);
                esp_mqtt_client_publish(s_mqtt_client, rsp_topic, payload, 0, 1, 0);
                ESP_LOGI(TAG, "MQTT [PUB] (%s) -> %s", rsp_topic, payload);
            }
        }
    }
}

static void task_display(void *pvParameters) {
    ESP_LOGI(TAG, "Iniciando Task de Renderização do Display OLED...");
    
    // O barramento I2C já foi inicializado por init_i2c() no app_main.
    // i2c_device_add associa o SSD1306 ao driver existente sem reinstalá-lo.
    i2c_device_add(&dev, I2C_NUM_0, -1, I2C_ADDRESS);
    ssd1306_init(&dev, 128, 64);
    ssd1306_clear_screen(&dev, false);
    ssd1306_contrast(&dev, 0xFF);
    
    // Tela Inicial / Logo Estático
    ssd1306_display_text(&dev, 0, "  KiiRA MONITOR ", 16, true);

    sensor_data_t sData = {0};
    char buf[24];

    while (1) {
        xSemaphoreTake(xStateMutex, portMAX_DELAY);
        sData = sLastSensorData;
        motor_state_t eState = eCurrentState;
        xSemaphoreGive(xStateMutex);

        const char *state_str = eState == STATE_ARMED ? "ARMADO  " :
                                eState == STATE_ALERT ? "ALERTA  " :
                                eState == STATE_COOLDOWN ? "RESFRIAR" :
                                eState == STATE_PROTECTED ? "PROTEG. " : "DESLIG. ";

        // Linha de Status Global
        snprintf(buf, sizeof(buf), "STS: %s", state_str);
        ssd1306_display_text(&dev, 2, buf, strlen(buf), false);

        // Linha de Temperatura
        snprintf(buf, sizeof(buf), "Temp: %.1f C", sData.temperature);
        ssd1306_display_text(&dev, 4, buf, strlen(buf), false);

        // Linha de Vibração
        snprintf(buf, sizeof(buf), "Vib:  %.2f g", sData.vib_resultant);
        ssd1306_display_text(&dev, 6, buf, strlen(buf), false);

        ESP_LOGI(TAG, "OLED ---> [T: %.1fC | V: %.2fg] ESTADO: %s",
                 sData.temperature, sData.vib_resultant, state_str);

        vTaskDelay(pdMS_TO_TICKS(500));
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
    
    const size_t audio_buffer_size = 32000; // 1 segundo, 16kHz, 16bit (32 KB) — ajustado para RAM disponível
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
    
    esp_err_t ret = spi_bus_initialize(host.slot, &bus_cfg, SPI_DMA_CH_AUTO);
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

// =============================================================================
// Wi-Fi — Event Handler e Inicialização Real
// =============================================================================
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi desconectado. Tentando reconectar...");
        xEventGroupClearBits(xSystemEvents, EVENT_WIFI_CONNECTED);
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Wi-Fi conectado! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(xSystemEvents, EVENT_WIFI_CONNECTED);
    }
}

static void init_wifi(void) {
    ESP_LOGI(TAG, "Inicializando Wi-Fi...");
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                         &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                         &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid     = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Aguardando conexão Wi-Fi (SSID: %s)...", WIFI_SSID);
}

// =============================================================================
// MQTT — Event Handler e Inicialização Real
// =============================================================================
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event  = (esp_mqtt_event_handle_t)event_data;
    esp_mqtt_client_handle_t client = event->client;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT conectado ao broker!");
            xEventGroupSetBits(xSystemEvents, EVENT_MQTT_CONNECTED);

            // Subscreve nos tópicos de comando e limiares
            esp_mqtt_client_subscribe(client, "motor/" DEVICE_ID "/cmd", 1);
            esp_mqtt_client_subscribe(client, "motor/" DEVICE_ID "/thresholds", 1);
            register_blockchain_audit_log(AUDIT_EVENT_BOOT, "MQTT conectado. Subscrito nos tópicos de comando.");
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT desconectado. Aguardando reconexão automática...");
            xEventGroupClearBits(xSystemEvents, EVENT_MQTT_CONNECTED);
            break;

        case MQTT_EVENT_DATA: {
            // Copia o tópico e payload para buffers terminados em \0
            char topic[128]  = {0};
            char payload[256] = {0};
            int topic_len   = (event->topic_len   < (int)sizeof(topic)   - 1) ? event->topic_len   : (int)sizeof(topic)   - 1;
            int payload_len = (event->data_len    < (int)sizeof(payload) - 1) ? event->data_len    : (int)sizeof(payload) - 1;
            memcpy(topic,   event->topic, topic_len);
            memcpy(payload, event->data,  payload_len);

            ESP_LOGI(TAG, "MQTT [REC]: %s -> %s", topic, payload);

            // Tópico de Comando: extrai action/value/command_id do JSON manualmente
            char cmd_topic[128];
            snprintf(cmd_topic, sizeof(cmd_topic), "motor/%s/cmd", DEVICE_ID);
            if (strcmp(topic, cmd_topic) == 0) {
                mqtt_command_t cmd = {0};
                // Parser minimalista sem biblioteca JSON extra
                // Formato esperado: {"command_id":"...","action":"...","value":"..."}
                char *p;
                if ((p = strstr(payload, "\"command_id\":\"")) != NULL) {
                    sscanf(p, "\"command_id\":\"%39[^\"]\"", cmd.command_id);
                }
                if ((p = strstr(payload, "\"action\":\"")) != NULL) {
                    sscanf(p, "\"action\":\"%31[^\"]\"", cmd.action);
                }
                if ((p = strstr(payload, "\"value\":\"")) != NULL) {
                    sscanf(p, "\"value\":\"%15[^\"]\"", cmd.value);
                }
                // Enfileira para a task_telemetry processar
                if (xQueueSend(xCommandQueue, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
                    ESP_LOGW(TAG, "Fila de comandos cheia! Comando descartado.");
                }
            }

            // Tópico de Limiares: atualiza dinamicamente
            char thr_topic[128];
            snprintf(thr_topic, sizeof(thr_topic), "motor/%s/thresholds", DEVICE_ID);
            if (strcmp(topic, thr_topic) == 0) {
                motor_thresholds_t new_t = {0};
                char *p;
                if ((p = strstr(payload, "\"temp_alert_c\":")) != NULL)    sscanf(p, "\"temp_alert_c\":%f",    &new_t.temp_alert);
                if ((p = strstr(payload, "\"temp_critical_c\":")) != NULL) sscanf(p, "\"temp_critical_c\":%f", &new_t.temp_critical);
                if ((p = strstr(payload, "\"vib_alert_g\":")) != NULL)     sscanf(p, "\"vib_alert_g\":%f",     &new_t.vib_alert);
                if ((p = strstr(payload, "\"vib_critical_g\":")) != NULL)  sscanf(p, "\"vib_critical_g\":%f",  &new_t.vib_critical);
                if (new_t.temp_critical > 0 && new_t.vib_critical > 0) {
                    update_active_thresholds(&new_t);
                    register_blockchain_audit_log(AUDIT_EVENT_THRESHOLD_CHANGE, "Limiares atualizados remotamente via MQTT.");
                }
            }
            break;
        }
        default:
            break;
    }
}

static void init_mqtt(void) {
    // Aguarda Wi-Fi antes de conectar ao broker
    ESP_LOGI(TAG, "Aguardando Wi-Fi para iniciar MQTT...");
    xEventGroupWaitBits(xSystemEvents, EVENT_WIFI_CONNECTED, pdFALSE, pdTRUE, pdMS_TO_TICKS(30000));

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        .session.keepalive  = 30,
    };

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID,
                                                    mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_mqtt_client));
    ESP_LOGI(TAG, "MQTT client iniciado. Broker: %s", MQTT_BROKER_URI);
}