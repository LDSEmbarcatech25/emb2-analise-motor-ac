#include "dht22.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rom/ets_sys.h"
#include <string.h>

static const char *TAG = "dht22";
static int s_gpio = -1;

esp_err_t dht22_init(int gpio)
{
    s_gpio = gpio;
    gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
    gpio_set_level(gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(1000));  // estabilização
    ESP_LOGI(TAG, "DHT22 inicializado (GPIO %d)", gpio);
    return ESP_OK;
}

static esp_err_t dht22_read_raw(uint8_t data[5])
{
    // Sinal de start: LOW 1ms, HIGH 30us
    gpio_set_direction(s_gpio, GPIO_MODE_OUTPUT);
    gpio_set_level(s_gpio, 0);
    ets_delay_us(1000);
    gpio_set_level(s_gpio, 1);
    ets_delay_us(30);
    gpio_set_direction(s_gpio, GPIO_MODE_INPUT);

    // Aguarda resposta do sensor
    int timeout = 100;
    while (gpio_get_level(s_gpio) == 1 && --timeout) ets_delay_us(1);
    if (!timeout) return ESP_ERR_TIMEOUT;
    timeout = 100;
    while (gpio_get_level(s_gpio) == 0 && --timeout) ets_delay_us(1);
    if (!timeout) return ESP_ERR_TIMEOUT;
    timeout = 100;
    while (gpio_get_level(s_gpio) == 1 && --timeout) ets_delay_us(1);
    if (!timeout) return ESP_ERR_TIMEOUT;

    // Lê 40 bits
    memset(data, 0, 5);
    for (int i = 0; i < 40; i++) {
        timeout = 60;
        while (gpio_get_level(s_gpio) == 0 && --timeout) ets_delay_us(1);
        ets_delay_us(35);
        if (gpio_get_level(s_gpio)) data[i / 8] |= (1 << (7 - (i % 8)));
        timeout = 60;
        while (gpio_get_level(s_gpio) == 1 && --timeout) ets_delay_us(1);
    }
    return ESP_OK;
}

esp_err_t dht22_read(dht22_data_t *out)
{
    uint8_t data[5] = {0};
    esp_err_t ret = dht22_read_raw(data);
    if (ret != ESP_OK) return ret;

    uint8_t checksum = data[0] + data[1] + data[2] + data[3];
    if (checksum != data[4]) return ESP_ERR_INVALID_CRC;

    out->humidity_pct    = ((data[0] << 8 | data[1]) & 0x7FFF) / 10.0f;
    out->temperature_c   = ((data[2] << 8 | data[3]) & 0x7FFF) / 10.0f;
    if (data[2] & 0x80) out->temperature_c *= -1;
    return ESP_OK;
}
