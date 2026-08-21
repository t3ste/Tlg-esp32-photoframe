#ifndef HEADLINES_H
#define HEADLINES_H

#include <stddef.h>

#include "esp_err.h"

#define HEADLINE_MAX_COUNT 3
#define HEADLINE_MAX_LEN 160

typedef struct {
    int count;
    char titles[HEADLINE_MAX_COUNT][HEADLINE_MAX_LEN];
} headlines_result_t;

/**
 * @brief Fetches `feed_url` (any RSS or Atom feed - no API key, no rate
 * limit) and extracts up to `max_count` (clamped to
 * [1, HEADLINE_MAX_COUNT]) headline titles, in feed order.
 *
 * Uses a lightweight, purpose-built extractor - NOT a full XML/RSS parser:
 * scans for `<item>`/`<entry>` blocks (RSS/Atom respectively), pulls the
 * `<title>` within each, unwraps CDATA, decodes a small fixed set of HTML
 * entities, and runs the result through
 * image_processor_sanitize_ascii() so titles are already ASCII-safe by the
 * time they reach the display overlay code. Each title is truncated to
 * HEADLINE_MAX_LEN - 1 chars.
 *
 * Best-effort: a fetch failure returns an error and leaves *out zeroed
 * (count = 0). A response larger than the internal fetch cap is still
 * parsed from whatever prefix was captured, rather than treated as a
 * failure - a feed with more items than fit is still useful for its first
 * few.
 */
esp_err_t headlines_fetch(const char *feed_url, int max_count, headlines_result_t *out);

/**
 * @brief Pure extraction logic behind headlines_fetch(), split out so it's
 * host-testable without a real HTTP fetch: parses `body` (a feed response
 * already in memory, `body_len` bytes) directly. See headlines_fetch() for
 * the extraction rules (CDATA, entities, ASCII sanitization, truncation).
 */
esp_err_t headlines_extract(const char *body, size_t body_len, int max_count, headlines_result_t *out);

#endif
