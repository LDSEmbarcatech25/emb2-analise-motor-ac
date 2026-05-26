#include "connectivity.h"
#include "state_machine.h"
#include "config.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "connectivity";
static EventGroupHandle_t s_wifi_eg;
static esp_mqtt_client_handle_t s_mqtt = NULL;
static bool s_mqtt_connected = false;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_MAX_RETRY     5
static int s_retry = 0;

// ─── Wi-Fi ────────────────────────────────────────────────────────────────────
static void wifi_event_handler(void *a, esp_event_base_t base, int32_t id, void *d)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry++ < WIFI_MAX_RETRY) esp_wifi_connect();
        else xEventGroupSetBits(s_wifi_eg, WIFI_FAIL_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retry = 0;
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_init(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    s_wifi_eg = xEventGroupCreate();
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,    wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);
    wifi_config_t wc = { .sta = { .ssid = WIFI_SSID, .password = WIFI_PASS,
                                  .threshold.authmode = WIFI_AUTH_WPA2_PSK } };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_start();
    esp_wifi_connect();
    EventBits_t b = xEventGroupWaitBits(s_wifi_eg,
                      WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                      pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
    return (b & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_FAIL;
}

// ─── MQTT (HiveMQ) + parsing de comandos ────────────────────────────────────
static void mqtt_event_handler(void *a, esp_event_base_t base, int32_t id, void *d)
{
    esp_mqtt_event_handle_t ev = d;
    switch (ev->event_id) {
        case MQTT_EVENT_CONNECTED:
            s_mqtt_connected = true;
            esp_mqtt_client_subscribe(s_mqtt, TOPIC_CMD, 1);
            ESP_LOGI(TAG, "MQTT conectado ao HiveMQ");
            break;
        case MQTT_EVENT_DISCONNECTED:
            s_mqtt_connected = false;
            ESP_LOGW(TAG, "MQTT desconectado");
            break;
        case MQTT_EVENT_DATA: {
            // Parsing simples dos comandos JSON
            char data[64] = {0};
            int len = ev->data_len < 63 ? ev->data_len : 63;
            strncpy(data, ev->data, len);
            if      (strstr(data, "MOTOR_ON"))          state_cmd_on();
            else if (strstr(data, "MOTOR_OFF"))         state_cmd_off();
            else if (strstr(data, "RESET_PROTECTION"))  state_cmd_reset();
            else if (strstr(data, "RECORD_AUDIO"))      { extern bool g_record_audio; g_record_audio = true; }
            ESP_LOGI(TAG, "CMD recebido: %s", data);
            break;
        }
        default: break;
    }
}

static esp_err_t mqtt_init(void)
{
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri      = MQTT_BROKER,
        .broker.address.port     = MQTT_PORT,
        .credentials.username    = MQTT_USER,
        .credentials.authentication.password = MQTT_PASS,
    };
    s_mqtt = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_mqtt);
    for (int i = 0; i < 100 && !s_mqtt_connected; i++)
        vTaskDelay(pdMS_TO_TICKS(100));
    return s_mqtt_connected ? ESP_OK : ESP_FAIL;
}

// ─── API pública ─────────────────────────────────────────────────────────────
esp_err_t connectivity_init(void)
{
    ESP_ERROR_CHECK(wifi_init());
    ESP_ERROR_CHECK(mqtt_init());
    return ESP_OK;
}

esp_mqtt_client_handle_t connectivity_get_client(void) { return s_mqtt; }

// Mantido para compatibilidade — telemetry agora é montada no main
esp_err_t connectivity_publish(const sensor_data_t *data, float rms, alarm_level_t level)
{
    (void)data; (void)rms; (void)level;
    return ESP_OK;
}
