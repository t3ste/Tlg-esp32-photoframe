#include "agenda_renderer.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "board_hal.h"
#include "config_manager.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "agenda_renderer";

// Generous enough for a ToDo line (priority + text + due date suffix) or a
// Calendar line (date/time prefix + summary) after image_processor_wrap_text()
// has already truncated the underlying text to fit the column width.
#define AGENDA_ROW_BUF_LEN 200
#define AGENDA_PADDING 4

typedef struct {
    int x, y, w, h;
} agenda_rect_t;

typedef enum {
    AGENDA_COLOR_DEFAULT,
    AGENDA_COLOR_URGENT,
    AGENDA_COLOR_WARNING,
    AGENDA_COLOR_INFO,
} agenda_color_role_t;

// Mirrors wants_portrait_frame() (display_manager.c) / wants_portrait_frame_now()
// (telegram_bot.c) - kept as a third small duplicate rather than a
// cross-module dependency, matching this codebase's own stated preference
// for that trade-off (see either of those functions' own comment).
static bool agenda_wants_portrait_frame(void)
{
    int rot = config_manager_get_display_rotation_deg() % 360;
    if (rot < 0) {
        rot += 360;
    }
    return (rot == 90 || rot == 270);
}

// Mirrors board_is_grayscale() (image_processor.c, file-private there) -
// same one-line duplication convention as agenda_wants_portrait_frame() above.
static bool agenda_board_is_grayscale(void)
{
    return strncmp(BOARD_HAL_DISPLAY_TYPE, "gc", 2) == 0;
}

// Translates a semantic role into an exact palette color: a Spectra6 hue on
// color boards (matching the flat, undithered values every other overlay
// draw function in image_processor.c already writes), or a distinct
// gray_theoretical[] level (image_processor.c) on grayscale boards - chosen
// to avoid the pure-black/white already used for body text/background so
// every role stays visually distinct there too.
static void agenda_role_color(agenda_color_role_t role, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (agenda_board_is_grayscale()) {
        static const uint8_t gray_level[4] = {0, 2, 8, 11};  // DEFAULT, URGENT, WARNING, INFO
        uint8_t v = (uint8_t) (gray_level[role] * 17);
        *r = *g = *b = v;
        return;
    }
    switch (role) {
    case AGENDA_COLOR_URGENT:
        *r = 255;
        *g = 0;
        *b = 0;  // red
        break;
    case AGENDA_COLOR_WARNING:
        *r = 255;
        *g = 255;
        *b = 0;  // yellow
        break;
    case AGENDA_COLOR_INFO:
        *r = 0;
        *g = 0;
        *b = 255;  // blue
        break;
    default:
        *r = 0;
        *g = 0;
        *b = 0;  // black
        break;
    }
}

// Overdue or priority-A is most prominent; due-today or priority-B is next;
// a future due date or a +project tag is a lighter accent; everything else
// is plain default-colored text. One role per row (not per-token) - see
// agenda_renderer.h's doc comment on that simplification.
static agenda_color_role_t todo_item_role(const todo_item_t *item, time_t now)
{
    bool overdue = false, due_today = false, due_future = false;
    if (item->due_date[0] != '\0') {
        struct tm now_tm;
        localtime_r(&now, &now_tm);
        // Oversized vs. the exact "YYYY-MM-DD" (10 chars) it normally
        // holds - silences -Wformat-truncation, which (correctly) can't
        // prove tm_year+1900 always fits in 4 digits from this call site
        // alone. Only the first 10 chars are ever compared below.
        char today[32];
        snprintf(today, sizeof(today), "%04d-%02d-%02d", now_tm.tm_year + 1900, now_tm.tm_mon + 1,
                 now_tm.tm_mday);
        int cmp = strncmp(item->due_date, today, 10);
        overdue = cmp < 0;
        due_today = (cmp == 0);
        due_future = cmp > 0;
    }
    if (overdue || item->priority == 'A') {
        return AGENDA_COLOR_URGENT;
    }
    if (due_today || item->priority == 'B') {
        return AGENDA_COLOR_WARNING;
    }
    if (due_future || item->project_count > 0) {
        return AGENDA_COLOR_INFO;
    }
    return AGENDA_COLOR_DEFAULT;
}

