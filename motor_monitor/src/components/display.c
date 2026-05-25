#include "display.h"
#include "sensors.h"
#include "esp_log.h"
#include "driver/i2c.h"
#include <string.h>
#include <stdio.h>

// Usa o mesmo I2C_PORT já inicializado por sensors_init()
// Inclua a lib: idf_component_manager add espressif/ssd1306
#include "ssd1306.h"
#include "font8x8_basic.h"

static const char *TAG = "display";
static SSD1306_t s_dev;

// ─── Init ─────────────────────────────────────────────────────────────────────

esp_err_t display_init(void)
{
    // I2C já inicializado em sensors_init(); passa o handle existente
    i2c_master_init(&s_dev, 14, 21, -1);   // SDA, SCL, reset=-1
    ssd1306_init(&s_dev, OLED_WIDTH, OLED_HEIGHT);
    ssd1306_clear_screen(&s_dev, false);
    ssd1306_contrast(&s_dev, 0xFF);
    ESP_LOGI(TAG, "OLED SSD1306 inicializado");
    return ESP_OK;
}

// ─── Update ───────────────────────────────────────────────────────────────────

void display_update(const sensor_data_t *data, float rms, alarm_level_t level)
{
    char line[22];
    ssd1306_clear_screen(&s_dev, false);

    // Linha 0: título
    ssd1306_display_text(&s_dev, 0, "Motor Monitor", 13, false);

    // Linha 2: temperatura e umidade
    snprintf(line, sizeof(line), "T:%.1fC  H:%.0f%%",
             data->env.temperature_c, data->env.humidity_pct);
    ssd1306_display_text(&s_dev, 2, line, strlen(line), false);

    // Linha 4: vibração RMS
    snprintf(line, sizeof(line), "RMS: %.4f g", rms);
    ssd1306_display_text(&s_dev, 4, line, strlen(line), false);

    // Linha 6: estado do alarme (invertido se CONFIRMED)
    bool invert = (level == ALARM_CONFIRMED);
    snprintf(line, sizeof(line), ">> %s <<", alarm_level_str(level));
    ssd1306_display_text(&s_dev, 6, line, strlen(line), invert);
}
