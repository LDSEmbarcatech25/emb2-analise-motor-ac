/**
 * @file buzzer.c
 * @brief Sinalização sonora via buzzer PWM (LEDC) do motor AC Kiira.
 *
 * Extraído do main.c original (task_buzzer + init_ledc_buzzer).
 */

#include "buzzer.h"
#include "components/state_machine/state_machine.h"

#include "driver/ledc.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "BUZZER";

// Referências externas (criadas no main.c)
extern EventGroupHandle_t xSystemEvents;

// =============================================================================
// Inicialização do PWM LEDC
// =============================================================================

void init_ledc_buzzer(void) {
    ESP_LOGI(TAG, "Inicializando PWM LEDC (Buzzer)...");
    ledc_timer_config_t ledc_timer = {
        .speed_mode     = LEDC_LOW_SPEED_MODE,
        .timer_num      = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .freq_hz        = 2700,
        .clk_cfg        = LEDC_AUTO_CLK
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .timer_sel  = LEDC_TIMER_0,
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = PIN_BUZZER,
        .duty       = 0,
        .hpoint     = 0
    };
    ledc_channel_config(&ledc_channel);
}

void buzzer_test_beep(uint32_t duration_ms) {
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 4000);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

// =============================================================================
// Task FreeRTOS — Buzzer
// =============================================================================

void task_buzzer(void *pvParameters) {
    ESP_LOGI(TAG, "Iniciando Task de Sinalização Buzzer...");

    while (1) {
        EventBits_t uxBits = xEventGroupGetBits(xSystemEvents);
        motor_state_t eState = get_system_state();

        // Botão para silenciar o alarme sonoro temporariamente
#if PIN_BTN_SILENCE >= 0
        if (gpio_get_level(PIN_BTN_SILENCE) == 0) {
            xEventGroupSetBits(xSystemEvents, EVENT_SILENCE_BUZZER);
            ESP_LOGW(TAG, "BUZZER: Alarme mutado localmente pelo operador.");
        }
#endif

        // Teste de Buzzer disparado remotamente
        if (uxBits & EVENT_TEST_BUZZER) {
            ESP_LOGI(TAG, "TESTE: Comando de teste do buzzer recebido. Emitindo 3 bipes...");
            for (int i = 0; i < 3; i++) {
                ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 4000);
                ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
                vTaskDelay(pdMS_TO_TICKS(150));

                ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
                ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
                vTaskDelay(pdMS_TO_TICKS(150));
            }
            xEventGroupClearBits(xSystemEvents, EVENT_TEST_BUZZER);
            continue;
        }

        if ((uxBits & EVENT_ALERT_ACTIVE) && !(uxBits & EVENT_SILENCE_BUZZER) &&
            (eState == STATE_PROTECTED || eState == STATE_COOLDOWN || eState == STATE_ALERT)) {
            if (eState == STATE_PROTECTED || eState == STATE_COOLDOWN) {
                ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 4000);
                ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
                vTaskDelay(pdMS_TO_TICKS(150));

                ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
                ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
                vTaskDelay(pdMS_TO_TICKS(150));
            } else if (eState == STATE_ALERT) {
                ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 4000);
                ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
                vTaskDelay(pdMS_TO_TICKS(500));

                ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
                ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        } else {
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);

            if (!(uxBits & EVENT_ALERT_ACTIVE)) {
                xEventGroupClearBits(xSystemEvents, EVENT_SILENCE_BUZZER);
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}
