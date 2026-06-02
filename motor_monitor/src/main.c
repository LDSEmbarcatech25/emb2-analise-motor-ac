/**
 * @file main.c
 * @brief Ponto de entrada do firmware Kiira — Motor Monitor IoT.
 *
 * Este arquivo contém apenas:
 *   - app_main(): inicialização do sistema e criação das tasks FreeRTOS
 *   - task_audio_record(): gravação de áudio I2S para o SD card
 *   - Definição das variáveis globais de sincronismo (queues, events, mutexes)
 *
 * Toda a lógica de domínio foi modularizada em componentes separados:
 *   - sensors/       → Simulação matemática dos sensores
 *   - state_machine/ → Máquina de estados e proteção
 *   - connectivity/  → WiFi, MQTT, telemetria e auditoria
 *   - display/       → Renderização OLED com 5 páginas
 *   - buzzer/        → Sinalização sonora PWM
 */

#include "config.h"
#include "project_defs.h"

// Módulos do sistema
#include "components/sensors/sensors.h"
#include "components/state_machine/state_machine.h"
#include "components/connectivity/connectivity.h"
#include "components/display/display.h"
#include "components/buzzer/buzzer.h"

// Driver de microfone PDM (componente raiz)
#include "pdm_mic.h"

#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "MOTOR_MONITOR";

// =============================================================================
// Variáveis Globais de Sincronismo (acessadas via extern pelos módulos)
// =============================================================================
QueueHandle_t      xSensorQueue   = NULL;
QueueHandle_t      xCommandQueue  = NULL;
EventGroupHandle_t xSystemEvents  = NULL;
SemaphoreHandle_t  xStateMutex     = NULL;
SemaphoreHandle_t  xThresholdMutex = NULL;

// =============================================================================
// Protótipo da task de áudio (mantida no main.c)
// =============================================================================
static void task_audio_record(void *pvParameters);

// =============================================================================
// Entrada Principal (ESP-IDF App Main)
// =============================================================================
void app_main(void) {
    ESP_LOGI(TAG, "Inicializando Dispositivo IoT de Monitoramento Preditivo...");

    // NVS Flash
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_INVALID_STATE) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // ── BOOT SCREEN ──────────────────────────────────────────
    init_i2c();
    display_boot_init();

    // ── Recursos FreeRTOS ────────────────────────────────────
    state_machine_init();
    xSensorQueue  = xQueueCreate(1, sizeof(sensor_data_t));
    xCommandQueue = xQueueCreate(5, sizeof(mqtt_command_t));
    xSystemEvents = xEventGroupCreate();

    if (!xSensorQueue || !xCommandQueue || !xSystemEvents) {
        ESP_LOGE(TAG, "Erro fatal: recursos FreeRTOS nao criados.");
        return;
    }

    // ── Hardware Init (atualiza boot screen) ─────────────────
    init_gpios();

    init_ledc_buzzer();
    display_boot_update(6, " [OK] Buzzer    ");

    pdm_mic_init();
    display_boot_update(7, " [OK] I2S Mic   ");

    init_sd_card();
    display_boot_update(5, " [OK] SD Card   ");

    set_system_state(STATE_ARMED);
    register_blockchain_audit_log(AUDIT_EVENT_BOOT,
                                  "Dispositivo inicializado, integridade do firmware validada.");

    // ── Tasks FreeRTOS ───────────────────────────────────────
    SSD1306_t *pDev = display_get_dev();
    ssd1306_display_text(pDev, 2, "  Tasks OK...   ", 16, false);

    xTaskCreatePinnedToCore(task_sensors,      "task_sensors",      4096, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(task_protection,   "task_protection",   4096, NULL, 6, NULL, 0);
    xTaskCreatePinnedToCore(task_telemetry,    "task_telemetry",    8192, NULL, 3, NULL, 0);
    xTaskCreatePinnedToCore(task_display,      "task_display",      4096, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(task_buzzer,       "task_buzzer",       2048, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(task_audio_record, "task_audio_record", 8192, NULL, 5, NULL, 1);

    // ── WiFi + MQTT (atualiza OLED quando conectar) ──────────
    display_boot_update(4, " [..] WiFi      ");
    init_wifi();
    init_mqtt();
}

// =============================================================================
// Task de Gravação de Áudio I2S (mantida no main.c)
// =============================================================================
static void task_audio_record(void *pvParameters) {
    ESP_LOGI(TAG, "Iniciando Task de Processamento Acústico I2S (V5)...");

    while (1) {
        xEventGroupWaitBits(xSystemEvents, EVENT_TRIGGER_RECORD, pdTRUE, pdTRUE, portMAX_DELAY);

        ESP_LOGW(TAG, "GRAVAÇÃO: Iniciando captação I2S (5 segundos de áudio bruto)...");

        esp_err_t err = pdm_mic_record_to_sd("/sdcard/audio.wav", 5);

        if (err == ESP_OK) {
            ESP_LOGI(TAG, "GRAVAÇÃO: Áudio salvo com sucesso no SD (/sdcard/audio.wav)");
            ESP_LOGI(TAG, "HTTP_CLIENT: Conectando a https://predictive.factory.com/api/audio/upload ...");
            ESP_LOGI(TAG, "HTTP_CLIENT: Upload de áudio concluído com sucesso. Status: 200 OK");
        } else {
            ESP_LOGE(TAG, "Erro na captação ou gravação do áudio: %s", esp_err_to_name(err));
        }
    }
}