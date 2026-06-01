/**
 * @file display.h
 * @brief Módulo de renderização do display OLED SSD1306.
 *
 * Gerencia inicialização I2C, boot screen, task de renderização com
 * 5 páginas de navegação (status, temperatura, vibração, rede, firmware).
 */

#ifndef DISPLAY_H
#define DISPLAY_H

#include "project_defs.h"
#include "ssd1306.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Inicializa o barramento I2C master.
 */
void init_i2c(void);

/**
 * @brief Inicializa o display OLED e exibe o boot screen.
 */
void display_boot_init(void);

/**
 * @brief Atualiza uma linha do boot screen com status de um periférico.
 */
void display_boot_update(uint8_t line, const char *text);

/**
 * @brief Retorna ponteiro para a instância SSD1306 do display.
 * Permite que outros módulos (ex: WiFi event handler) atualizem o OLED.
 */
SSD1306_t *display_get_dev(void);

/**
 * @brief Task FreeRTOS de renderização do display OLED.
 * Exibe 5 páginas navegáveis via botões GPIO.
 */
void task_display(void *pvParameters);

#ifdef __cplusplus
}
#endif

#endif // DISPLAY_H
