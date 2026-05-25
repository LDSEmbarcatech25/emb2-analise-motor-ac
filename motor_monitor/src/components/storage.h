#pragma once
#include "alarm.h"
#include "sensors.h"
#include "esp_err.h"

// Pinagem SDMMC 1-bit
#define SD_CLK  39
#define SD_CMD  38
#define SD_D0   40

#define SD_MOUNT_POINT  "/sdcard"
#define SD_LOG_FILE     "/sdcard/motor_log.csv"

esp_err_t storage_init(void);
esp_err_t storage_log(const sensor_data_t *data, float rms, alarm_level_t level);
