/**
 * @file connectivity.c
 * @brief WiFi, MQTT, telemetria, SD card e auditoria blockchain.
 *
 * Extrai toda a lógica de conectividade do main.c original:
 * - WiFi event handler e inicialização
 * - MQTT event handler e inicialização
 * - task_telemetry (envio de dados + processamento de comandos)
 * - init_sd_card (barramento SPI + FAT)
 * - register_blockchain_audit_log
 */

#include "connectivity.h"
#include "config.h"
#include "components/state_machine/state_machine.h"
#include "components/sensors/sensors.h"

#include "driver/sdspi_host.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "esp_wifi.h"
#include "mqtt_client.h"
#include "nvs_flash.h"
#include "sdmmc_cmd.h"
#include "ssd1306.h"

#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>

static const char *TAG = "CONNECTIVITY";
static esp_mqtt_client_handle_t s_mqtt_client = NULL;

// Referência ao display para atualizar WiFi status no boot screen
extern SSD1306_t *display_get_dev(void);

// =============================================================================
// Auditoria Blockchain
// =============================================================================

void register_blockchain_audit_log(const char *event_type, const char *description) {
    char payload[384];
    snprintf(payload, sizeof(payload),
             "{\"device_id\":\"%s\",\"event_type\":\"%s\",\"description\":\"%s\",\"firmware_hash\":\"%s\"}",
             DEVICE_ID, event_type, description, FIRMWARE_HASH_HEX);

    char pub_topic[64];
    snprintf(pub_topic, sizeof(pub_topic), "motor/%s/audit", DEVICE_ID);

    EventBits_t uxBits = xEventGroupGetBits(xSystemEvents);
    if (uxBits & EVENT_MQTT_CONNECTED) {
        int msg_id = esp_mqtt_client_publish(s_mqtt_client, pub_topic, payload, 0, 1, 0);
        ESP_LOGI(TAG, "MQTT [AUDIT] Enviado para Dashboard Web3 (msg_id=%d) -> %s", msg_id, event_type);
    } else {
        ESP_LOGW(TAG, "MQTT [AUDIT] Sem rede! Evento local apenas: %s - %s", event_type, description);
    }
}

esp_mqtt_client_handle_t connectivity_get_mqtt_client(void) {
    return s_mqtt_client;
}

// =============================================================================
// SD Card
// =============================================================================

void init_sd_card(void) {
    ESP_LOGI(TAG, "Inicializando barramento SPI para Cartão SD...");

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_card_t *card;
    const char mount_point[] = "/sdcard";

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_SD_MOSI,
        .miso_io_num = PIN_SD_MISO,
        .sclk_io_num = PIN_SD_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };

    esp_err_t ret = spi_bus_initialize(host.slot, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao inicializar o barramento SPI para o SD Card.");
        return;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_SD_CS;
    slot_config.host_id = host.slot;

    ESP_LOGI(TAG, "Montando sistema de arquivos FAT no Cartão SD...");
    ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Erro ao montar o cartão SD. Verifique se o cartão está inserido.");
    } else {
        ESP_LOGI(TAG, "Cartão SD montado com sucesso em %s! Capacidade: %llu MB",
                 mount_point,
                 ((uint64_t)card->csd.capacity) * card->csd.sector_size / (1024 * 1024));
    }
}

// =============================================================================
// Wi-Fi
// =============================================================================

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *evt = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "Wi-Fi desconectado (Razao: %d). Tentando reconectar...", evt->reason);
        xEventGroupClearBits(xSystemEvents, EVENT_WIFI_CONNECTED);
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Wi-Fi conectado! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(xSystemEvents, EVENT_WIFI_CONNECTED);

        // Atualiza OLED com WiFi OK
        SSD1306_t *pDev = display_get_dev();
        if (pDev) {
            ssd1306_display_text(pDev, 4, " [OK] WiFi      ", 16, false);
        }
    }
}

