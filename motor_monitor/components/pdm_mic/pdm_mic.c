#include "pdm_mic.h"
#include "driver/i2s_pdm.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h> // Necessário para fopen, fwrite, fclose

// Tamanho do bloco de leitura I2S em bytes
#define PDM_CHUNK_SIZE  4096

static const char *TAG = "pdm_mic";
static i2s_chan_handle_t s_rx = NULL;

esp_err_t pdm_mic_init(void)
{
    i2s_chan_config_t ch_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&ch_cfg, NULL, &s_rx));

    i2s_pdm_rx_config_t pdm_cfg = {
        .clk_cfg  = I2S_PDM_RX_CLK_DEFAULT_CONFIG(PDM_SAMPLE_RATE),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                    I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk  = PDM_CLK_PIN,
            .din  = PDM_DATA_PIN,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_pdm_rx_mode(s_rx, &pdm_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx));
    ESP_LOGI(TAG, "PDM inicializado (%d Hz)", PDM_SAMPLE_RATE);
    return ESP_OK;
}

// Função auxiliar para escrever o cabeçalho WAV de 44 bytes
static void write_wav_header(FILE *f, uint32_t total_audio_bytes)
{
    uint32_t total_data_len = total_audio_bytes + 36;
    uint32_t bitrate = PDM_SAMPLE_RATE * 1 * 2; // SampleRate * Channels * (BitsPerSample/8)
    
    uint8_t header[44] = {
        'R', 'I', 'F', 'F',
        (uint8_t)(total_data_len & 0xFF), (uint8_t)((total_data_len >> 8) & 0xFF),
        (uint8_t)((total_data_len >> 16) & 0xFF), (uint8_t)((total_data_len >> 24) & 0xFF),
        'W', 'A', 'V', 'E',
        'f', 'm', 't', ' ',
        16, 0, 0, 0,    // Tamanho do bloco fmt (16)
        1, 0,           // Formato de áudio (1 = PCM não comprimido)
        1, 0,           // Número de canais (1 = Mono)
        (uint8_t)(PDM_SAMPLE_RATE & 0xFF), (uint8_t)((PDM_SAMPLE_RATE >> 8) & 0xFF),
        (uint8_t)((PDM_SAMPLE_RATE >> 16) & 0xFF), (uint8_t)((PDM_SAMPLE_RATE >> 24) & 0xFF),
        (uint8_t)(bitrate & 0xFF), (uint8_t)((bitrate >> 8) & 0xFF),
        (uint8_t)((bitrate >> 16) & 0xFF), (uint8_t)((bitrate >> 24) & 0xFF),
        2, 0,           // Bloco de alinhamento (Channels * BitsPerSample/8)
        16, 0,          // Bits por amostra (16 bits)
        'd', 'a', 't', 'a',
        (uint8_t)(total_audio_bytes & 0xFF), (uint8_t)((total_audio_bytes >> 8) & 0xFF),
        (uint8_t)((total_audio_bytes >> 16) & 0xFF), (uint8_t)((total_audio_bytes >> 24) & 0xFF)
    };

    // Escreve os 44 bytes no início do arquivo
    size_t written = fwrite(header, 1, 44, f);
    if (written != 44) {
        ESP_LOGE(TAG, "Erro ao escrever cabecalho WAV (%u/44 bytes)!", (unsigned)written);
    }
}

esp_err_t pdm_mic_record(int16_t *buf, size_t buf_bytes, size_t *bytes_written)
{
    *bytes_written = 0;
    size_t total = 0;
    uint8_t *ptr = (uint8_t *)buf;

    while (total < buf_bytes) {
        size_t chunk = 4096;
        if (total + chunk > buf_bytes) chunk = buf_bytes - total;
        size_t read = 0;
        esp_err_t r = i2s_channel_read(s_rx, ptr + total, chunk, &read, pdMS_TO_TICKS(500));
        if (r != ESP_OK) return r;
        total += read;
    }
    *bytes_written = total;
    return ESP_OK;
}

esp_err_t pdm_mic_record_to_sd(const char *filepath, uint32_t duration_s)
{
    if (!s_rx) return ESP_ERR_INVALID_STATE;

    FILE *f = fopen(filepath, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Falha ao criar o arquivo WAV no SD: %s", filepath);
        return ESP_FAIL;
    }

    // 1. Escreve um cabeçalho WAV temporário (vazio) para ocupar os primeiros 44 bytes
    write_wav_header(f, 0);

    size_t total_bytes_to_write = PDM_SAMPLE_RATE * sizeof(int16_t) * duration_s;
    size_t bytes_written_to_sd = 0;

    uint8_t *temp_buf = (uint8_t *)malloc(PDM_CHUNK_SIZE);
    if (temp_buf == NULL) {
        ESP_LOGE(TAG, "Falha ao alocar buffer temporario na Heap!");
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Gravando WAV no SD: %s (%d segundos)...", filepath, (int)duration_s);

    while (bytes_written_to_sd < total_bytes_to_write) {
        size_t bytes_to_read = PDM_CHUNK_SIZE;
        if (bytes_written_to_sd + bytes_to_read > total_bytes_to_write) {
            bytes_to_read = total_bytes_to_write - bytes_written_to_sd;
        }

        size_t bytes_read_i2s = 0;
        esp_err_t r = i2s_channel_read(s_rx, temp_buf, bytes_to_read, &bytes_read_i2s, pdMS_TO_TICKS(500));
        if (r != ESP_OK) {
            ESP_LOGE(TAG, "Erro ao ler dados do I2S: %s", esp_err_to_name(r));
            free(temp_buf);
            fclose(f);
            return r;
        }

        if (bytes_read_i2s > 0) {
            size_t written = fwrite(temp_buf, 1, bytes_read_i2s, f);
            if (written != bytes_read_i2s) {
                ESP_LOGE(TAG, "Erro de escrita no SD!");
                free(temp_buf);
                fclose(f);
                return ESP_FAIL;
            }
            bytes_written_to_sd += written;
        }
    }

    // 2. Volta ao início do arquivo (Byte 0) e reescreve o cabeçalho com os tamanhos corretos
    fseek(f, 0, SEEK_SET);
    write_wav_header(f, bytes_written_to_sd);

    free(temp_buf);
    fclose(f);
    
    ESP_LOGI(TAG, "Arquivo WAV salvo! Total de áudio: %u bytes.", (unsigned)bytes_written_to_sd);
    return ESP_OK;
}

void pdm_mic_deinit(void)
{
    if (s_rx) {
        i2s_channel_disable(s_rx);
        i2s_del_channel(s_rx);
        s_rx = NULL;
    }
}
