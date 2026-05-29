#pragma once
#include <stdbool.h>
#include "sensors.h"

// Parâmetros de calibração (ajuste conforme o motor)
#define ALARM_LEARN_SAMPLES     500      // amostras para calibração (~5s a 100Hz)
#define ALARM_OUTLIER_PCT       0.10f    // remove 10% extremos
#define ALARM_THRESHOLD_SIGMA   3.0f     // threshold = média + 3σ
#define ALARM_SAMPLE_MS         10       // intervalo entre amostras (100 Hz)

typedef enum {
    ALARM_NONE = 0,
    ALARM_PROBABLE,    // RMS > threshold
    ALARM_CONFIRMED,   // RMS > threshold * 1.5
} alarm_level_t;

typedef struct {
    float mean;
    float std_dev;
    float threshold;
    bool  calibrated;
} alarm_signature_t;

// Calcula RMS da aceleração 3 eixos (em g)
float alarm_rms(const mpu6050_data_t *imu);

// Aprende a assinatura normal da máquina (bloqueia ~5s)
esp_err_t alarm_calibrate(alarm_signature_t *sig);

// Verifica nível de alarme dado o RMS atual
alarm_level_t alarm_detect(const alarm_signature_t *sig, float rms);

const char *alarm_level_str(alarm_level_t level);
