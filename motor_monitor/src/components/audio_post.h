#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

esp_err_t audio_post_send(const int16_t *buf, size_t bytes);
