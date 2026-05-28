#include "state_machine.h"
#include "config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "state_machine";

static motor_state_t s_state = STATE_IDLE;

// Variáveis de controle de tempo não-bloqueantes
static int64_t s_prestart_start = 0;
static int64_t s_relay2_pulse_start = 0;
static bool s_relay2_pulsing = false;

static int64_t s_relay1_pulse_start = 0;
static bool s_relay1_disconnecting = false;

esp_err_t state_init(void)
{
    // Configura Relé 1 (Bloqueio)
    gpio_set_direction(RELAY_1_BIO_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(RELAY_1_BIO_GPIO, 0);  // NC fechado

    // Configura Relé 2 (Pulso de partida)
    gpio_set_direction(RELAY_2_CMD_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(RELAY_2_CMD_GPIO, 0);  // NO aberto

    // Configura Buzzer
    gpio_set_direction(BUZZER_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(BUZZER_GPIO, 0); // Desligado inicialmente

    s_state = STATE_IDLE;
    return ESP_OK;
}

motor_state_t state_get(void) { return s_state; }

const char *state_str(motor_state_t s)
{
    switch (s) {
        case STATE_IDLE:      return "IDLE (Pronto)";
        case STATE_PRESTART:  return "PRESTART (Buzzer Ativo)";
        case STATE_ARMED:     return "ARMED (Ligado Normal)";
        case STATE_ALERT:     return "ALERT (Limiar 1 Atingido)";
        case STATE_PROTECTED: return "PROTECTED (Bloqueado/Erro)";
        default:              return "UNKNOWN";
    }
}

static void trigger_protection(const char *reason, const motor_metrics_t *m)
{
    gpio_set_level(RELAY_1_BIO_GPIO, 1);  // ATIVA Relé 1 = Abre contato NC, derruba o motor
    gpio_set_level(BUZZER_GPIO, 0);       // Garante que o buzzer desliga se houver erro no prestart
    s_state = STATE_PROTECTED;
    ESP_LOGW(TAG, "!!! ERRO GRAVE - PROTEÇÃO ATIVADA: %s | RMS=%.2fg Temp=%.1f°C",
             reason, m->accel_rms, m->temp_c);
}

void state_update(const motor_metrics_t *m)
{
    int64_t now = esp_timer_get_time();

    // ─── GERENCIAMENTO AUTOMÁTICO DE PULSOS DOS RELÉS ───
    
    // Desliga o pulso do Relé 2 após 1 segundo
    if (s_relay2_pulsing && (now - s_relay2_pulse_start >= 1000000)) {
        gpio_set_level(RELAY_2_CMD_GPIO, 0);
        s_relay2_pulsing = false;
        s_state = STATE_ARMED; // Passa de fato para o estado ARMED após o término do pulso física
        ESP_LOGI(TAG, "Relé 2: Pulso de partida concluído (Aberto). Motor operacional.");
    }

    // Desliga o comando temporário do Relé 1 (Desligar Remoto) após 1.5 segundos
    if (s_relay1_disconnecting && (now - s_relay1_pulse_start >= 1500000)) {
        gpio_set_level(RELAY_1_BIO_GPIO, 0); 
        s_relay1_disconnecting = false;
        s_state = STATE_IDLE;
        ESP_LOGI(TAG, "Relé 1: Reset do corte concluído. Painel pronto para nova partida.");
    }

    // ─── MÁQUINA DE ESTADOS PRINCIPAL ───
    switch (s_state) {
        case STATE_PRESTART:
            // Verifica se já se passaram 5 segundos (5000000 microsegundos) de aviso sonoro
            if (now - s_prestart_start >= 5000000) {
                gpio_set_level(BUZZER_GPIO, 0); // Desliga o Buzzer
                ESP_LOGW(TAG, "Pré-aviso concluído! Acionando motor agora...");

                // Transiciona para a ativação física do Relé 2
                gpio_set_level(RELAY_2_CMD_GPIO, 1); // Fecha contato NO
                s_relay2_pulse_start = esp_timer_get_time();
                s_relay2_pulsing = true;
                
                // Mudamos temporariamente para rastrear o pulso antes de cravar ARMED definitivo
                s_state = STATE_ARMED; 
            }
            break;

        case STATE_PROTECTED:
            break;

        case STATE_ARMED:
        case STATE_ALERT:
            // Monitoramento dos Limiares Críticos (Pode usar os dados vindos de alarm_detect se preferir)
            if (m->accel_rms >= THRESH_VIB_CRIT) {
                trigger_protection("VIBRATION_CRIT", m);
            } else if (m->temp_c >= THRESH_TEMP_CRIT) {
                trigger_protection("TEMPERATURE_CRIT", m);
            } else if (m->accel_rms >= THRESH_VIB_ALERT || m->temp_c >= THRESH_TEMP_ALERT) {
                if (s_state != STATE_ALERT) {
                    s_state = STATE_ALERT;
                    ESP_LOGW(TAG, "[ALERTA] Limiar 1 atingido! RMS=%.2fg Temp=%.1f°C", m->accel_rms, m->temp_c);
                }
            } else {
                s_state = STATE_ARMED;
            }
            break;

        case STATE_IDLE:
            if (m->accel_rms >= THRESH_VIB_CRIT || m->temp_c >= THRESH_TEMP_CRIT) {
                trigger_protection("CRIT_IN_IDLE", m);
            }
            break;
    }
}

// Comando vindo do botão físico ou Dashboard para LIGAR
void state_cmd_on(void)
{
    if (s_state == STATE_PROTECTED) {
        ESP_LOGE(TAG, "Comando LIGAR Rejeitado! O sistema está BLOQUEADO por segurança.");
        return;
    }

    if (s_state != STATE_IDLE) {
        ESP_LOGE(TAG, "Comando rejeitado. Sistema não está em IDLE.");
        return;
    }

    // Em vez de acionar o Relé 2 direto, inicia o ciclo do Buzzer (PRESTART)
    s_state = STATE_PRESTART;
    s_prestart_start = esp_timer_get_time();
    gpio_set_level(BUZZER_GPIO, 1); // Liga o Buzzer físico
    
    ESP_LOGW(TAG, "Comando LIGAR recebido. ATIVANDO BUZZER de segurança por 5 segundos...");
}

// Comando vindo do Dashboard para DESLIGAR
void state_cmd_off(void)
{
    if (s_state == STATE_PROTECTED) {
        ESP_LOGE(TAG, "Motor já desligado devido ao bloqueio de segurança.");
        return;
    }

    // Se tentarem desligar durante o tempo do alarme de pré-partida
    if (s_state == STATE_PRESTART) {
        gpio_set_level(BUZZER_GPIO, 0); // Desliga o alarme
        s_state = STATE_IDLE;
        ESP_LOGI(TAG, "Partida cancelada durante o pré-aviso. Retornando para IDLE.");
        return;
    }

    if (!s_relay1_disconnecting) {
        gpio_set_level(RELAY_1_BIO_GPIO, 1); // Abre contato NC temporariamente para desarmar
        s_relay1_pulse_start = esp_timer_get_time();
        s_relay1_disconnecting = true;
        ESP_LOGW(TAG, "Comando DESLIGAR recebido. Cortando retenção por 1.5s via Relé 1...");
    }
}

// Comando vindo do Dashboard ou Botão físico para RESETAR o erro do sistema
void state_cmd_reset(const motor_metrics_t *current_metrics)
{
    if (s_state == STATE_PROTECTED) {
        if (current_metrics->accel_rms < THRESH_VIB_CRIT && current_metrics->temp_c < THRESH_TEMP_CRIT) {
            gpio_set_level(RELAY_1_BIO_GPIO, 0); // Fecha o contato NC novamente
            s_state = STATE_IDLE;
            ESP_LOGI(TAG, "Trava de segurança resetada com sucesso! Sistema em IDLE.");
        } else {
            ESP_LOGE(TAG, "Falha ao resetar! Os sensores ainda apontam valores críticos.");
        }
    }
}

// #include "state_machine.h"
// #include "config.h"
// #include "driver/gpio.h"
// #include "esp_log.h"
// #include "esp_timer.h"
// #include "freertos/FreeRTOS.h"
// #include "freertos/task.h"

// static const char *TAG = "state_machine";

// static motor_state_t s_state = STATE_IDLE;

// // Variáveis para controle de pulso de relé sem travar o processador
// static int64_t s_relay2_pulse_start = 0;
// static bool s_relay2_pulsing = false;

// static int64_t s_relay1_pulse_start = 0;
// static bool s_relay1_disconnecting = false;

// esp_err_t state_init(void)
// {
//     // Configura Relé 1 (Bloqueio)
//     gpio_set_direction(RELAY_1_BIO_GPIO, GPIO_MODE_OUTPUT);
//     gpio_set_level(RELAY_1_BIO_GPIO, 0);  // 0 = Contato NC fechado (Permite passagem de energia)

//     // Configura Relé 2 (Pulso de partida)
//     gpio_set_direction(RELAY_2_CMD_GPIO, GPIO_MODE_OUTPUT);
//     gpio_set_level(RELAY_2_CMD_GPIO, 0);  // 0 = Contato NO aberto (Botão solto)

//     s_state = STATE_IDLE;
//     return ESP_OK;
// }

// motor_state_t state_get(void) { return s_state; }

// const char *state_str(motor_state_t s)
// {
//     switch (s) {
//         case STATE_IDLE:      return "IDLE (Pronto)";
//         case STATE_ARMED:     return "ARMED (Ligado Normal)";
//         case STATE_ALERT:     return "ALERT (Limiar 1 Atingido)";
//         case STATE_PROTECTED: return "PROTECTED (Bloqueado/Erro)";
//         default:              return "UNKNOWN";
//     }
// }

// static void trigger_protection(const char *reason, const motor_metrics_t *m)
// {
//     gpio_set_level(RELAY_1_BIO_GPIO, 1);  // ATIVA Relé 1 = Abre contato NC, derruba o motor elétrico
//     s_state = STATE_PROTECTED;
//     ESP_LOGW(TAG, "!!! ERRO GRAVE - PROTEÇÃO ATIVADA: %s | RMS=%.2fg Temp=%.1f°C",
//              reason, m->accel_rms, m->temp_c);
// }

// void state_update(const motor_metrics_t *m)
// {
//     int64_t now = esp_timer_get_time();

//     // ─── GERENCIAMENTO AUTOMÁTICO DE PULSOS (NÃO-BLOQUEANTE) ───
    
//     // Desliga o pulso do Relé 2 após 1 segundo (1000000 microsegundos)
//     if (s_relay2_pulsing && (now - s_relay2_pulse_start >= 1000000)) {
//         gpio_set_level(RELAY_2_CMD_GPIO, 0);
//         s_relay2_pulsing = false;
//         ESP_LOGI(TAG, "Relé 2: Pulso de partida concluído (Aberto).");
//     }

//     // Desliga o comando temporário do Relé 1 (Desligar Remoto) após 1.5 segundos
//     if (s_relay1_disconnecting && (now - s_relay1_pulse_start >= 1500000)) {
//         gpio_set_level(RELAY_1_BIO_GPIO, 0); // Fecha o NC novamente, deixando pronto para ligar
//         s_relay1_disconnecting = false;
//         s_state = STATE_IDLE;
//         ESP_LOGI(TAG, "Relé 1: Reset do corte concluído. Painel pronto para nova partida.");
//     }

//     // ─── MÁQUINA DE ESTADOS PRINCIPAL ───
//     switch (s_state) {
//         case STATE_PROTECTED:
//             // Trava de segurança ativa. Ignora sensores e comandos externos.
//             // O Relé 1 fica travado em 1 (Aberto).
//             break;

//         case STATE_ARMED:
//         case STATE_ALERT:
//             // 1. Verificação de Erro Crítico (Limiar 2)
//             if (m->accel_rms >= THRESH_VIB_CRIT) {
//                 trigger_protection("VIBRATION_CRIT", m);
//             } else if (m->temp_c >= THRESH_TEMP_CRIT) {
//                 trigger_protection("TEMPERATURE_CRIT", m);
//             } 
//             // 2. Verificação de Alerta Visual/Notificação (Limiar 1)
//             else if (m->accel_rms >= THRESH_VIB_ALERT || m->temp_c >= THRESH_TEMP_ALERT) {
//                 if (s_state != STATE_ALERT) {
//                     s_state = STATE_ALERT;
//                     ESP_LOGW(TAG, "[ALERTA] Limiar 1 atingido! Notificar Dashboard. RMS=%.2fg Temp=%.1f°C", 
//                              m->accel_rms, m->temp_c);
//                 }
//             } else {
//                 s_state = STATE_ARMED;
//             }
//             break;

//         case STATE_IDLE:
//             // Se o motor está em IDLE mas apresenta anomalias antes mesmo de ligar
//             if (m->accel_rms >= THRESH_VIB_CRIT || m->temp_c >= THRESH_TEMP_CRIT) {
//                 trigger_protection("CRIT_IN_IDLE", m);
//             }
//             break;
//     }
// }

// // Comando vindo do botão físico ou Dashboard para LIGAR
// void state_cmd_on(void)
// {
//     if (s_state == STATE_PROTECTED) {
//         ESP_LOGE(TAG, "Comando LIGAR Rejeitado! O sistema está BLOQUEADO por segurança.");
//         return;
//     }

//     if (s_relay1_disconnecting) {
//         ESP_LOGE(TAG, "Aguarde o ciclo de desligamento terminar.");
//         return;
//     }

//     if (!s_relay2_pulsing) {
//         gpio_set_level(RELAY_2_CMD_GPIO, 1); // Fecha contato NO (Simula pressionar botão de Start)
//         s_relay2_pulse_start = esp_timer_get_time();
//         s_relay2_pulsing = true;
//         s_state = STATE_ARMED;
//         ESP_LOGI(TAG, "Comando LIGAR recebido. Gerando pulso de 1s no Relé 2...");
//     }
// }

// // Comando vindo do Dashboard para DESLIGAR
// void state_cmd_off(void)
// {
//     if (s_state == STATE_PROTECTED) {
//         ESP_LOGE(TAG, "Motor já desligado devido ao bloqueio de segurança.");
//         return;
//     }

//     if (!s_relay1_disconnecting) {
//         gpio_set_level(RELAY_1_BIO_GPIO, 1); // Abre contato NC temporariamente para desarmar a retenção
//         s_relay1_pulse_start = esp_timer_get_time();
//         s_relay1_disconnecting = true;
//         ESP_LOGW(TAG, "Comando DESLIGAR recebido. Cortando retenção por 1.5s via Relé 1...");
//     }
// }

// // Comando vindo do Dashboard ou Botão físico para RESETAR o erro do sistema
// void state_cmd_reset(const motor_metrics_t *current_metrics)
// {
//     if (s_state == STATE_PROTECTED) {
//         // Verifica se os sensores voltaram de fato a níveis aceitáveis
//         if (current_metrics->accel_rms < THRESH_VIB_CRIT && current_metrics->temp_c < THRESH_TEMP_CRIT) {
//             gpio_set_level(RELAY_1_BIO_GPIO, 0); // Fecha o contato NC novamente
//             s_state = STATE_IDLE;
//             ESP_LOGI(TAG, "Trava de segurança resetada com sucesso! Sistema em IDLE.");
//         } else {
//             ESP_LOGE(TAG, "Falha ao resetar! Os sensores ainda apontam valores críticos. Temp=%.1f°C", 
//                      current_metrics->temp_c);
//         }
//     }
// }



// // #include "state_machine.h"
// // #include "config.h"
// // #include "driver/gpio.h"
// // #include "esp_log.h"
// // #include "esp_timer.h"
// // #include "freertos/FreeRTOS.h"
// // #include "freertos/task.h"

// // static const char *TAG = "state";
// // static motor_state_t s_state = STATE_IDLE;
// // static int64_t s_protected_at = 0;

// // esp_err_t state_init(void)
// // {
// //     gpio_set_direction(RELAY_GPIO, GPIO_MODE_OUTPUT);
// //     gpio_set_level(RELAY_GPIO, 0);  // relé aberto na inicialização
// //     return ESP_OK;
// // }

// // motor_state_t state_get(void) { return s_state; }

// // const char *state_str(motor_state_t s)
// // {
// //     switch (s) {
// //         case STATE_IDLE:      return "IDLE";
// //         case STATE_ARMED:     return "ARMED";
// //         case STATE_ALERT:     return "ALERT";
// //         case STATE_PROTECTED: return "PROTECTED";
// //         case STATE_COOLDOWN:  return "COOLDOWN";
// //         default:              return "UNKNOWN";
// //     }
// // }

// // static void trigger_protection(const char *reason, const motor_metrics_t *m)
// // {
// //     gpio_set_level(RELAY_GPIO, 0);  // abre o relé (desliga motor)
// //     s_state = STATE_PROTECTED;
// //     s_protected_at = esp_timer_get_time();
// //     ESP_LOGW(TAG, "PROTEÇÃO ACIONADA: %s | RMS=%.2fg Temp=%.1f°C",
// //              reason, m->accel_rms, m->temp_c);
// // }

// // void state_update(const motor_metrics_t *m)
// // {
// //     switch (s_state) {
// //         case STATE_COOLDOWN: {
// //             int64_t elapsed = (esp_timer_get_time() - s_protected_at) / 1000;
// //             if (elapsed >= COOLDOWN_MS) {
// //                 s_state = STATE_IDLE;
// //                 ESP_LOGI(TAG, "Cooldown concluído → IDLE");
// //             }
// //             break;
// //         }
// //         case STATE_PROTECTED:
// //             s_state = STATE_COOLDOWN;
// //             break;

// //         case STATE_ARMED:
// //         case STATE_ALERT: {
// //             // Verifica limiares críticos
// //             if (m->accel_rms >= THRESH_VIB_CRIT) {
// //                 trigger_protection("VIBRATION", m);
// //             } else if (m->temp_c >= THRESH_TEMP_CRIT) {
// //                 trigger_protection("TEMPERATURE", m);
// //             } else if (m->accel_rms >= THRESH_VIB_ALERT || m->temp_c >= THRESH_TEMP_ALERT) {
// //                 s_state = STATE_ALERT;
// //             } else {
// //                 s_state = STATE_ARMED;
// //             }
// //             break;
// //         }
// //         default:
// //             break;
// //     }
// // }

// // void state_cmd_on(void)
// // {
// //     if (s_state == STATE_IDLE) {
// //         gpio_set_level(RELAY_GPIO, 1);
// //         s_state = STATE_ARMED;
// //         ESP_LOGI(TAG, "Motor LIGADO → ARMED");
// //     }
// // }

// // void state_cmd_off(void)
// // {
// //     gpio_set_level(RELAY_GPIO, 0);
// //     s_state = STATE_IDLE;
// //     ESP_LOGI(TAG, "Motor DESLIGADO → IDLE");
// // }

// // void state_cmd_reset(void)
// // {
// //     if (s_state == STATE_PROTECTED || s_state == STATE_COOLDOWN) {
// //         s_state = STATE_IDLE;
// //         ESP_LOGI(TAG, "Proteção resetada → IDLE");
// //     }
// // }
