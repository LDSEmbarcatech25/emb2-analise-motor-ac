#include "pdm_mic.h"
#include "driver/i2s_pdm.h"
#include "esp_log.h"
#include <string.h>

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

esp_err_t pdm_mic_record(int16_t *buf, size_t buf_bytes, size_t *bytes_written)
{
    *bytes_written = 0;
    size_t total = 0;
    uint8_t *ptr = (uint8_t *)buf;

    // Lê em blocos de 4 KB até completar buf_bytes (~160 KB = 5 s)
    while (total < buf_bytes) {
        size_t chunk = 4096;
        if (total + chunk > buf_bytes) chunk = buf_bytes - total;
        size_t read = 0;
        esp_err_t r = i2s_channel_read(s_rx, ptr + total, chunk, &read, pdMS_TO_TICKS(500));
        if (r != ESP_OK) return r;
        total += read;
    }
    *bytes_written = total;
    ESP_LOGI(TAG, "Gravados %u bytes (%.1f s)", (unsigned)total,
             total / (float)(PDM_SAMPLE_RATE * sizeof(int16_t)));
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
