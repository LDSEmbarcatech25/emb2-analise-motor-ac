#pragma once

// ── Identidade do dispositivo ─────────────────────────────────────────────────
#define DEVICE_ID           "motor_01"

// ── Wi-Fi ─────────────────────────────────────────────────────────────────────
#define WIFI_SSID           "SEU_SSID"
#define WIFI_PASS           "SUA_SENHA"

// ── MQTT (HiveMQ Cloud) ───────────────────────────────────────────────────────
#define MQTT_BROKER         "mqtts://SEU_CLUSTER.hivemq.cloud"
#define MQTT_PORT           8883
#define MQTT_USER           "SEU_USUARIO"
#define MQTT_PASS           "SUA_SENHA_MQTT"

#define TOPIC_TELEMETRY     "motor/" DEVICE_ID "/telemetry"
#define TOPIC_STATUS        "motor/" DEVICE_ID "/status"
#define TOPIC_CMD           "motor/" DEVICE_ID "/cmd"

// ── Servidor de áudio (HTTP POST) ─────────────────────────────────────────────
#define AUDIO_SERVER_URL    "http://SEU_SERVIDOR/audio/upload"
#define AUDIO_DURATION_S    5
#define AUDIO_SAMPLE_RATE   16000

// ── Pinagem ───────────────────────────────────────────────────────────────────
#define I2C_PORT            I2C_NUM_0
#define I2C_SDA             14
#define I2C_SCL             21
#define I2C_FREQ_HZ         400000

#define MPU6050_ADDR        0x68
#define OLED_ADDR           0x3C

#define DHT22_GPIO          36
#define PDM_CLK_GPIO        2
#define PDM_DATA_GPIO       37
#define BUZZER_GPIO         3

#define RELAY_1_BIO_GPIO  47  // Bloqueio de Segurança (NC) e Desligamento remoto
#define RELAY_2_CMD_GPIO  48  // Comando de Pulso para Ligar (NO)

#define BTN_RESET_GPIO      45
#define BTN_SILENCE_GPIO    46

// ── Limiares de proteção ──────────────────────────────────────────────────────
#define THRESH_TEMP_ALERT   70.0f
#define THRESH_TEMP_CRIT    85.0f
#define THRESH_VIB_ALERT    1.5f
#define THRESH_VIB_CRIT     3.0f
#define COOLDOWN_MS         60000

// ── Intervalos de tasks ───────────────────────────────────────────────────────
#define SENSOR_INTERVAL_MS      100
#define TELEMETRY_INTERVAL_MS   2000
#define DISPLAY_INTERVAL_MS     200
#define BUZZER_INTERVAL_MS      100
