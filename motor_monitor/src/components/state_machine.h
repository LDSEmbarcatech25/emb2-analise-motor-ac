#pragma once
#include "esp_err.h"
#include <stdbool.h>

typedef enum {
    STATE_IDLE = 0,
    STATE_ARMED,
    STATE_ALERT,
    STATE_PROTECTED,   // relé aberto, bloqueado
    STATE_COOLDOWN,    // aguardando 60s
} motor_state_t;

typedef struct {
    float temp_c;
    float humidity_pct;
    float accel_rms;
    float ax_peak, ay_peak, az_peak;
} motor_metrics_t;

motor_state_t state_get(void);
const char   *state_str(motor_state_t s);
void          state_update(const motor_metrics_t *m);
void          state_cmd_on(void);
void          state_cmd_off(void);
void          state_cmd_reset(void);   // reset proteção via MQTT ou botão
ESP_ERR_T     state_init(void);
