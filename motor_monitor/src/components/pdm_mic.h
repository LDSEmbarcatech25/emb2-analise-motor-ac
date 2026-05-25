#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

// PDM via I2S0 — GPIO 2 (CLK) / GPIO 37 (DATA)
#define PDM_CLK_PIN      2
#define PDM_DATA_PIN     37
#define PDM_SAMPLE_RATE  16000
#define PDM_DURATION_S   5
#define PDM_BUF_SIZE     (PDM_SAMPLE_RATE * PDM_DURATION_S * sizeof(int16_t))  // ~160 KB

esp_err_t pdm_mic_init(void);
esp_err_t pdm_mic_record(int16_t *buf, size_t buf_bytes, size_t *bytes_written);
void      pdm_mic_deinit(void);
