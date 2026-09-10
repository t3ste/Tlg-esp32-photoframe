#ifndef AGENDA_RENDERER_H
#define AGENDA_RENDERER_H

#include "calendar_ics.h"
#include "esp_err.h"
#include "image_processor.h"
#include "todo.h"

/**
 * @brief Renders ToDo and/or Calendar content as a full-screen grid - no
 * source photo at all, unlike every other display path in this firmware.
 * Exactly one of `todo`/`events` may be NULL/empty and the other still
 * renders as a single full-width column; if both have content, landscape
 * boards show them side by side and portrait boards stack them, mirroring
 * image_processor_compose_pair_to_rgb()'s own orientation convention.
 *
 * Rows are color-coded (Spectra6 hue on color boards, a distinct gray
 * level on grayscale boards): overdue/priority-A ToDo items and a
 * happening-today Calendar event are most prominent, a future due date or
 * a +project tag is a secondary accent, everything else is plain text.
 * This is a whole-row color, not per-token - see docs/AGENDA.md for the
 * simplification rationale.
 *
 * @param lookahead_days Only used for the Calendar column's header label
 * (the actual event filtering already happened when `events` was fetched).
 * @return ESP_ERR_INVALID_ARG on bad arguments, ESP_ERR_INVALID_STATE if
 * both `todo` and `events` are NULL/empty (nothing to render - callers
 * should check this first rather than relying on it), ESP_ERR_NO_MEM if
 * the canvas buffer can't be allocated, otherwise whatever
 * image_processor_write_rgb_to_fmt() returns.
 */
esp_err_t agenda_renderer_render(const todo_list_t *todo, const ics_event_list_t *events,
                                 int lookahead_days, const char *output_path,
                                 image_format_t out_format);

#endif
