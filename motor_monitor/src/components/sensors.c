#include "sensors.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sensors";

// ─── Helpers I2C ─────────────────────────────────────────────────────────────

static esp_err_t i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_write_to_device(I2C_PORT, addr, buf, 2, pdMS_TO_TICKS(100));
}

static esp_err_t i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_write_read_device(I2C_PORT, addr, &reg, 1, data, len, pdMS_TO_TICKS(100));
}

// ─── Inicialização ────────────────────────────────────────────────────────────

esp_err_t sensors_init(void)
{
    // Configura barramento I2C
    i2c_config_t cfg = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = I2C_SDA,
        .scl_io_num       = I2C_SCL,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_PORT, &cfg));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0));

    // HDC1080 — configuração: aquisição separada, 14-bit temp + 14-bit hum
    // Registro 0x02: bits[13:12]=00 (14-bit), bit[8]=0 (aquisição separada)
    uint8_t hdc_cfg[3] = {0x02, 0x00, 0x00};
    ESP_ERROR_CHECK(i2c_master_write_to_device(I2C_PORT, HDC1080_ADDR,
                                               hdc_cfg, 3, pdMS_TO_TICKS(100)));

    // MPU6050 — sai do modo sleep
    ESP_ERROR_CHECK(i2c_write_reg(MPU6050_ADDR, 0x6B, 0x00));
    // Fundo de escala acelerômetro: ±2g (0x1C = 0x00), giroscópio: ±250°/s (0x1B = 0x00)
    ESP_ERROR_CHECK(i2c_write_reg(MPU6050_ADDR, 0x1C, 0x00));
    ESP_ERROR_CHECK(i2c_write_reg(MPU6050_ADDR, 0x1B, 0x00));

    ESP_LOGI(TAG, "HDC1080 e MPU6050 inicializados (SDA=%d SCL=%d)", I2C_SDA, I2C_SCL);
    return ESP_OK;
}

// ─── HDC1080 ─────────────────────────────────────────────────────────────────

static esp_err_t hdc1080_read(hdc1080_data_t *out)
{
    uint8_t raw[2];

    // Temperatura
    uint8_t reg_t = 0x00;
    ESP_ERROR_CHECK(i2c_master_write_to_device(I2C_PORT, HDC1080_ADDR,
                                               &reg_t, 1, pdMS_TO_TICKS(100)));
    vTaskDelay(pdMS_TO_TICKS(10));  // conversão ~6,35 ms a 14-bit
    ESP_ERROR_CHECK(i2c_master_read_from_device(I2C_PORT, HDC1080_ADDR,
                                                raw, 2, pdMS_TO_TICKS(100)));
    out->temperature_c = ((raw[0] << 8 | raw[1]) / 65536.0f) * 165.0f - 40.0f;

    // Umidade
    uint8_t reg_h = 0x01;
    ESP_ERROR_CHECK(i2c_master_write_to_device(I2C_PORT, HDC1080_ADDR,
                                               &reg_h, 1, pdMS_TO_TICKS(100)));
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_ERROR_CHECK(i2c_master_read_from_device(I2C_PORT, HDC1080_ADDR,
                                                raw, 2, pdMS_TO_TICKS(100)));
    out->humidity_pct = ((raw[0] << 8 | raw[1]) / 65536.0f) * 100.0f;

    return ESP_OK;
}

// ─── MPU6050 ─────────────────────────────────────────────────────────────────

// static esp_err_t mpu6050_read(mpu6050_data_t *out)
// {
//     uint8_t raw[14];
//     // Lê 14 bytes a partir de 0x3B: ACCEL XYZ + TEMP + GYRO XYZ
//     ESP_ERROR_CHECK(i2c_read_reg(MPU6050_ADDR, 0x3B, raw, 14));

//     // Aceleração: sensibilidade 16384 LSB/g (±2g)
//     out->ax = (int16_t)(raw[0]  << 8 | raw[1])  / 16384.0f;
//     out->ay = (int16_t)(raw[2]  << 8 | raw[3])  / 16384.0f;
//     out->az = (int16_t)(raw[4]  << 8 | raw[5])  / 16384.0f;

//     // raw[6..7] = temperatura interna (ignorada aqui, usamos HDC1080)

//     // Giroscópio: sensibilidade 131 LSB/(°/s) (±250°/s)
//     out->gx = (int16_t)(raw[8]  << 8 | raw[9])  / 131.0f;
//     out->gy = (int16_t)(raw[10] << 8 | raw[11]) / 131.0f;
//     out->gz = (int16_t)(raw[12] << 8 | raw[13]) / 131.0f;

//     return ESP_OK;
// }

static esp_err_t mpu6050_read(mpu6050_data_t *out)
{
    uint8_t raw[14];
    // Lê 14 bytes a partir de 0x3B: ACCEL_XYZ (6b) + TEMP (2b) + GYRO_XYZ (6b)
    ESP_ERROR_CHECK(i2c_read_reg(MPU6050_ADDR, 0x3B, raw, 14));

    // ─── Aceleração (Sensibilidade: ±2g -> 16384 LSB/g) ─────────────────────
    out->ax = (int16_t)(raw[0] << 8 | raw[1]) / 16384.0f;
    out->ay = (int16_t)(raw[2] << 8 | raw[3]) / 16384.0f;
    out->az = (int16_t)(raw[4] << 8 | raw[5]) / 16384.0f;

    // ─── Temperatura Interna (Fórmula oficial do Datasheet) ──────────────────
    int16_t raw_temp = (int16_t)(raw[6] << 8 | raw[7]);
    out->temperature_c = (raw_temp / 340.0f) + 36.53f;

    // ─── Giroscópio / Rotação (Sensibilidade: ±250°/s -> 131 LSB/°/s) ────────
    out->gx = (int16_t)(raw[8]  << 8 | raw[9])  / 131.0f;
    out->gy = (int16_t)(raw[10] << 8 | raw[11]) / 131.0f;
    out->gz = (int16_t)(raw[12] << 8 | raw[13]) / 131.0f;

    return ESP_OK;
}

// ─── Leitura unificada ────────────────────────────────────────────────────────

esp_err_t sensors_read_all(sensor_data_t *out)
{
    esp_err_t r = ESP_OK;
    r |= hdc1080_read(&out->env);
    r |= mpu6050_read(&out->imu);
    return r;
}

// ─── Leitura unificada (Modificada para Teste Isolado) ────────────────────────

// esp_err_t sensors_read_all(sensor_data_t *out)
// {
//     // Preenche dados zerados fictícios para o HDC no struct, evitando lixo de memória
//     out->env.temperature_c = 0.0f;
//     out->env.humidity_pct = 0.0f;

//     // Chama APENAS o MPU6050 que está fisicamente conectado
//     return mpu6050_read(&out->imu);
// }