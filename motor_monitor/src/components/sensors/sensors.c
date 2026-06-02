/**
 * @file sensors.c
 * @brief Simulação matemática dos sensores industriais do motor AC.
 *
 * Este módulo mantém toda a simulação de sensores (temperatura, umidade,
 * vibração triaxial) usando funções matemáticas. Nenhum hardware real é
 * acessado — a integração com sensores físicos será feita em fase posterior.
 */

#include "sensors.h"
#include "project_defs.h"
#include "config.h"
#include "esp_log.h"
#include <math.h>
#include <stdlib.h>

static const char *TAG = "SENSORS";

// Variáveis internas do módulo
static sensor_data_t sLastSensorData = {0};
static bool bInjectAnomaly = false;

// API Pública
// =============================================================================

void sensors_get_last_data(sensor_data_t *pData) {
    xSemaphoreTake(xStateMutex, portMAX_DELAY);
    *pData = sLastSensorData;
    xSemaphoreGive(xStateMutex);
}

void sensors_inject_anomaly(bool bEnable) {
    bInjectAnomaly = bEnable;
}

bool sensors_get_inject_anomaly(void) {
    return bInjectAnomaly;
}

// =============================================================================
// Task FreeRTOS
// =============================================================================

void task_sensors(void *pvParameters) {
    ESP_LOGI(TAG, "Iniciando Task de Leitura de Sensores...");
    sensor_data_t sData = {0};
    float t_sim = 0.0f;
    float fAnomalyAccumulator = 0.0f;

    while (1) {
        t_sim += 0.1f;

        // Simulação matemática rica das medições industriais
        sData.temperature =
            45.0f + (sinf(t_sim * 0.05f) * 10.0f) + ((float)(rand() % 10) / 10.0f);
        sData.humidity = 60.0f + (cosf(t_sim * 0.02f) * 5.0f);

        // Aceleração RMS harmônica
        sData.accel_x_rms =
            0.50f + (sinf(t_sim * 2.0f) * 0.10f) + ((float)(rand() % 20) / 100.0f);
        sData.accel_y_rms =
            0.40f + (cosf(t_sim * 2.0f) * 0.08f) + ((float)(rand() % 15) / 100.0f);
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
        sData.vib_resultant = sqrtf((sData.accel_x_rms * sData.accel_x_rms) +
                                    (sData.accel_y_rms * sData.accel_y_rms) +
                                    (sData.accel_z_rms * sData.accel_z_rms));

        // Injeção de Anomalia Progressiva (Para testes end-to-end de estresse)
        if (bInjectAnomaly) {
            fAnomalyAccumulator += 0.25f; // Sobe 0.25g a cada ciclo (500ms)

            sData.vib_resultant += fAnomalyAccumulator;
            sData.temperature +=
                (fAnomalyAccumulator * 4.0f); // Aquece por fricção mecânica severa

            if (sData.vib_resultant > 6.0f)
                sData.vib_resultant = 6.0f;
            if (sData.temperature > 95.0f)
                sData.temperature = 95.0f;

            ESP_LOGW(TAG, "ANOMALIA INJETADA: Vibração: %.2fg | Temperatura: %.1fC",
                     sData.vib_resultant, sData.temperature);
        } else {
            // Limpa o acumulador se o teste for desligado
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
