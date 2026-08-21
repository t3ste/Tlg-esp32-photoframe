#include "headlines.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "http_fetch.h"
#include "image_processor.h"

static const char *TAG = "headlines";

#define HEADLINES_HTTP_TIMEOUT_MS 10000
#define HEADLINES_MAX_RESPONSE_BYTES (64 * 1024)
// Generous scratch size for a raw (pre-entity-decode, pre-ASCII-sanitize,
// pre-truncate) title - real-world RSS titles are rarely anywhere near this,
// but a feed with unusually long titles shouldn't overrun anything.
#define RAW_TITLE_BUF_LEN 512

// Finds whichever of `needle1`/`needle2` occurs first at or after `hay`.
// Returns NULL (via *out_which = NULL) if neither is found.
static const char *find_first_of(const char *hay, const char *needle1, const char *needle2,
                                 const char **out_which)
{
    const char *m1 = strstr(hay, needle1);
    const char *m2 = needle2 ? strstr(hay, needle2) : NULL;
    if (m1 && (!m2 || m1 <= m2)) {
        *out_which = needle1;
        return m1;
    }
    if (m2) {
        *out_which = needle2;
        return m2;
    }
    *out_which = NULL;
    return NULL;
}

// Decodes the small fixed set of HTML/XML entities RSS titles commonly use.
// Anything else (an unrecognized named entity, or a non-ASCII numeric
// reference) is copied through as literal text - image_processor_sanitize_ascii()
// downstream will drop what it can't render anyway.
static void decode_entities(const char *in, size_t in_len, char *out, size_t out_len)
{
    size_t o = 0;
    size_t i = 0;
    while (i < in_len && in[i] != '\0' && o + 1 < out_len) {
        if (in[i] == '&') {
            struct {
                const char *entity;
                char value;
            } table[] = {
                {"&amp;", '&'}, {"&lt;", '<'},   {"&gt;", '>'},
                {"&quot;", '"'}, {"&apos;", '\''}, {"&#39;", '\''},
            };
            bool matched = false;
            for (size_t t = 0; t < sizeof(table) / sizeof(table[0]); t++) {
                size_t elen = strlen(table[t].entity);
                if (i + elen <= in_len && strncmp(in + i, table[t].entity, elen) == 0) {
                    out[o++] = table[t].value;
                    i += elen;
                    matched = true;
                    break;
                }
            }
            if (matched) {
                continue;
            }
            // Numeric reference &#NNN; - only handle the printable ASCII range.
            if (i + 2 < in_len && in[i + 1] == '#' && isdigit((unsigned char) in[i + 2])) {
                size_t j = i + 2;
                long code = 0;
                while (j < in_len && isdigit((unsigned char) in[j]) && (j - i) < 8) {
                    code = code * 10 + (in[j] - '0');
                    j++;
                }
                if (j < in_len && in[j] == ';') {
                    if (code >= 0x20 && code <= 0x7E) {
                        out[o++] = (char) code;
                    }
                    i = j + 1;
                    continue;
                }
            }
        }
        out[o++] = in[i++];
    }
    out[o] = '\0';
}

