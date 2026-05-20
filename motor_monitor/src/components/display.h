#pragma once
#include "alarm.h"
#include "sensors.h"
#include "esp_err.h"

// Mesmo barramento I2C dos sensores (GPIO 14/21)
// Endereço padrão SSD1306: 0x3C
#define OLED_ADDR    0x3C
#define OLED_WIDTH   128
#define OLED_HEIGHT  64

esp_err_t display_init(void);
void display_update(const sensor_data_t *data, float rms, alarm_level_t level);