static void wifi_scan_task(void *pvParameters) {
    ESP_LOGI(TAG, "Iniciando escaneamento de redes Wi-Fi na Task...");
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err == ESP_OK) {
        uint16_t number = 20;
        wifi_ap_record_t ap_info[20];
        uint16_t ap_count = 0;
        esp_wifi_scan_get_ap_num(&ap_count);
        if (ap_count > 0) {
            if (ap_count > number) ap_count = number;
            esp_wifi_scan_get_ap_records(&ap_count, ap_info);
            ESP_LOGI(TAG, "Encontradas %d redes Wi-Fi em alcance:", ap_count);
            for (int i = 0; i < ap_count; i++) {
                ESP_LOGI(TAG, "  [%d] SSID: %-20.20s | RSSI: %d dBm | Canal: %d", 
                         i + 1, (char *)ap_info[i].ssid, ap_info[i].rssi, ap_info[i].primary);
            }
        } else {
            ESP_LOGW(TAG, "Nenhuma rede Wi-Fi encontrada em alcance.");
        }
    } else {
        ESP_LOGE(TAG, "Falha ao iniciar escaneamento Wi-Fi: %d", err);
    }
    
    vTaskDelete(NULL);
}

void init_wifi(void) {
    ESP_LOGI(TAG, "Inicializando Wi-Fi...");
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_OPEN,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    // Executa escaneamento de redes visíveis no boot usando uma task dedicada para evitar stack overflow na main task
    xTaskCreatePinnedToCore(wifi_scan_task, "wifi_scan_task", 4096, NULL, 5, NULL, 0);
    
    ESP_LOGI(TAG, "Aguardando conexao Wi-Fi (SSID: %s)...", WIFI_SSID);
}

// =============================================================================
// MQTT
// =============================================================================

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    esp_mqtt_client_handle_t client = event->client;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT conectado ao broker!");
        xEventGroupSetBits(xSystemEvents, EVENT_MQTT_CONNECTED);

        // Subscreve nos tópicos de comando e limiares
        esp_mqtt_client_subscribe(client, "motor/" DEVICE_ID "/cmd", 1);
        esp_mqtt_client_subscribe(client, "motor/" DEVICE_ID "/thresholds", 1);
        register_blockchain_audit_log(
            AUDIT_EVENT_BOOT, "MQTT conectado. Subscrito nos tópicos de comando.");

        // Publish boot status with firmware hash
        {
            char boot_topic[64];
            snprintf(boot_topic, sizeof(boot_topic), "motor/%s/status", DEVICE_ID);
            char boot_payload[256];
            snprintf(boot_payload, sizeof(boot_payload),
                     "{\"connection\":\"ONLINE\",\"firmware_hash\":\"%s\"}",
                     FIRMWARE_HASH_HEX);
            esp_mqtt_client_publish(client, boot_topic, boot_payload, 0, 1, 1);
        }
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT desconectado. Aguardando reconexão automática...");
        xEventGroupClearBits(xSystemEvents, EVENT_MQTT_CONNECTED);
        break;

    case MQTT_EVENT_DATA: {
        // Copia o tópico e payload para buffers terminados em \0
        char topic[128] = {0};
        char payload[256] = {0};
        int topic_len = (event->topic_len < (int)sizeof(topic) - 1)
                            ? event->topic_len
                            : (int)sizeof(topic) - 1;
        int payload_len = (event->data_len < (int)sizeof(payload) - 1)
                              ? event->data_len
                              : (int)sizeof(payload) - 1;
        memcpy(topic, event->topic, topic_len);
        memcpy(payload, event->data, payload_len);

        ESP_LOGI(TAG, "MQTT [REC]: %s -> %s", topic, payload);

        // Tópico de Comando: extrai action/value/command_id do JSON manualmente
        char cmd_topic[128];
        snprintf(cmd_topic, sizeof(cmd_topic), "motor/%s/cmd", DEVICE_ID);
        if (strcmp(topic, cmd_topic) == 0) {
            mqtt_command_t cmd = {0};
            char *p;
            if ((p = strstr(payload, "\"command_id\":\"")) != NULL) {
                sscanf(p, "\"command_id\":\"%39[^\"]\"", cmd.command_id);
            }
            if ((p = strstr(payload, "\"action\":\"")) != NULL) {
                sscanf(p, "\"action\":\"%31[^\"]\"", cmd.action);
            }
            if ((p = strstr(payload, "\"value\":\"")) != NULL) {
                sscanf(p, "\"value\":\"%15[^\"]\"", cmd.value);
            }
            if (xQueueSend(xCommandQueue, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
                ESP_LOGW(TAG, "Fila de comandos cheia! Comando descartado.");
            }
        }

        // Tópico de Limiares: atualiza dinamicamente
        char thr_topic[128];
        snprintf(thr_topic, sizeof(thr_topic), "motor/%s/thresholds", DEVICE_ID);
        if (strcmp(topic, thr_topic) == 0) {
            motor_thresholds_t new_t = {0};
            char *p;
            if ((p = strstr(payload, "\"temp_alert_c\":")) != NULL)
                sscanf(p, "\"temp_alert_c\":%f", &new_t.temp_alert);
            if ((p = strstr(payload, "\"temp_critical_c\":")) != NULL)
                sscanf(p, "\"temp_critical_c\":%f", &new_t.temp_critical);
            if ((p = strstr(payload, "\"vib_alert_g\":")) != NULL)
                sscanf(p, "\"vib_alert_g\":%f", &new_t.vib_alert);
            if ((p = strstr(payload, "\"vib_critical_g\":")) != NULL)
                sscanf(p, "\"vib_critical_g\":%f", &new_t.vib_critical);
            if (new_t.temp_critical > 0 && new_t.vib_critical > 0) {
                update_active_thresholds(&new_t);
                register_blockchain_audit_log(
                    AUDIT_EVENT_THRESHOLD_CHANGE,
                    "Limiares atualizados remotamente via MQTT.");
            }
        }
        break;
    }
    default:
        break;
    }
}

