#include "alarm.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "alarm";

// ─── RMS dos 3 eixos ──────────────────────────────────────────────────────────

float alarm_rms(const mpu6050_data_t *imu)
{
    return sqrtf(imu->ax * imu->ax +
                 imu->ay * imu->ay +
                 imu->az * imu->az);
}

// ─── Comparador para qsort ────────────────────────────────────────────────────

static int cmp_float(const void *a, const void *b)
{
    float fa = *(float *)a, fb = *(float *)b;
    return (fa > fb) - (fa < fb);
}

// ─── Calibração ───────────────────────────────────────────────────────────────

esp_err_t alarm_calibrate(alarm_signature_t *sig)
{
    float *samples = malloc(ALARM_LEARN_SAMPLES * sizeof(float));
    if (!samples) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "Calibrando... (%d amostras)", ALARM_LEARN_SAMPLES);

    sensor_data_t data;
    for (int i = 0; i < ALARM_LEARN_SAMPLES; i++) {
        if (sensors_read_all(&data) == ESP_OK)
            samples[i] = alarm_rms(&data.imu);
        else
            samples[i] = 0.0f;
        vTaskDelay(pdMS_TO_TICKS(ALARM_SAMPLE_MS));
    }

    // Ordena e remove outliers (10% de cada extremo)
    qsort(samples, ALARM_LEARN_SAMPLES, sizeof(float), cmp_float);
    int start = (int)(ALARM_LEARN_SAMPLES * ALARM_OUTLIER_PCT);
    int end   = (int)(ALARM_LEARN_SAMPLES * (1.0f - ALARM_OUTLIER_PCT));
    int count = end - start;

    // Média
    double sum = 0;
    for (int i = start; i < end; i++) sum += samples[i];
    sig->mean = (float)(sum / count);

    // Desvio padrão
    double var = 0;
    for (int i = start; i < end; i++) {
        float d = samples[i] - sig->mean;
        var += d * d;
    }
    sig->std_dev  = sqrtf((float)(var / count));
    sig->threshold = sig->mean + ALARM_THRESHOLD_SIGMA * sig->std_dev;
    sig->calibrated = true;

    ESP_LOGI(TAG, "Calibrado: media=%.4fg  std=%.4fg  threshold=%.4fg",
             sig->mean, sig->std_dev, sig->threshold);

    free(samples);
    return ESP_OK;
}

// ─── Detecção ─────────────────────────────────────────────────────────────────

alarm_level_t alarm_detect(const alarm_signature_t *sig, float rms)
{
    if (!sig->calibrated) return ALARM_NONE;
    if (rms >= sig->threshold * 1.5f)  return ALARM_CONFIRMED;
    if (rms >= sig->threshold)          return ALARM_PROBABLE;
    return ALARM_NONE;
}

const char *alarm_level_str(alarm_level_t level)
{
    switch (level) {
        case ALARM_PROBABLE:  return "PROBABLE";
        case ALARM_CONFIRMED: return "CONFIRMED";
        default:              return "NONE";
    }
}
