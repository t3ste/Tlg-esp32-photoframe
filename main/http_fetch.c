#include "http_fetch.h"

#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
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
    char *etag_out;  // NULL if the caller doesn't want the ETag captured
    size_t etag_out_len;
} http_body_buf_t;

static esp_err_t body_capture_handler(esp_http_client_event_t *evt)
{
    http_body_buf_t *ctx = (http_body_buf_t *) evt->user_data;

    if (evt->event_id == HTTP_EVENT_ON_HEADER) {
        if (ctx->etag_out && ctx->etag_out_len > 0 && strcasecmp(evt->header_key, "ETag") == 0) {
            strncpy(ctx->etag_out, evt->header_value, ctx->etag_out_len - 1);
            ctx->etag_out[ctx->etag_out_len - 1] = '\0';
        }
        return ESP_OK;
    }

    if (evt->event_id != HTTP_EVENT_ON_DATA) {
        return ESP_OK;
    }
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
        // PSRAM, not the default (internal-preferred) heap: this can grow up
        // to max_response_bytes (tens of KB for weather/headlines), and
        // internal SRAM exhaustion here was measured to break a subsequent
        // TLS handshake in the same wake cycle - see the fix in
        // display_manager.c's rotate_random() for the original incident.
        char *grown = heap_caps_realloc(ctx->buf, new_cap, MALLOC_CAP_SPIRAM);
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

// Shared retry/client-setup core behind both http_fetch_get() and
// http_fetch_get_conditional() - the two differ only in whether they send
// If-None-Match and whether a 304 is treated as success-with-no-body rather
// than a retry-worthy failure.
static esp_err_t do_http_fetch(const char *url, int timeout_ms, size_t max_response_bytes,
                               const char *if_none_match, char **out_body, size_t *out_len,
                               bool *out_truncated, char *out_etag, size_t out_etag_len,
                               bool *out_not_modified, const char *user_agent)
{
    *out_body = NULL;
    if (out_len) {
        *out_len = 0;
    }
    if (out_truncated) {
        *out_truncated = false;
    }
    if (out_not_modified) {
        *out_not_modified = false;
    }
    if (out_etag && out_etag_len > 0) {
        out_etag[0] = '\0';
    }

    esp_err_t last_err = ESP_FAIL;

    for (int attempt = 1; attempt <= HTTP_FETCH_RETRY_COUNT; attempt++) {
        if (attempt > 1) {
            ESP_LOGW(TAG, "Retrying GET (%d/%d) after %d ms...", attempt, HTTP_FETCH_RETRY_COUNT,
                     HTTP_FETCH_RETRY_DELAY_MS);
            vTaskDelay(pdMS_TO_TICKS(HTTP_FETCH_RETRY_DELAY_MS));
        }

        if (out_etag && out_etag_len > 0) {
            out_etag[0] = '\0';  // don't carry a partial value across retries
        }
        http_body_buf_t ctx = {
            .max_len = max_response_bytes, .etag_out = out_etag, .etag_out_len = out_etag_len};

        esp_http_client_config_t config = {
            .url = url,
            .timeout_ms = timeout_ms,
            .event_handler = body_capture_handler,
            .user_data = &ctx,
            .buffer_size = 2048,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .user_agent = user_agent,
            // This project's lwIP config resolves only one address per
            // hostname (CONFIG_LWIP_DNS_MAX_HOST_IP=1) with no
            // "happy eyeballs" fallback between address families - if that
            // one address happens to be IPv6 and the route to it is
            // broken/slow for this network (a real-world case found live:
            // a Google Calendar ICS fetch failed with ESP_ERR_HTTP_CONNECT
            // on every attempt despite the identical URL working fine from
            // a browser/curl, which do have automatic dual-stack fallback),
            // the fetch fails outright with no retry via IPv4. Forcing IPv4
            // here sidesteps that whole class of failure; IPv4 reachability
            // is universal for every host this shared helper talks to.
            .addr_type = HTTP_ADDR_TYPE_INET,
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            ESP_LOGE(TAG, "Failed to init HTTP client for GET");
            free(ctx.buf);
            last_err = ESP_FAIL;
            continue;
        }

        if (if_none_match && if_none_match[0] != '\0') {
            esp_http_client_set_header(client, "If-None-Match", if_none_match);
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

        if (status == 304) {
            ESP_LOGI(TAG, "GET returned HTTP 304 Not Modified");
            free(ctx.buf);
            if (out_not_modified) {
                *out_not_modified = true;
            }
            return ESP_OK;
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

esp_err_t http_fetch_get(const char *url, int timeout_ms, size_t max_response_bytes,
                         char **out_body, size_t *out_len, bool *out_truncated,
                         const char *user_agent)
{
    return do_http_fetch(url, timeout_ms, max_response_bytes, NULL, out_body, out_len,
                         out_truncated, NULL, 0, NULL, user_agent);
}

esp_err_t http_fetch_get_conditional(const char *url, int timeout_ms, size_t max_response_bytes,
                                     const char *if_none_match, char **out_body, size_t *out_len,
                                     bool *out_truncated, char *out_etag, size_t out_etag_len,
                                     bool *out_not_modified, const char *user_agent)
{
    return do_http_fetch(url, timeout_ms, max_response_bytes, if_none_match, out_body, out_len,
                         out_truncated, out_etag, out_etag_len, out_not_modified, user_agent);
}
