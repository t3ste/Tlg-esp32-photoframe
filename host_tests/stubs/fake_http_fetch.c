// Link-satisfying stub: headlines.c references http_fetch_get() from
// headlines_fetch() (the network-calling wrapper), but the headlines host
// test only exercises headlines_extract() (the pure parsing logic) and
// never actually calls headlines_fetch() - this stub exists purely so the
// test binary links, not to be meaningfully invoked.
#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

const char *esp_err_to_name(esp_err_t code)
{
    return code == ESP_OK ? "ESP_OK" : "ESP_ERR";
}

esp_err_t http_fetch_get(const char *url, int timeout_ms, size_t max_response_bytes,
                         char **out_body, size_t *out_len, bool *out_truncated,
                         const char *user_agent)
{
    (void) url;
    (void) timeout_ms;
    (void) max_response_bytes;
    (void) user_agent;
    *out_body = NULL;
    if (out_len) {
        *out_len = 0;
    }
    if (out_truncated) {
        *out_truncated = false;
    }
    return ESP_FAIL;
}

// Link-satisfying stub for todo.c/calendar_ics.c's conditional-GET (ETag)
// caching - same rationale as http_fetch_get() above, never actually invoked
// by the parser-only host tests.
esp_err_t http_fetch_get_conditional(const char *url, int timeout_ms, size_t max_response_bytes,
                                     const char *if_none_match, char **out_body, size_t *out_len,
                                     bool *out_truncated, char *out_etag, size_t out_etag_len,
                                     bool *out_not_modified, const char *user_agent)
{
    (void) url;
    (void) timeout_ms;
    (void) max_response_bytes;
    (void) if_none_match;
    (void) user_agent;
    *out_body = NULL;
    if (out_len) {
        *out_len = 0;
    }
    if (out_truncated) {
        *out_truncated = false;
    }
    if (out_etag && out_etag_len > 0) {
        out_etag[0] = '\0';
    }
    if (out_not_modified) {
        *out_not_modified = false;
    }
    return ESP_FAIL;
}
