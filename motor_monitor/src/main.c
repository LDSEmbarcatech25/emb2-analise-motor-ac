#include "config.h"
#include "sensors.h"       // MPU6050
#include "dht22.h"
#include "alarm.h"
#include "state_machine.h"
#include "connectivity.h"
#include "display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

static const char *TAG = "main";

// ── Estado global compartilhado ───────────────────────────────────────────────
static motor_metrics_t  g_metrics  = {0};
static SemaphoreHandle_t g_mutex;
static bool g_buzzer_silence = false;
static bool g_record_audio   = false;

// ── Helpers ───────────────────────────────────────────────────────────────────
static float peak(float a, float b, float c) {
    float m = fabsf(a);
    if (fabsf(b) > m) m = fabsf(b);
    if (fabsf(c) > m) m = fabsf(c);
    return m;
}

// ── task_sensors (Core 0, 10 Hz) ──────────────────────────────────────────────
static void task_sensors(void *arg)
{
    alarm_signature_t sig = {0};
    alarm_calibrate(&sig);   // ~5s de calibração

    sensor_data_t imu_data;
    dht22_data_t  dht_data;

    while (1) {
        motor_metrics_t m = {0};

        if (sensors_read_all(&imu_data) == ESP_OK) {
            m.accel_rms  = alarm_rms(&imu_data.imu);
            m.ax_peak    = fabsf(imu_data.imu.ax);
            m.ay_peak    = fabsf(imu_data.imu.ay);
            m.az_peak    = fabsf(imu_data.imu.az);
        }
        if (dht22_read(&dht_data) == ESP_OK) {
            m.temp_c       = dht_data.temperature_c;
            m.humidity_pct = dht_data.humidity_pct;
        }

        xSemaphoreTake(g_mutex, portMAX_DELAY);
        g_metrics = m;
        xSemaphoreGive(g_mutex);

        vTaskDelay(pdMS_TO_TICKS(SENSOR_INTERVAL_MS));
    }
}

// ── task_protection (Core 0, 10 Hz) ───────────────────────────────────────────
static void task_protection(void *arg)
{
    while (1) {
        motor_metrics_t m;
        xSemaphoreTake(g_mutex, portMAX_DELAY);
        m = g_metrics;
        xSemaphoreGive(g_mutex);

        state_update(&m);

        // Botão reset (GPIO 45) — religar pós-proteção
        if (!gpio_get_level(BTN_RESET_GPIO))
            state_cmd_reset();

        vTaskDelay(pdMS_TO_TICKS(SENSOR_INTERVAL_MS));
    }
}

// ── task_telemetry (Core 0, 0.5 Hz) ───────────────────────────────────────────
static void task_telemetry(void *arg)
{
    while (1) {
        motor_metrics_t m;
        xSemaphoreTake(g_mutex, portMAX_DELAY);
        m = g_metrics;
        xSemaphoreGive(g_mutex);

        char payload[320];
        snprintf(payload, sizeof(payload),
            "{\"device_id\":\"%s\","
            "\"ts\":%lld,"
            "\"temp_c\":%.2f,"
            "\"humidity_pct\":%.1f,"
            "\"accel_rms_g\":%.3f,"
            "\"accel_x_peak\":%.3f,"
            "\"accel_y_peak\":%.3f,"
            "\"accel_z_peak\":%.3f,"
            "\"motor_state\":\"%s\"}",
            DEVICE_ID,
            esp_timer_get_time() / 1000000LL,
            m.temp_c, m.humidity_pct,
            m.accel_rms,
            m.ax_peak, m.ay_peak, m.az_peak,
            state_str(state_get()));

        esp_mqtt_client_publish(connectivity_get_client(),
                                TOPIC_TELEMETRY, payload, 0, 1, 0);

        ESP_LOGI(TAG, "%s", payload);
        vTaskDelay(pdMS_TO_TICKS(TELEMETRY_INTERVAL_MS));
    }
}

