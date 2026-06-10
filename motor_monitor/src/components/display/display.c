/**
 * @file display.c
 * @brief Renderização do display OLED SSD1306 com 5 páginas de navegação.
 *
 * Páginas:
 *   1 — Status do Motor (estado, relé, uptime)
 *   2 — Temperatura e Umidade (com indicador de limite)
 *   3 — Vibração (resultante, pico, eixos X/Y/Z)
 *   4 — Conectividade (WiFi, MQTT, Device ID)
 *   5 — Firmware e Segurança (hash, limiares, versão)
 */

#include "display.h"
#include "components/sensors/sensors.h"
#include "components/state_machine/state_machine.h"
#include "components/connectivity/connectivity.h"
#include "config.h"

#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "font8x8_basic.h"
#include "ssd1306.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "DISPLAY";

// Instância do display OLED (gerenciada exclusivamente por este módulo)
static SSD1306_t dev;

// =============================================================================
// API Pública
// =============================================================================

SSD1306_t *display_get_dev(void) { return &dev; }

void init_i2c(void) {
  ESP_LOGI(TAG, "Inicializando I2C0...");
  i2c_config_t conf = {
      .mode = I2C_MODE_MASTER,
      .sda_io_num = PIN_I2C_SDA,
      .sda_pullup_en = GPIO_PULLUP_ENABLE,
      .scl_io_num = PIN_I2C_SCL,
      .scl_pullup_en = GPIO_PULLUP_ENABLE,
      .master.clk_speed = 400000,
  };
  i2c_param_config(I2C_NUM_0, &conf);
  i2c_driver_install(I2C_NUM_0, conf.mode, 0, 0, 0);
}

void display_boot_init(void) {
  i2c_device_add(&dev, I2C_NUM_0, -1, I2C_ADDRESS);
  ssd1306_init(&dev, 128, 64);
  ssd1306_clear_screen(&dev, false);
  ssd1306_contrast(&dev, 0xFF);

  ssd1306_display_text(&dev, 0, "                ", 16, false);
  ssd1306_display_text(&dev, 1, "    KIIRA v2    ", 16, true);
  ssd1306_display_text(&dev, 2, "  Iniciando...  ", 16, false);
  ssd1306_display_text(&dev, 3, "                ", 16, false);
  ssd1306_display_text(&dev, 4, " [..] WiFi      ", 16, false);
  ssd1306_display_text(&dev, 5, " [..] SD Card   ", 16, false);
  ssd1306_display_text(&dev, 6, " [..] Buzzer    ", 16, false);
  ssd1306_display_text(&dev, 7, " [..] I2S Mic   ", 16, false);
  vTaskDelay(pdMS_TO_TICKS(400));
}

void display_boot_update(uint8_t line, const char *text) {
  ssd1306_display_text(&dev, line, text, strlen(text), false);
  vTaskDelay(pdMS_TO_TICKS(150));
}

// =============================================================================
// Task FreeRTOS — Display OLED com 5 Páginas
// =============================================================================

