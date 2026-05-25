#pragma once
#include "esp_err.h"

// Pinagem I2C (compartilhada entre HDC1080 e MPU6050)
#define I2C_PORT     I2C_NUM_0
#define I2C_SDA      14
#define I2C_SCL      21
#define I2C_FREQ_HZ  400000

// Endereços
#define HDC1080_ADDR 0x40
#define MPU6050_ADDR 0x68  // AD0=GND; use 0x69 se AD0=VCC

typedef struct {
    float temperature_c;
    float humidity_pct;
} hdc1080_data_t;

typedef struct {
    float ax, ay, az;  // aceleração em g
    float gx, gy, gz;  // giroscópio em °/s
} mpu6050_data_t;

typedef struct {
    hdc1080_data_t env;
    mpu6050_data_t imu;
} sensor_data_t;

esp_err_t sensors_init(void);
esp_err_t sensors_read_all(sensor_data_t *out);
