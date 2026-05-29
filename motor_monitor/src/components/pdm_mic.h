#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

// PDM via I2S0 — GPIO 2 (CLK) / GPIO 37 (DATA)
#define PDM_CLK_PIN      2
#define PDM_DATA_PIN     37
#define PDM_SAMPLE_RATE  16000

// Configuração do novo tempo estendido para o SD
#define PDM_SD_DURATION_S 30  // Altere aqui para o tempo que quiser (ex: 30s, 60s)

esp_err_t pdm_mic_init(void);
esp_err_t pdm_mic_record(int16_t *buf, size_t buf_bytes, size_t *bytes_written);

// Nova função para gravação direta no Cartão SD
esp_err_t pdm_mic_record_to_sd(const char *filepath, uint32_t duration_s);

void      pdm_mic_deinit(void);
