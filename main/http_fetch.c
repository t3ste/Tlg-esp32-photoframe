#include "http_fetch.h"

#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "http_fetch";

// Mirrors telegram_bot.c's TELEGRAM_HTTP_RETRY_COUNT/_DELAY_MS - transient
// TLS/network hiccups are common enough on ESP32 to warrant a couple of
// quick retries rather than giving up on the first blip.
#define HTTP_FETCH_RETRY_COUNT 3
#define HTTP_FETCH_RETRY_DELAY_MS 1500

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    size_t max_len;
    bool overflow;
} http_body_buf_t;

static esp_err_t body_capture_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA) {
        return ESP_OK;
    }
    http_body_buf_t *ctx = (http_body_buf_t *) evt->user_data;
    if (ctx->overflow || evt->data_len <= 0) {
        return ESP_OK;
    }

    size_t need = ctx->len + (size_t) evt->data_len + 1;
    if (need > ctx->max_len) {
        // Cap reached - keep whatever was captured so far and stop growing;
        // not necessarily a hard failure for callers happy with a truncated
        // body (e.g. an RSS feed's trailing items).
        ctx->overflow = true;
        return ESP_OK;
    }
    if (need > ctx->cap) {
        size_t new_cap = ctx->cap ? ctx->cap * 2 : 4096;
        while (new_cap < need) {
            new_cap *= 2;
        }
        if (new_cap > ctx->max_len) {
            new_cap = ctx->max_len;
        }
        char *grown = realloc(ctx->buf, new_cap);
        if (!grown) {
            ESP_LOGE(TAG, "Out of memory growing response buffer to %zu bytes", new_cap);
            ctx->overflow = true;
            return ESP_OK;
        }
        ctx->buf = grown;
        ctx->cap = new_cap;
    }

    memcpy(ctx->buf + ctx->len, evt->data, evt->data_len);
    ctx->len += evt->data_len;
    ctx->buf[ctx->len] = '\0';
    return ESP_OK;
}

esp_err_t http_fetch_get(const char *url, int timeout_ms, size_t max_response_bytes,
                         char **out_body, size_t *out_len, bool *out_truncated)
{
    *out_body = NULL;
    if (out_len) {
        *out_len = 0;
    }
    if (out_truncated) {
        *out_truncated = false;
    }

    esp_err_t last_err = ESP_FAIL;

    for (int attempt = 1; attempt <= HTTP_FETCH_RETRY_COUNT; attempt++) {
        if (attempt > 1) {
            ESP_LOGW(TAG, "Retrying GET (%d/%d) after %d ms...", attempt, HTTP_FETCH_RETRY_COUNT,
                     HTTP_FETCH_RETRY_DELAY_MS);
            vTaskDelay(pdMS_TO_TICKS(HTTP_FETCH_RETRY_DELAY_MS));
        }

        http_body_buf_t ctx = {.max_len = max_response_bytes};

        esp_http_client_config_t config = {
            .url = url,
            .timeout_ms = timeout_ms,
            .event_handler = body_capture_handler,
            .user_data = &ctx,
            .buffer_size = 2048,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            ESP_LOGE(TAG, "Failed to init HTTP client for GET");
            free(ctx.buf);
            last_err = ESP_FAIL;
            continue;
        }

        esp_err_t err = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "GET failed: %s", esp_err_to_name(err));
            free(ctx.buf);
            last_err = err;
            continue;
        }
        if (status != 200 || !ctx.buf) {
            ESP_LOGE(TAG, "GET returned HTTP %d", status);
            free(ctx.buf);
            last_err = ESP_FAIL;
            continue;
        }

        *out_body = ctx.buf;
        if (out_len) {
            *out_len = ctx.len;
        }
        if (out_truncated) {
            *out_truncated = ctx.overflow;
        }
        return ESP_OK;
    }

    return last_err;
}
