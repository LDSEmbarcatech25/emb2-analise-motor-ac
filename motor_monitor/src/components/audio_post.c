#include "audio_post.h"
#include "config.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "audio_post";

esp_err_t audio_post_send(const int16_t *buf, size_t bytes)
{
    esp_http_client_config_t cfg = {
        .url    = AUDIO_SERVER_URL,
        .method = HTTP_METHOD_POST,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);

    // multipart/form-data manual (campo "file", PCM 16-bit mono 16 kHz)
    const char *boundary = "----KIIRABoundary";
    char header[256];
    snprintf(header, sizeof(header),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"motor.pcm\"\r\n"
        "Content-Type: application/octet-stream\r\n\r\n",
        boundary);
    char footer[64];
    snprintf(footer, sizeof(footer), "\r\n--%s--\r\n", boundary);

    char content_type[64];
    snprintf(content_type, sizeof(content_type),
             "multipart/form-data; boundary=%s", boundary);

    size_t total = strlen(header) + bytes + strlen(footer);
    esp_http_client_set_header(client, "Content-Type", content_type);
    esp_http_client_set_post_field(client, NULL, total);  // reserva tamanho

    esp_err_t ret = esp_http_client_open(client, (int)total);
    if (ret != ESP_OK) goto cleanup;

    esp_http_client_write(client, header, strlen(header));
    esp_http_client_write(client, (const char *)buf, bytes);
    esp_http_client_write(client, footer, strlen(footer));

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP POST status=%d (%u bytes enviados)", status, (unsigned)bytes);
    ret = (status == 200) ? ESP_OK : ESP_FAIL;

cleanup:
    esp_http_client_cleanup(client);
    return ret;
}