// An event happening today is highlighted; a later day within the
// lookahead window is a lighter accent.
static agenda_color_role_t event_role(const ics_event_t *ev, time_t now)
{
    struct tm now_tm, ev_tm;
    localtime_r(&now, &now_tm);
    localtime_r(&ev->start, &ev_tm);
    bool same_day = (now_tm.tm_year == ev_tm.tm_year && now_tm.tm_yday == ev_tm.tm_yday);
    return same_day ? AGENDA_COLOR_WARNING : AGENDA_COLOR_INFO;
}

static void format_todo_line(const todo_item_t *item, char *out, size_t out_len)
{
    int n = 0;
    if (item->priority != 0) {
        n += snprintf(out + n, out_len - (size_t) n, "[%c] ", item->priority);
    }
    if (n < 0 || (size_t) n >= out_len) {
        return;
    }
    n += snprintf(out + n, out_len - (size_t) n, "%s", item->text);
    if (n > 0 && (size_t) n < out_len && item->due_date[0] != '\0') {
        snprintf(out + n, out_len - (size_t) n, " (due %s)", item->due_date);
    }
}

static void format_event_line(const ics_event_t *ev, char *out, size_t out_len)
{
    struct tm start_tm;
    localtime_r(&ev->start, &start_tm);
    if (ev->all_day) {
        snprintf(out, out_len, "%02d.%02d %s", start_tm.tm_mday, start_tm.tm_mon + 1, ev->summary);
    } else {
        snprintf(out, out_len, "%02d.%02d %02d:%02d %s", start_tm.tm_mday, start_tm.tm_mon + 1,
                 start_tm.tm_hour, start_tm.tm_min, ev->summary);
    }
}

// Draws one column: a solid black header bar (title, white text), then one
// truncated-to-fit line per entry in its own role color, stopping once the
// column runs out of vertical room - with a trailing "+N more" line if any
// entries had to be omitted, rather than silently dropping them (same
// fail-soft, best-effort spirit as the rest of the overlay system).
static void draw_column(uint8_t *rgb, int width, int height, agenda_rect_t rect, const char *title,
                        const char *const *lines, const agenda_color_role_t *roles, int line_count)
{
    int header_h = IMAGE_PROCESSOR_FONT_HEIGHT + 2 * AGENDA_PADDING;
    image_processor_fill_rect(rgb, width, height, rect.x, rect.y, rect.w, header_h, 0, 0, 0);
    image_processor_draw_text(rgb, width, height, rect.x + AGENDA_PADDING, rect.y + AGENDA_PADDING,
                              title, 255, 255, 255);

    int row_h = IMAGE_PROCESSOR_FONT_HEIGHT + AGENDA_PADDING;
    int content_top = rect.y + header_h + AGENDA_PADDING;
    int content_h = rect.h - header_h - AGENDA_PADDING;
    int max_rows = (content_h > 0) ? content_h / row_h : 0;

    int rows_drawn = 0;
    int text_width = rect.w - 2 * AGENDA_PADDING;
    int budget = (line_count > max_rows) ? max_rows - 1 : max_rows;  // reserve a row for "+N more"
    if (budget < 0) {
        budget = 0;
    }
    for (int i = 0; i < line_count && rows_drawn < budget; i++) {
        char wrapped[1][OVERLAY_LINE_MAX_CHARS];
        int wrapped_count = image_processor_wrap_text(lines[i], text_width, 1, wrapped);
        if (wrapped_count <= 0) {
            continue;
        }
        uint8_t r, g, b;
        agenda_role_color(roles[i], &r, &g, &b);
        int y = content_top + rows_drawn * row_h;
        image_processor_draw_text(rgb, width, height, rect.x + AGENDA_PADDING, y, wrapped[0], r, g,
                                  b);
        rows_drawn++;
    }
    if (line_count > rows_drawn && rows_drawn < max_rows) {
        char more[32];
        snprintf(more, sizeof(more), "+%d more", line_count - rows_drawn);
        int y = content_top + rows_drawn * row_h;
        image_processor_draw_text(rgb, width, height, rect.x + AGENDA_PADDING, y, more, 0, 0, 0);
    }
}

