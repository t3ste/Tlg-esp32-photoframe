#include "todo.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "http_fetch.h"
#include "image_processor.h"

static const char *TAG = "todo";

#define TODO_HTTP_TIMEOUT_MS 10000
#define TODO_MAX_RESPONSE_BYTES (32 * 1024)
// Generous scratch size for a raw (pre-ASCII-sanitize) line - a todo.txt
// line is conventionally short, but a pathological one shouldn't overrun
// anything.
#define RAW_LINE_BUF_LEN 512

// True if `pos` (with `remaining` bytes left in the line) starts with a
// "YYYY-MM-DD" token immediately followed by a space or end-of-line - the
// todo.txt creation/completion date shape. Doesn't validate the date is a
// real calendar date (e.g. month 13 passes) - callers only use this to
// decide whether to skip the token, not to interpret it.
static bool is_date_token(const char *pos, size_t remaining)
{
    if (remaining < 10) {
        return false;
    }
    for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) {
            if (pos[i] != '-') {
                return false;
            }
        } else if (!isdigit((unsigned char) pos[i])) {
            return false;
        }
    }
    return remaining == 10 || pos[10] == ' ';
}

// Appends `word` (word_len bytes, not NUL-terminated) to `out` with a
// single separating space if `out` is already non-empty, truncating
// safely rather than overflowing.
static void append_word(char *out, size_t out_len, const char *word, size_t word_len)
{
    size_t used = strlen(out);
    if (used > 0 && used + 1 < out_len) {
        out[used++] = ' ';
        out[used] = '\0';
    }
    size_t avail = (used < out_len) ? out_len - used - 1 : 0;
    if (word_len > avail) {
        word_len = avail;
    }
    memcpy(out + used, word, word_len);
    out[used + word_len] = '\0';
}

// Parses one already-trimmed (no trailing \r/\n) todo.txt line into
// `item`. Returns false if the line is a completed task ("x " prefix) or
// blank - both are excluded from the rendered list - true otherwise
// (always fills `item`, even for a garbage line: worst case its whole
// text ends up as one plain-text token).
static bool parse_todo_line(const char *line, size_t line_len, todo_item_t *item)
{
    memset(item, 0, sizeof(*item));

    // Skip incidental leading whitespace some editors/exports add.
    while (line_len > 0 && (*line == ' ' || *line == '\t')) {
        line++;
        line_len--;
    }
    if (line_len == 0) {
        return false;  // blank line
    }

    // Completed task marker: lowercase 'x' immediately followed by a
    // space, at the very start of the (trimmed) line. Ambiguous with a
    // genuine task starting "x " by coincidence, but this is the
    // standard todo.txt convention every client follows.
    if (line[0] == 'x' && line_len > 1 && line[1] == ' ') {
        return false;
    }

    const char *pos = line;
    size_t remaining = line_len;

    // Priority: "(A) " through "(Z) ", must be at the very start.
    if (remaining >= 4 && pos[0] == '(' && isupper((unsigned char) pos[1]) && pos[2] == ')' &&
        pos[3] == ' ') {
        item->priority = pos[1];
        pos += 4;
        remaining -= 4;
    }

    // Creation date, if present - not currently surfaced to the display,
    // just skipped so it doesn't end up as a stray word in the text.
    if (is_date_token(pos, remaining)) {
        size_t consumed = (remaining == 10) ? 10 : 11;  // include the trailing space, if any
        pos += consumed;
        remaining -= consumed;
    }

    // Tokenize the rest on spaces; classify each token as a +project,
    // @context, "due:"-metadata, or plain text (joined into item->text).
    while (remaining > 0) {
        while (remaining > 0 && *pos == ' ') {
            pos++;
            remaining--;
        }
        if (remaining == 0) {
            break;
        }
        const char *tok_start = pos;
        size_t tok_len = 0;
        while (tok_len < remaining && tok_start[tok_len] != ' ') {
            tok_len++;
        }
        pos += tok_len;
        remaining -= tok_len;

        if (tok_len > 1 && tok_start[0] == '+') {
            if (item->project_count < TODO_TAG_MAX) {
                size_t copy_len = tok_len - 1;
                if (copy_len > TODO_TAG_MAX_LEN - 1) {
                    copy_len = TODO_TAG_MAX_LEN - 1;
                }
                memcpy(item->projects[item->project_count], tok_start + 1, copy_len);
                item->projects[item->project_count][copy_len] = '\0';
                item->project_count++;
            }
            continue;
        }
        if (tok_len > 1 && tok_start[0] == '@') {
            if (item->context_count < TODO_TAG_MAX) {
                size_t copy_len = tok_len - 1;
                if (copy_len > TODO_TAG_MAX_LEN - 1) {
                    copy_len = TODO_TAG_MAX_LEN - 1;
                }
                memcpy(item->contexts[item->context_count], tok_start + 1, copy_len);
                item->contexts[item->context_count][copy_len] = '\0';
                item->context_count++;
            }
            continue;
        }
        // key:value metadata - both sides non-whitespace, non-colon. Only
        // "due" is captured; any other recognized key:value token is
        // still excluded from the displayed text (it's metadata, not
        // prose), matching todo.txt's own convention that key:value
        // pairs aren't meant to be read literally.
        const char *colon = memchr(tok_start, ':', tok_len);
        if (colon && colon > tok_start && colon < tok_start + tok_len - 1) {
            size_t key_len = (size_t) (colon - tok_start);
            if (key_len == 3 && strncmp(tok_start, "due", 3) == 0) {
                size_t val_len = tok_len - key_len - 1;
                if (val_len > sizeof(item->due_date) - 1) {
                    val_len = sizeof(item->due_date) - 1;
                }
                memcpy(item->due_date, colon + 1, val_len);
                item->due_date[val_len] = '\0';
            }
            continue;
        }

        append_word(item->text, sizeof(item->text), tok_start, tok_len);
    }

    return true;
}