// ── task_display (Core 1, 5 Hz) ───────────────────────────────────────────────
static void task_display(void *arg)
{
    sensor_data_t fake = {0};  // display_update usa sensor_data_t + rms + level
    while (1) {
        motor_metrics_t m;
        xSemaphoreTake(g_mutex, portMAX_DELAY);
        m = g_metrics;
        xSemaphoreGive(g_mutex);

        // Mapeia motor_state → alarm_level para reusar display existente
        alarm_level_t lv = (state_get() == STATE_PROTECTED) ? ALARM_CONFIRMED :
                           (state_get() == STATE_ALERT)     ? ALARM_PROBABLE  : ALARM_NONE;

        fake.env.temperature_c = m.temp_c;
        fake.env.humidity_pct  = m.humidity_pct;
        display_update(&fake, m.accel_rms, lv);
        vTaskDelay(pdMS_TO_TICKS(DISPLAY_INTERVAL_MS));
    }
}

// ── task_buzzer (Core 1, 10 Hz) ───────────────────────────────────────────────
static void task_buzzer(void *arg)
{
    // Padrão: ALERT = bip lento, PROTECTED = bip rápido
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_10_BIT, .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_timer_config(&t);
    ledc_channel_config_t ch = {
        .gpio_num = BUZZER_GPIO, .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0, .duty = 0
    };
    ledc_channel_config(&ch);

    int tick = 0;
    while (1) {
        if (!g_buzzer_silence) {
            motor_state_t s = state_get();
            bool on = false;
            if      (s == STATE_PROTECTED) on = (tick % 4  < 2);  // rápido
            else if (s == STATE_ALERT)     on = (tick % 10 < 2);  // lento
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, on ? 512 : 0);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
        }

        // Botão silenciar (GPIO 46)
        if (!gpio_get_level(BTN_SILENCE_GPIO)) g_buzzer_silence = true;
        if (state_get() == STATE_IDLE) g_buzzer_silence = false;  // reativa

        tick++;
        vTaskDelay(pdMS_TO_TICKS(BUZZER_INTERVAL_MS));
    }
}

// ── task_mqtt_cmd (Core 1, evento) ────────────────────────────────────────────
// Callback registrado em connectivity_init() via mqtt_event_handler
// Os comandos chegam como JSON no tópico motor/{id}/cmd
// Veja connectivity.c para o parsing e chamadas state_cmd_*

// ── app_main ──────────────────────────────────────────────────────────────────
void app_main(void)
{
    // GPIO inputs com pull-up
    gpio_set_direction(BTN_RESET_GPIO,   GPIO_MODE_INPUT);
    gpio_set_pull_mode(BTN_RESET_GPIO,   GPIO_PULLUP_ONLY);
    gpio_set_direction(BTN_SILENCE_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BTN_SILENCE_GPIO, GPIO_PULLUP_ONLY);

    ESP_ERROR_CHECK(sensors_init());           // MPU6050 (I2C)
    ESP_ERROR_CHECK(dht22_init(DHT22_GPIO));   // DHT22 (GPIO 36)
    ESP_ERROR_CHECK(state_init());             // relé
    ESP_ERROR_CHECK(display_init());           // OLED
    ESP_ERROR_CHECK(connectivity_init());      // Wi-Fi + MQTT HiveMQ

    g_mutex = xSemaphoreCreateMutex();

    // Core 0
    xTaskCreatePinnedToCore(task_sensors,    "sensors",    6144, NULL, 6, NULL, 0);
    xTaskCreatePinnedToCore(task_protection, "protection", 4096, NULL, 7, NULL, 0);
    xTaskCreatePinnedToCore(task_telemetry,  "telemetry",  6144, NULL, 4, NULL, 0);

    // Core 1
    xTaskCreatePinnedToCore(task_display, "display", 4096, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(task_buzzer,        "buzzer",   2048, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(task_audio_record, "audio_rec", 8192, NULL, 2, NULL, 1);
}

// ── task_audio_record (Core 1, sob demanda) ───────────────────────────────────
// Ativada quando connectivity.c seta g_record_audio via comando RECORD_AUDIO
#include "pdm_mic.h"
#include "audio_post.h"

void task_audio_record(void *arg)
{
    while (1) {
        // Aguarda sinal via flag (setada no callback MQTT)
        if (g_record_audio) {
            g_record_audio = false;
            ESP_LOGI("audio", "Iniciando gravação PDM 5 s...");

            int16_t *buf = malloc(PDM_BUF_SIZE);
            if (!buf) { ESP_LOGE("audio", "Sem memória"); continue; }

            pdm_mic_init();
            size_t written = 0;
            if (pdm_mic_record(buf, PDM_BUF_SIZE, &written) == ESP_OK) {
                audio_post_send(buf, written);
            }
            pdm_mic_deinit();
            free(buf);
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}
