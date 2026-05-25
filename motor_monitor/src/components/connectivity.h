#pragma once
#include "alarm.h"
#include "sensors.h"
#include "mqtt_client.h"
#include "esp_err.h"

esp_err_t connectivity_init(void);
esp_mqtt_client_handle_t connectivity_get_client(void);
esp_err_t connectivity_publish(const sensor_data_t *data, float rms, alarm_level_t level);
