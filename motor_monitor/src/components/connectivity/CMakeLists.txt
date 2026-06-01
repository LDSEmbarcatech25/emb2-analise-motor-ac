idf_component_register(
    SRCS "connectivity.c"
    REQUIRES driver esp_wifi esp_netif mqtt esp_event nvs_flash fatfs ssd1306_lib
    INCLUDE_DIRS "."
)
