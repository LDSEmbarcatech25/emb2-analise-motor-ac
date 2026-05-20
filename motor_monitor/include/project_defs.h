#ifndef PROJECT_DEFS_H
#define PROJECT_DEFS_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// Definições de Portas GPIO (ESP32-S3 KIIRA)
// =============================================================================
#define PIN_I2C_SDA         14
#define PIN_I2C_SCL         21

#define PIN_I2S_SCK         2
#define PIN_I2S_WS          37
#define PIN_I2S_SD          35

#define PIN_DHT22           36
#define PIN_RELE            47
#define PIN_BUZZER          3

#define PIN_SD_CS           38
#define PIN_SD_SCK          40
#define PIN_SD_MOSI         39
#define PIN_SD_MISO         41

#define PIN_BTN_RESET       4
#define PIN_BTN_SILENCE     5

// =============================================================================
// Limiares de Alerta e Proteção do Motor AC
// =============================================================================
#define TEMP_THRESHOLD_ALERT      70.0f  // °C
#define TEMP_THRESHOLD_CRITICAL   85.0f  // °C

#define VIB_THRESHOLD_ALERT       1.50f  // g (Resultante RMS tridimensional)
#define VIB_THRESHOLD_CRITICAL    3.00f  // g (Resultante RMS tridimensional)

#define COOLDOWN_DURATION_S       60     // 1 minuto de travamento para resfriamento

// =============================================================================
// Hash de Integridade do Firmware (Representação de 32 bytes para auditoria)
// =============================================================================
// Em produção, isso seria calculado dinamicamente ou inserido no build (SHA-256)
#define FIRMWARE_HASH_HEX   "d6a8f10b2c349e5d7f10b2c349e5d7f10b2c349e5d7f10b2c349e5d7f10b2c34"

// =============================================================================
// Códigos de Eventos de Auditoria Blockchain
// =============================================================================
#define AUDIT_EVENT_BOOT             "BOOT"
#define AUDIT_EVENT_SHUTDOWN         "SHUTDOWN"
#define AUDIT_EVENT_OVERRIDE         "OVERRIDE"
#define AUDIT_EVENT_THRESHOLD_CHANGE "THRESHOLD_CHANGE"

// =============================================================================
// Estados Globais do Sistema
// =============================================================================
typedef enum {
    STATE_IDLE = 0,      // Motor desligado (manualmente ou em repouso)
    STATE_ARMED,         // Monitoramento ativo, motor em funcionamento normal
    STATE_ALERT,         // Vibração ou temperatura em nível de alerta
    STATE_PROTECTED,     // Motor desarmado por violação crítica (trava de resfriamento)
    STATE_COOLDOWN       // Período de resfriamento ativo, religamento bloqueado
} motor_state_t;

// =============================================================================
// Estruturas de Dados do Sistema
// =============================================================================

// Dados brutos de leitura dos sensores
typedef struct {
    float temperature;
    float humidity;
    float accel_x_rms;
    float accel_y_rms;
    float accel_z_rms;
    float vib_peak;
    float vib_resultant;
} sensor_data_t;

// Controle de comandos recebidos via MQTT
typedef struct {
    char command_id[40];
    char action[32];     // Ex: "SET_MOTOR_STATE", "TRIGGER_AUDIO_RECORD"
    char value[16];      // Ex: "ON", "OFF"
    int param_duration;  // Parâmetro opcional de duração de gravação
} mqtt_command_t;

// Ajuste dinâmico de limiares de segurança
typedef struct {
    float temp_alert;
    float temp_critical;
    float vib_alert;
    float vib_critical;
} motor_thresholds_t;

// =============================================================================
// Sinalizadores de Eventos (Event Groups)
// =============================================================================
#define EVENT_WIFI_CONNECTED      (1 << 0)
#define EVENT_MQTT_CONNECTED      (1 << 1)
#define EVENT_ALERT_ACTIVE        (1 << 2)
#define EVENT_SILENCE_BUZZER      (1 << 3)
#define EVENT_TRIGGER_RECORD      (1 << 4)

#ifdef __cplusplus
}
#endif

#endif // PROJECT_DEFS_H
