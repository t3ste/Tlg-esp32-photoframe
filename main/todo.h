#ifndef TODO_H
#define TODO_H

#include <stddef.h>

#include "esp_err.h"

#define TODO_MAX_ITEMS 24
#define TODO_LINE_MAX_LEN 200
#define TODO_TAG_MAX 8
#define TODO_TAG_MAX_LEN 32

typedef struct {
    char priority;                 // 'A'-'Z', or 0 if none
    char text[TODO_LINE_MAX_LEN];  // description, with priority/date/tags/due: stripped
    char due_date[11];             // "YYYY-MM-DD", or "" if none
    char projects[TODO_TAG_MAX][TODO_TAG_MAX_LEN];
    int project_count;
    char contexts[TODO_TAG_MAX][TODO_TAG_MAX_LEN];
    int context_count;
} todo_item_t;

typedef struct {
    int count;
    todo_item_t items[TODO_MAX_ITEMS];
} todo_list_t;

/**
 * @brief Fetches `url` (a plain todo.txt file - no API key, no rate limit)
 * and extracts up to TODO_MAX_ITEMS incomplete tasks, in file order.
 *
 * Uses a lightweight, purpose-built line parser - NOT a full todo.txt
 * client: completed tasks (lines starting with "x ") are excluded
 * entirely, since this is a read-only display feed, not an editor. A
 * malformed line (missing closing paren on a priority marker, a garbage
 * date, etc.) degrades to "whole line is plain text" rather than being
 * dropped or aborting the fetch, matching this project's fail-soft
 * philosophy elsewhere (see weather.c/headlines.c).
 *
 * If `cache_path` is non-NULL, this is a conditional GET: `etag_in` (may be
 * NULL/empty) is sent as If-None-Match, and on a 304 reply the body cached
 * at `cache_path` from the last successful 200 is re-parsed instead of
 * re-downloading (the parse itself is still redone every call - due-today
 * coloring etc. depends on the current date, not just the source content).
 * `etag_out`/`etag_out_len` receive the validator to persist for next time
 * (already carries forward `etag_in` if this response didn't repeat an
 * ETag) - the caller owns actually persisting it (see config_manager.h's
 * agenda ETag getters/setters). Pass cache_path/etag_in/etag_out as NULL to
 * skip conditional-GET entirely and always fetch unconditionally.
 *
 * Best-effort: a fetch failure returns an error and leaves *out zeroed
 * (count = 0).
 */
esp_err_t todo_fetch(const char *url, int timeout_ms, const char *cache_path,
                     const char *etag_in, char *etag_out, size_t etag_out_len, todo_list_t *out);

/**
 * @brief Pure parsing logic behind todo_fetch(), split out so it's
 * host-testable without a real HTTP fetch: parses `body` (a todo.txt file
 * already in memory, `body_len` bytes) directly. See todo_fetch() for the
 * parsing rules.
 */
esp_err_t todo_parse(const char *body, size_t body_len, todo_list_t *out);

#endif