void init_mqtt(void) {
    ESP_LOGI(TAG, "Aguardando Wi-Fi para iniciar MQTT...");
    xEventGroupWaitBits(xSystemEvents, EVENT_WIFI_CONNECTED, pdFALSE, pdTRUE,
                        pdMS_TO_TICKS(30000));

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        .session.keepalive = 30,
    };

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(
        s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_mqtt_client));
    ESP_LOGI(TAG, "MQTT client iniciado. Broker: %s", MQTT_BROKER_URI);
}

// =============================================================================
// Task de Telemetria
// =============================================================================

void task_telemetry(void *pvParameters) {
    ESP_LOGI(TAG, "Iniciando Task de Telemetria e Histórico...");
    sensor_data_t sData = {0};
    mqtt_command_t sCmd = {0};
    char payload[256];

    while (1) {
        // Envio periódico a cada 5 segundos
        if (xQueueReceive(xSensorQueue, &sData, pdMS_TO_TICKS(5000)) == pdTRUE) {
            motor_state_t eState = get_system_state();

            // Backup local no cartão SD (CSV real)
            {
                const char *csv_path = "/sdcard/telemetry.csv";
                bool file_exists = (access(csv_path, F_OK) == 0);
                FILE *f = fopen(csv_path, "a");
                if (f != NULL) {
                    if (!file_exists) {
                        fprintf(f, "timestamp_ms,temp_c,humidity,vib_rms_g,vib_peak_g,state\n");
                    }
                    fprintf(f, "%lld,%.2f,%.1f,%.2f,%.2f,%d\n",
                            (long long)(esp_timer_get_time() / 1000),
                            sData.temperature, sData.humidity,
                            sData.vib_resultant, sData.vib_peak, eState);
                    fclose(f);
                    ESP_LOGI(TAG, "SD_CARD [CSV]: Gravado -> Temp: %.1fC, Vib: %.2fg",
                             sData.temperature, sData.vib_resultant);
                } else {
                    ESP_LOGW(TAG, "SD_CARD: Cartão não disponível. Dados não salvos.");
                }
            }

            // Telemetria MQTT para a nuvem
            EventBits_t uxBits = xEventGroupGetBits(xSystemEvents);
            if (uxBits & EVENT_MQTT_CONNECTED) {
                snprintf(payload, sizeof(payload),
                         "{\"device_id\":\"%s\",\"temp_c\":%.2f,\"vib_rms_g\":%.2f,"
                         "\"state\":%d}",
                         DEVICE_ID, sData.temperature, sData.vib_resultant, eState);

                char pub_topic[64];
                snprintf(pub_topic, sizeof(pub_topic), "motor/%s/telemetry", DEVICE_ID);
                int msg_id =
                    esp_mqtt_client_publish(s_mqtt_client, pub_topic, payload, 0, 1, 0);
                ESP_LOGI(TAG, "MQTT [PUB] (msg_id=%d) %s -> %s", msg_id, pub_topic,
                         payload);
            }
        }

        // Processamento e Handshake de Comandos da Dashboard
        if (xQueueReceive(xCommandQueue, &sCmd, 0) == pdTRUE) {
            ESP_LOGI(TAG, "MQTT [CMD]: Processando ID: %s | Ação: %s",
                     sCmd.command_id, sCmd.action);

            bool cmd_success = false;
            char err_msg[64] = "";

            if (strcmp(sCmd.action, "SET_MOTOR_STATE") == 0) {
                motor_state_t eState = get_system_state();

                if (strcmp(sCmd.value, "ON") == 0) {
                    if (eState == STATE_IDLE) {
                        set_system_state(STATE_ARMED);
                        cmd_success = true;

                        register_blockchain_audit_log(
                            AUDIT_EVENT_OVERRIDE,
                            "Operador ligou o motor remotamente via Dashboard.");
                    } else {
                        snprintf(err_msg, sizeof(err_msg),
                                 "Motor não está em IDLE (Estado atual: %d)", eState);
                    }
                } else if (strcmp(sCmd.value, "OFF") == 0) {
                    set_system_state(STATE_IDLE);
                    cmd_success = true;

                    register_blockchain_audit_log(
                        AUDIT_EVENT_OVERRIDE,
                        "Operador desligou o motor manualmente via Dashboard.");
                }
            } else if (strcmp(sCmd.action, "TRIGGER_AUDIO_RECORD") == 0) {
                xEventGroupSetBits(xSystemEvents, EVENT_TRIGGER_RECORD);
                cmd_success = true;
            } else if (strcmp(sCmd.action, "SET_THRESHOLDS") == 0) {
                motor_thresholds_t new_t = {
                    .temp_alert    = 65.0f,
                    .temp_critical = 80.0f,
                    .vib_alert     = 1.20f,
                    .vib_critical  = 2.80f
                };
                update_active_thresholds(&new_t);
                cmd_success = true;

                register_blockchain_audit_log(
                    AUDIT_EVENT_THRESHOLD_CHANGE,
                    "Alteração remota nos limites críticos de segurança.");
            } else if (strcmp(sCmd.action, "INJECT_ANOMALY") == 0) {
                sensors_inject_anomaly(true);
                cmd_success = true;
                ESP_LOGE(TAG, "TESTE: Comando de Injeção de Falha Recebido! Subindo "
                              "vibração e calor.");
            }

            // Publica o Handshake/Confirmação de recebimento do Comando
            if (xEventGroupGetBits(xSystemEvents) & EVENT_MQTT_CONNECTED) {
                snprintf(payload, sizeof(payload),
                         "{\"command_id\":\"%s\",\"status\":\"%s\",\"error\":\"%s\"}",
                         sCmd.command_id, cmd_success ? "SUCCESS" : "ERROR", err_msg);

                char rsp_topic[64];
                snprintf(rsp_topic, sizeof(rsp_topic), "motor/%s/cmd/response",
                         DEVICE_ID);
                esp_mqtt_client_publish(s_mqtt_client, rsp_topic, payload, 0, 1, 0);
                ESP_LOGI(TAG, "MQTT [PUB] (%s) -> %s", rsp_topic, payload);
            }
        }
    }
}
