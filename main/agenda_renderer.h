#ifndef AGENDA_RENDERER_H
#define AGENDA_RENDERER_H

#include "calendar_ics.h"
#include "esp_err.h"
#include "image_processor.h"
#include "todo.h"
#include "weather.h"

/**
 * @brief Renders ToDo and/or Calendar content as a full-screen grid - no
 * source photo at all, unlike every other display path in this firmware.
 * `events_a`/`events_b` are two independent calendar sources (e.g. work vs.
 * personal) merged into one day-grouped list, sorted by start time; either
 * (or both) may be NULL/empty. `todo` may also be NULL/empty - whichever of
 * ToDo/Calendar has content renders as a single full-width column if the
 * other doesn't; if both do, the layout follows
 * config_manager_get_agenda_stack_layout() (landscape only - portrait
 * always stacks, mirroring image_processor_compose_pair_to_rgb()'s own
 * orientation convention).
 *
 * Color-coded per docs/AGENDA_COLORS.html. ToDo rows color priority,
 * +project/@context tags, and due-date urgency independently within one
 * row. Calendar rows are colored by origin (events_a vs. events_b) via
 * calendar_source_color() - plain colored text (blue for A, green for B),
 * no background chip, on any page background. The shared background
 * (config_manager_get_agenda_bg_color()) and its automatic
 * collision-avoidance fallback (agenda_avoid_bg_collision()) apply to
 * every plain (non-chip) text color in both columns, including the day
 * divider and both column headers, which invert (light chip average shown
 * dark and vice versa) rather than staying black-fixed like other chips
 * that draw their own always-black/white fill.
 *
 * @param cal_weather Optional (NULL if the opt-in
 * config_manager_get_agenda_cal_weather_enabled() setting is off, or the
 * fetch failed) - when present, each Calendar day divider whose date has a
 * matching entry in `cal_weather->days[]` gets a " [min/max condition]"
 * suffix appended to its "<weekday> <day>." label (e.g. "Fr 11. [18/25
 * cloudy]"). A day with no matching forecast entry (most commonly the 4th
 * day, since WEATHER_FORECAST_DAYS is 3 but the lookahead window can reach
 * a 4th calendar day late in the evening) simply shows the plain label,
 * same as before this setting existed.
 * @param lookahead_days Only used for the Calendar column's day-window
 * bookkeeping (the actual event filtering already happened when
 * `events_a`/`events_b` were fetched) - no longer shown in the header text.
 * @return ESP_ERR_INVALID_ARG on bad arguments, ESP_ERR_INVALID_STATE if
 * `todo`, `events_a`, and `events_b` are all NULL/empty (nothing to render -
 * callers should check this first rather than relying on it),
 * ESP_ERR_NO_MEM if the canvas buffer can't be allocated, otherwise
 * whatever image_processor_write_rgb_to_fmt() returns.
 */
esp_err_t agenda_renderer_render(const todo_list_t *todo, const ics_event_list_t *events_a,
                                 const ics_event_list_t *events_b,
                                 const weather_forecast_t *cal_weather, int lookahead_days,
                                 const char *output_path, image_format_t out_format);

#endif