void task_display(void *pvParameters) {
  ESP_LOGI(TAG, "Iniciando Task de Renderização do Display OLED...");

  // Aguarda 3 segundos para o usuário ler o checklist de Boot
  vTaskDelay(pdMS_TO_TICKS(3000));

  // Limpa o checklist e prepara para assumir o menu principal
  ssd1306_clear_screen(&dev, false);
  ssd1306_contrast(&dev, 0xFF);

  sensor_data_t sData = {0};
  motor_thresholds_t sThresh = {0};
  char buf[64];
  uint8_t uCurrentPage = 0;
  const uint8_t TOTAL_PAGES = 5;
  bool bBtnDownPrev = true; // Pull-up: HIGH = solto
  bool bBtnSelPrev = true;
  bool bShowFeedback = false;
  uint8_t uFeedbackTimer = 0;

  const char *page_actions[] = {
      "[>] Rearmar    ", // Página 1: Status do Motor
      "[>] Info Temp  ", // Página 2: Temperatura
      "[>] Info Vib   ", // Página 3: Vibração
      "[>] Gravar Aud.", // Página 4: Conectividade
      "[>] Info FW    "  // Página 5: Firmware
  };

  // Variável para cálculo do uptime
  int64_t boot_time_us = esp_timer_get_time();

  while (1) {
    // --- Leitura do estado global usando APIs modulares thread-safe ---
    sensors_get_last_data(&sData);
    motor_state_t eState = get_system_state();
    get_active_thresholds(&sThresh);

    // =============================================================
    // BOTÃO DOWN (GPIO 45): Desce / Troca de página
    // =============================================================
    bool bBtnDownNow = gpio_get_level(PIN_BTN_DOWN);
    if (bBtnDownPrev && !bBtnDownNow) {
      uCurrentPage = (uCurrentPage + 1) % TOTAL_PAGES;
      bShowFeedback = false;
      ssd1306_clear_screen(&dev, false);
      ESP_LOGI(TAG, "OLED: Navegou para pagina %d/%d", uCurrentPage + 1,
               TOTAL_PAGES);
    }
    bBtnDownPrev = bBtnDownNow;

    // =============================================================
    // BOTÃO SELECT (GPIO 46): Executa ação da página atual
    // =============================================================
    bool bBtnSelNow = gpio_get_level(PIN_BTN_SELECT);
    if (bBtnSelPrev && !bBtnSelNow) {
      bShowFeedback = true;
      uFeedbackTimer = 4; // 2 segundos de feedback visual

      switch (uCurrentPage) {
      case 0: // STATUS -> Rearmar Motor
        if (eState == STATE_IDLE) {
          set_system_state(STATE_ARMED);
          register_blockchain_audit_log(AUDIT_EVENT_OVERRIDE,
                                        "Rearme local via menu OLED.");
          ESP_LOGI(TAG, "OLED [ACT]: Motor rearmado pelo menu local.");
        } else {
          ESP_LOGW(TAG, "OLED [ACT]: Rearme negado (estado: %d)", eState);
        }
        break;

      case 1: // TEMPERATURA -> Dump térmico no log
        ESP_LOGI(TAG, "OLED [ACT]: Dump térmico: %.1fC / Umid: %.1f%%",
                 sData.temperature, sData.humidity);
        break;

      case 2: // VIBRAÇÃO -> Dump de vibração no log
        ESP_LOGI(TAG,
                 "OLED [ACT]: Dump vibração: Res=%.2fg Pico=%.2fg X=%.2f "
                 "Y=%.2f Z=%.2f",
                 sData.vib_resultant, sData.vib_peak, sData.accel_x_rms,
                 sData.accel_y_rms, sData.accel_z_rms);
        break;

      case 3: // CONECTIVIDADE -> Gravar Áudio I2S
        xEventGroupSetBits(xSystemEvents, EVENT_TRIGGER_RECORD);
        ESP_LOGI(TAG, "OLED [ACT]: Gravacao de audio disparada pelo menu.");
        break;

      case 4: // FIRMWARE -> Dump de info no log serial
        ESP_LOGI(TAG, "========== INFO FIRMWARE ==========");
        ESP_LOGI(TAG, "Device ID:  %s", DEVICE_ID);
        ESP_LOGI(TAG, "FW Hash:    %s", FIRMWARE_HASH_HEX);
        ESP_LOGI(TAG, "Version:    %s", SYSTEM_VERSION);
        ESP_LOGI(TAG, "Vib Alerta: %.2fg  Critico: %.2fg", sThresh.vib_alert,
                 sThresh.vib_critical);
        ESP_LOGI(TAG, "Tmp Alerta: %.1fC  Critico: %.1fC", sThresh.temp_alert,
                 sThresh.temp_critical);
        ESP_LOGI(TAG, "===================================");
        break;
      }
      ssd1306_clear_screen(&dev, false);
    }
    bBtnSelPrev = bBtnSelNow;

    // --- Timer de feedback visual ---
    if (bShowFeedback) {
      uFeedbackTimer--;
      if (uFeedbackTimer == 0)
        bShowFeedback = false;
    }

    // =============================================================
    // CABEÇALHO (Linha 0): Nome + estado abreviado
    // =============================================================
    const char *state_str;
    switch (eState) {
      case STATE_ARMED:     state_str = "ARM"; break;
      case STATE_ALERT:     state_str = "ALR"; break;
      case STATE_COOLDOWN:  state_str = "COL"; break;
      case STATE_PROTECTED: state_str = "PRT"; break;
      default:              state_str = "OFF"; break;
    }
    snprintf(buf, sizeof(buf), " KIIRA  [%s] ", state_str);
    ssd1306_display_text(&dev, 0, buf, strlen(buf), true);

    // =============================================================
    // RODAPÉ (Linha 7): Ação disponível ou feedback de confirmação
    // =============================================================
    if (bShowFeedback) {
      ssd1306_display_text(&dev, 7, "  >> OK! <<     ", 16, true);
    } else {
      snprintf(buf, sizeof(buf), "%d/%d %s", uCurrentPage + 1, TOTAL_PAGES,
               page_actions[uCurrentPage]);
      ssd1306_display_text(&dev, 7, buf, strlen(buf), false);
    }

    // =============================================================
    // CONTEÚDO POR PÁGINA (Linhas 2-6)
    // =============================================================
    switch (uCurrentPage) {

    // --- PÁGINA 1: STATUS DO MOTOR ---
    case 0: {
      const char *full_state;
      switch (eState) {
        case STATE_ARMED:     full_state = "ARMADO  "; break;
        case STATE_ALERT:     full_state = "ALERTA! "; break;
        case STATE_COOLDOWN:  full_state = "RESFRIAR"; break;
        case STATE_PROTECTED: full_state = "PROTEG.!"; break;
        default:              full_state = "DESLIG. "; break;
      }
      snprintf(buf, sizeof(buf), "STS: %s", full_state);
      ssd1306_display_text(
          &dev, 2, buf, strlen(buf),
          (eState == STATE_PROTECTED || eState == STATE_ALERT));

      // Estado do relé
      bool rele_on = (eState == STATE_ARMED || eState == STATE_ALERT);
      snprintf(buf, sizeof(buf), "Rele: %s  ",
               rele_on ? "ENERGIZ." : "DESLIG. ");
      ssd1306_display_text(&dev, 4, buf, strlen(buf), false);

      // Uptime do sistema
      int64_t uptime_s = (esp_timer_get_time() - boot_time_us) / 1000000;
      int hours = (int)(uptime_s / 3600);
      int minutes = (int)((uptime_s % 3600) / 60);
      int secs = (int)(uptime_s % 60);
      snprintf(buf, sizeof(buf), "Up: %02d:%02d:%02d  ", hours, minutes, secs);
      ssd1306_display_text(&dev, 5, buf, strlen(buf), false);
      break;
    }

    // --- PÁGINA 2: TEMPERATURA E UMIDADE ---
    case 1: {
      snprintf(buf, sizeof(buf), "Temp: %5.1f  C  ", sData.temperature);
      bool temp_over = (sData.temperature >= sThresh.temp_alert);
      ssd1306_display_text(&dev, 2, buf, strlen(buf), temp_over);

      snprintf(buf, sizeof(buf), "Lim:  %5.1f  C  ", sThresh.temp_alert);
      ssd1306_display_text(&dev, 3, buf, strlen(buf), false);

      snprintf(buf, sizeof(buf), "Umid: %5.1f %%   ", sData.humidity);
      ssd1306_display_text(&dev, 5, buf, strlen(buf), false);

      snprintf(buf, sizeof(buf), "%s",
               temp_over ? "!! ACIMA LIMITE !!" : "  Dentro do lim. ");
      ssd1306_display_text(&dev, 6, buf, strlen(buf), temp_over);
      break;
    }

    // --- PÁGINA 3: VIBRAÇÃO ---
    case 2: {
      snprintf(buf, sizeof(buf), "Res:  %5.2f  g  ", sData.vib_resultant);
      bool vib_over = (sData.vib_resultant >= sThresh.vib_alert);
      ssd1306_display_text(&dev, 2, buf, strlen(buf), vib_over);

      snprintf(buf, sizeof(buf), "Pico: %5.2f  g  ", sData.vib_peak);
      ssd1306_display_text(&dev, 3, buf, strlen(buf), false);

      snprintf(buf, sizeof(buf), "X:%.2f Y:%.2f   ", sData.accel_x_rms,
               sData.accel_y_rms);
      ssd1306_display_text(&dev, 5, buf, strlen(buf), false);

      snprintf(buf, sizeof(buf), "Z:%.2f g       ", sData.accel_z_rms);
      ssd1306_display_text(&dev, 6, buf, strlen(buf), false);
      break;
    }

    // --- PÁGINA 4: CONECTIVIDADE ---
    case 3: {
      EventBits_t uxBits = xEventGroupGetBits(xSystemEvents);
      bool bWifi = (uxBits & EVENT_WIFI_CONNECTED) != 0;
      bool bMqtt = (uxBits & EVENT_MQTT_CONNECTED) != 0;
      bool bSd   = (uxBits & EVENT_SD_CARD_OK) != 0;

      snprintf(buf, sizeof(buf), "WiFi: %s   ", bWifi ? "OK     " : "OFFLINE");
      ssd1306_display_text(&dev, 2, buf, strlen(buf), !bWifi);

      snprintf(buf, sizeof(buf), "MQTT: %s   ", bMqtt ? "OK     " : "OFFLINE");
      ssd1306_display_text(&dev, 3, buf, strlen(buf), !bMqtt);

      snprintf(buf, sizeof(buf), "SD:   %s   ", bSd ? "OK     " : "OFFLINE");
      ssd1306_display_text(&dev, 4, buf, strlen(buf), !bSd);

      ssd1306_display_text(&dev, 5, "                ", 16, false);

      // Device ID
      snprintf(buf, sizeof(buf), "ID:%.13s", DEVICE_ID);
      ssd1306_display_text(&dev, 6, buf, strlen(buf), false);
      break;
    }

    // --- PÁGINA 5: FIRMWARE E SEGURANÇA ---
    case 4: {
      snprintf(buf, sizeof(buf), "VibA:%.1f C:%.1f ", sThresh.vib_alert,
               sThresh.vib_critical);
      ssd1306_display_text(&dev, 2, buf, strlen(buf), false);

      snprintf(buf, sizeof(buf), "TmpA:%.0f  C:%.0f ", sThresh.temp_alert,
               sThresh.temp_critical);
      ssd1306_display_text(&dev, 3, buf, strlen(buf), false);

      snprintf(buf, sizeof(buf), "FW:%.16s", FIRMWARE_HASH_HEX);
      ssd1306_display_text(&dev, 5, buf, strlen(buf), false);

      snprintf(buf, sizeof(buf), "v%s          ", SYSTEM_VERSION);
      buf[16] = '\0';
      ssd1306_display_text(&dev, 6, buf, strlen(buf), false);
      break;
    }
    }

    ESP_LOGD(TAG, "OLED [P%d] T:%.1fC V:%.2fg %s", uCurrentPage + 1,
             sData.temperature, sData.vib_resultant, state_str);

    vTaskDelay(pdMS_TO_TICKS(500));
  }
}
