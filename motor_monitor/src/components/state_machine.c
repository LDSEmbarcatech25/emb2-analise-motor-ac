#include "state_machine.h"
#include "config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "state";
static motor_state_t s_state = STATE_IDLE;
static int64_t s_protected_at = 0;

esp_err_t state_init(void)
{
    gpio_set_direction(RELAY_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(RELAY_GPIO, 0);  // relé aberto na inicialização
    return ESP_OK;
}

motor_state_t state_get(void) { return s_state; }

const char *state_str(motor_state_t s)
{
    switch (s) {
        case STATE_IDLE:      return "IDLE";
        case STATE_ARMED:     return "ARMED";
        case STATE_ALERT:     return "ALERT";
        case STATE_PROTECTED: return "PROTECTED";
        case STATE_COOLDOWN:  return "COOLDOWN";
        default:              return "UNKNOWN";
    }
}

static void trigger_protection(const char *reason, const motor_metrics_t *m)
{
    gpio_set_level(RELAY_GPIO, 0);  // abre o relé (desliga motor)
    s_state = STATE_PROTECTED;
    s_protected_at = esp_timer_get_time();
    ESP_LOGW(TAG, "PROTEÇÃO ACIONADA: %s | RMS=%.2fg Temp=%.1f°C",
             reason, m->accel_rms, m->temp_c);
}

void state_update(const motor_metrics_t *m)
{
    switch (s_state) {
        case STATE_COOLDOWN: {
            int64_t elapsed = (esp_timer_get_time() - s_protected_at) / 1000;
            if (elapsed >= COOLDOWN_MS) {
                s_state = STATE_IDLE;
                ESP_LOGI(TAG, "Cooldown concluído → IDLE");
            }
            break;
        }
        case STATE_PROTECTED:
            s_state = STATE_COOLDOWN;
            break;

        case STATE_ARMED:
        case STATE_ALERT: {
            // Verifica limiares críticos
            if (m->accel_rms >= THRESH_VIB_CRIT) {
                trigger_protection("VIBRATION", m);
            } else if (m->temp_c >= THRESH_TEMP_CRIT) {
                trigger_protection("TEMPERATURE", m);
            } else if (m->accel_rms >= THRESH_VIB_ALERT || m->temp_c >= THRESH_TEMP_ALERT) {
                s_state = STATE_ALERT;
            } else {
                s_state = STATE_ARMED;
            }
            break;
        }
        default:
            break;
    }
}

void state_cmd_on(void)
{
    if (s_state == STATE_IDLE) {
        gpio_set_level(RELAY_GPIO, 1);
        s_state = STATE_ARMED;
        ESP_LOGI(TAG, "Motor LIGADO → ARMED");
    }
}

void state_cmd_off(void)
{
    gpio_set_level(RELAY_GPIO, 0);
    s_state = STATE_IDLE;
    ESP_LOGI(TAG, "Motor DESLIGADO → IDLE");
}

void state_cmd_reset(void)
{
    if (s_state == STATE_PROTECTED || s_state == STATE_COOLDOWN) {
        s_state = STATE_IDLE;
        ESP_LOGI(TAG, "Proteção resetada → IDLE");
    }
}
