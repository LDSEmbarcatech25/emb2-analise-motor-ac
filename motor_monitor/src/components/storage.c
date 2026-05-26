#include "storage.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_timer.h"
#include <stdio.h>

static const char *TAG = "storage";
static sdmmc_card_t *s_card = NULL;

// ─── Inicialização ────────────────────────────────────────────────────────────

esp_err_t storage_init(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = 25000;  // 25 MHz

    // 1-bit mode: apenas D0
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk   = SD_CLK;
    slot.cmd   = SD_CMD;
    slot.d0    = SD_D0;
    slot.width = 1;             // 1-bit SDMMC
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files              = 4,
        .allocation_unit_size   = 16 * 1024,
    };

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &mount, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao montar SD: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "SD montado em %s", SD_MOUNT_POINT);

    // Cria cabeçalho CSV se o arquivo não existir
    FILE *f = fopen(SD_LOG_FILE, "r");
    if (!f) {
        f = fopen(SD_LOG_FILE, "w");
        if (f) {
            fprintf(f, "timestamp_us,temp_c,hum_pct,ax,ay,az,rms,alarm\n");
            fclose(f);
        }
    } else {
        fclose(f);
    }

    return ESP_OK;
}

// ─── Log CSV ──────────────────────────────────────────────────────────────────

esp_err_t storage_log(const sensor_data_t *data, float rms, alarm_level_t level)
{
    if (!s_card) return ESP_ERR_INVALID_STATE;

    FILE *f = fopen(SD_LOG_FILE, "a");
    if (!f) {
        ESP_LOGE(TAG, "Erro ao abrir arquivo de log");
        return ESP_FAIL;
    }

    fprintf(f, "%lld,%.2f,%.1f,%.4f,%.4f,%.4f,%.4f,%s\n",
            esp_timer_get_time(),           // microssegundos desde boot
            data->env.temperature_c,
            data->env.humidity_pct,
            data->imu.ax, data->imu.ay, data->imu.az,
            rms,
            alarm_level_str(level));

    fclose(f);
    return ESP_OK;
}