esp_err_t agenda_renderer_render(const todo_list_t *todo, const ics_event_list_t *events,
                                 int lookahead_days, const char *output_path,
                                 image_format_t out_format)
{
    if (!output_path) {
        return ESP_ERR_INVALID_ARG;
    }
    bool show_todo = todo && todo->count > 0;
    bool show_cal = events && events->count > 0;
    if (!show_todo && !show_cal) {
        return ESP_ERR_INVALID_STATE;
    }

    int width = BOARD_HAL_DISPLAY_WIDTH;
    int height = BOARD_HAL_DISPLAY_HEIGHT;
    size_t buf_size = (size_t) width * (size_t) height * 3;
    uint8_t *rgb = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!rgb) {
        ESP_LOGE(TAG, "Failed to allocate %zu-byte agenda canvas", buf_size);
        return ESP_ERR_NO_MEM;
    }
    image_processor_fill_rect(rgb, width, height, 0, 0, width, height, 255, 255, 255);

    bool both = show_todo && show_cal;
    agenda_rect_t todo_rect, cal_rect;
    if (!both) {
        agenda_rect_t full = {0, 0, width, height};
        todo_rect = full;
        cal_rect = full;
    } else if (agenda_wants_portrait_frame()) {
        todo_rect = (agenda_rect_t){0, 0, width, height / 2};
        cal_rect = (agenda_rect_t){0, height / 2, width, height - height / 2};
        image_processor_fill_rect(rgb, width, height, 0, height / 2 - 1, width, 2, 0, 0, 0);
    } else {
        todo_rect = (agenda_rect_t){0, 0, width / 2, height};
        cal_rect = (agenda_rect_t){width / 2, 0, width - width / 2, height};
        image_processor_fill_rect(rgb, width, height, width / 2 - 1, 0, 2, height, 0, 0, 0);
    }

    time_t now = time(NULL);

    if (show_todo) {
        char *formatted =
            heap_caps_malloc((size_t) todo->count * AGENDA_ROW_BUF_LEN, MALLOC_CAP_SPIRAM);
        const char **lines =
            heap_caps_malloc((size_t) todo->count * sizeof(char *), MALLOC_CAP_SPIRAM);
        agenda_color_role_t *roles =
            heap_caps_malloc((size_t) todo->count * sizeof(agenda_color_role_t), MALLOC_CAP_SPIRAM);
        if (formatted && lines && roles) {
            for (int i = 0; i < todo->count; i++) {
                char *slot = formatted + (size_t) i * AGENDA_ROW_BUF_LEN;
                format_todo_line(&todo->items[i], slot, AGENDA_ROW_BUF_LEN);
                lines[i] = slot;
                roles[i] = todo_item_role(&todo->items[i], now);
            }
            draw_column(rgb, width, height, todo_rect, "TODO", lines, roles, todo->count);
        } else {
            ESP_LOGW(TAG, "Failed to allocate ToDo render scratch buffers - skipping ToDo column");
        }
        heap_caps_free(formatted);
        heap_caps_free(lines);
        heap_caps_free(roles);
    }

    if (show_cal) {
        char *formatted =
            heap_caps_malloc((size_t) events->count * AGENDA_ROW_BUF_LEN, MALLOC_CAP_SPIRAM);
        const char **lines =
            heap_caps_malloc((size_t) events->count * sizeof(char *), MALLOC_CAP_SPIRAM);
        agenda_color_role_t *roles = heap_caps_malloc(
            (size_t) events->count * sizeof(agenda_color_role_t), MALLOC_CAP_SPIRAM);
        if (formatted && lines && roles) {
            for (int i = 0; i < events->count; i++) {
                char *slot = formatted + (size_t) i * AGENDA_ROW_BUF_LEN;
                format_event_line(&events->events[i], slot, AGENDA_ROW_BUF_LEN);
                lines[i] = slot;
                roles[i] = event_role(&events->events[i], now);
            }
            char header[32];
            snprintf(header, sizeof(header), "CALENDAR (%d DAY%s)", lookahead_days,
                     lookahead_days == 1 ? "" : "S");
            draw_column(rgb, width, height, cal_rect, header, lines, roles, events->count);
        } else {
            ESP_LOGW(TAG, "Failed to allocate Calendar render scratch buffers - skipping column");
        }
        heap_caps_free(formatted);
        heap_caps_free(lines);
        heap_caps_free(roles);
    }

    image_format_t actual_format = out_format;
    esp_err_t err = image_processor_write_rgb_to_fmt(rgb, width, height, output_path, out_format,
                                                     &actual_format);
    heap_caps_free(rgb);
    return err;
}
