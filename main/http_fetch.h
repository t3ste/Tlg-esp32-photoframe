#ifndef HTTP_FETCH_H
#define HTTP_FETCH_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/**
 * @brief Performs a GET request (with a couple of retries on transient
 * network/TLS failure) against a plain public HTTPS endpoint (verified via
 * ESP-IDF's built-in public CA bundle - no custom cert pinning), returning
 * the response body.
 *
 * Shared by weather.c and headlines.c, whose fetch needs are identical in
 * shape to telegram_bot.c's telegram_http_get() but target non-secret URLs
 * (Open-Meteo, a user-configured RSS feed) so no token redaction is needed.
 *
 * @param url URL to GET.
 * @param timeout_ms Per-attempt HTTP timeout.
 * @param max_response_bytes Response body cap; a response larger than this
 * is truncated (whatever was captured up to the cap is still returned) - the
 * caller decides whether a truncated body is still useful (e.g. an RSS feed
 * with truncated trailing items is still fine to parse).
 * @param out_body Response body, NUL-terminated (caller frees with free()).
 * NULL on total failure (no data at all captured).
 * @param out_len Response body length in bytes (excluding the NUL), or NULL
 * if not needed.
 * @param out_truncated Set to true if the response was cut off at
 * max_response_bytes, false otherwise. May be NULL if not needed.
 * @param user_agent Custom User-Agent header value, or NULL for ESP-IDF's
 * default. Some free APIs (e.g. MET Norway's yr.no) require a real,
 * identifying User-Agent and reject/throttle requests without one.
 */
esp_err_t http_fetch_get(const char *url, int timeout_ms, size_t max_response_bytes,
                         char **out_body, size_t *out_len, bool *out_truncated,
                         const char *user_agent);

#endif