esp_err_t todo_parse(const char *body, size_t body_len, todo_list_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (!body || body_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *pos = body;
    const char *body_end = body + body_len;
    while (pos < body_end && out->count < TODO_MAX_ITEMS) {
        const char *nl = memchr(pos, '\n', (size_t) (body_end - pos));
        const char *line_end = nl ? nl : body_end;
        size_t raw_len = (size_t) (line_end - pos);
        if (raw_len > 0 && pos[raw_len - 1] == '\r') {
            raw_len--;  // tolerate CRLF
        }

        if (raw_len > 0) {
            char raw[RAW_LINE_BUF_LEN];
            size_t copy_len = (raw_len < sizeof(raw) - 1) ? raw_len : sizeof(raw) - 1;
            memcpy(raw, pos, copy_len);
            raw[copy_len] = '\0';

            char ascii[RAW_LINE_BUF_LEN];
            image_processor_sanitize_ascii(raw, ascii, sizeof(ascii));

            todo_item_t item;
            if (parse_todo_line(ascii, strlen(ascii), &item)) {
                out->items[out->count++] = item;
            }
        }

        pos = nl ? nl + 1 : body_end;
    }

    return ESP_OK;  // an empty (all-completed or blank) file is not an error
}

esp_err_t todo_fetch(const char *url, int timeout_ms, todo_list_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (!url || url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    char *body = NULL;
    size_t body_len = 0;
    bool truncated = false;
    esp_err_t err = http_fetch_get(url, timeout_ms > 0 ? timeout_ms : TODO_HTTP_TIMEOUT_MS,
                                   TODO_MAX_RESPONSE_BYTES, &body, &body_len, &truncated, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ToDo fetch failed: %s", esp_err_to_name(err));
        return err;
    }
    if (truncated) {
        ESP_LOGW(TAG, "ToDo response truncated at %d bytes - parsing what was captured",
                 TODO_MAX_RESPONSE_BYTES);
    }

    err = todo_parse(body, body_len, out);
    free(body);
    return err;
}
