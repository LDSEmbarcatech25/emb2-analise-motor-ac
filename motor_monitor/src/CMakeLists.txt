# CMakeLists.txt — Componente principal (main) do firmware Kiira
# GLOB_RECURSE coleta todos os .c/.h nas subpastas de src/

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.c)

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS "." "../include"
    REQUIRES driver esp_wifi esp_netif mqtt esp_event nvs_flash fatfs
             ssd1306_lib pdm_mic
)