// Extracts the text content of the first <title>...</title> within
// [block_start, block_end), unwrapping a CDATA section if present. Returns
// false if no title tag is found in the block.
static bool extract_title(const char *block_start, const char *block_end, char *out,
                          size_t out_len)
{
    const char *title_open = NULL;
    for (const char *p = block_start; p < block_end;) {
        const char *candidate = strstr(p, "<title");
        if (!candidate || candidate >= block_end) {
            return false;
        }
        // Skip a false match like "<titleXYZ" that isn't actually the tag
        // (must be immediately followed by '>' or whitespace/attributes).
        char next = candidate[6];
        if (next == '>' || next == ' ' || next == '\t' || next == '/') {
            title_open = candidate;
            break;
        }
        p = candidate + 6;
    }
    if (!title_open) {
        return false;
    }
    const char *content_start = strchr(title_open, '>');
    if (!content_start || content_start >= block_end) {
        return false;
    }
    content_start++;
    const char *content_end = strstr(content_start, "</title>");
    if (!content_end || content_end > block_end) {
        return false;
    }

    // Unwrap CDATA if the whole title content is wrapped in one.
    const char *cdata_prefix = "<![CDATA[";
    size_t cdata_prefix_len = strlen(cdata_prefix);
    if ((size_t) (content_end - content_start) >= cdata_prefix_len &&
        strncmp(content_start, cdata_prefix, cdata_prefix_len) == 0) {
        content_start += cdata_prefix_len;
        const char *cdata_end = strstr(content_start, "]]>");
        if (cdata_end && cdata_end < content_end) {
            content_end = cdata_end;
        }
    }

    size_t raw_len = (size_t) (content_end - content_start);
    if (raw_len >= RAW_TITLE_BUF_LEN) {
        raw_len = RAW_TITLE_BUF_LEN - 1;
    }
    char raw[RAW_TITLE_BUF_LEN];
    memcpy(raw, content_start, raw_len);
    raw[raw_len] = '\0';

    char decoded[RAW_TITLE_BUF_LEN];
    decode_entities(raw, raw_len, decoded, sizeof(decoded));

    char ascii[RAW_TITLE_BUF_LEN];
    image_processor_sanitize_ascii(decoded, ascii, sizeof(ascii));

    // Collapse runs of whitespace/newlines (feeds often pretty-print titles
    // across multiple lines) into single spaces, and trim leading/trailing.
    size_t o = 0;
    bool last_was_space = true;  // true so leading whitespace is dropped
    for (const char *p = ascii; *p != '\0' && o + 1 < out_len; p++) {
        bool is_space = (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r');
        if (is_space) {
            if (!last_was_space) {
                out[o++] = ' ';
            }
            last_was_space = true;
        } else {
            out[o++] = *p;
            last_was_space = false;
        }
    }
    while (o > 0 && out[o - 1] == ' ') {
        o--;
    }
    out[o] = '\0';
    return out[0] != '\0';
}

esp_err_t headlines_extract(const char *body, size_t body_len, int max_count, headlines_result_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (!body || body_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (max_count < 1) {
        max_count = 1;
    } else if (max_count > HEADLINE_MAX_COUNT) {
        max_count = HEADLINE_MAX_COUNT;
    }

    const char *pos = body;
    const char *body_end = body + body_len;
    while (out->count < max_count && pos < body_end) {
        const char *which = NULL;
        const char *block_start = find_first_of(pos, "<item", "<entry", &which);
        if (!block_start) {
            break;
        }
        bool is_atom = (which == NULL) ? false : (strcmp(which, "<entry") == 0);
        const char *close_tag = is_atom ? "</entry>" : "</item>";
        const char *block_end = strstr(block_start, close_tag);
        if (!block_end || block_end > body_end) {
            break;  // incomplete trailing block (e.g. response was truncated) - stop
        }

        char title[HEADLINE_MAX_LEN];
        if (extract_title(block_start, block_end, title, sizeof(title))) {
            strncpy(out->titles[out->count], title, HEADLINE_MAX_LEN - 1);
            out->titles[out->count][HEADLINE_MAX_LEN - 1] = '\0';
            out->count++;
        }

        pos = block_end + strlen(close_tag);
    }

    if (out->count == 0) {
        ESP_LOGW(TAG, "No headlines extracted from feed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t headlines_fetch(const char *feed_url, int max_count, headlines_result_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (!feed_url || feed_url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    char *body = NULL;
    size_t body_len = 0;
    bool truncated = false;
    esp_err_t err = http_fetch_get(feed_url, HEADLINES_HTTP_TIMEOUT_MS, HEADLINES_MAX_RESPONSE_BYTES,
                                   &body, &body_len, &truncated, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Feed request failed: %s", esp_err_to_name(err));
        return err;
    }
    if (truncated) {
        ESP_LOGW(TAG, "Feed response truncated at %d bytes - parsing what was captured",
                 HEADLINES_MAX_RESPONSE_BYTES);
    }

    err = headlines_extract(body, body_len, max_count, out);
    free(body);
    return err;
}
