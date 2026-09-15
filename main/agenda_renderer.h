#ifndef AGENDA_RENDERER_H
#define AGENDA_RENDERER_H

#include "calendar_ics.h"
#include "climate.h"
#include "esp_err.h"
#include "image_processor.h"
#include "todo.h"
#include "weather.h"

// Optional climate readout for both column headers (ToDo and Calendar
// alike, unlike cal_weather below which is Calendar-only) - see
// config_manager_get_climate_agenda_header_enabled(). Pre-formatted text
// (e.g. "21C"/"48%"), same reasoning as the photo-overlay climate badges:
// agenda_renderer.c stays unit-agnostic. Each channel is independently
// optional (has_temp/has_hum), same as the overlay badges.
typedef struct {
    bool has_temp;
    char temp_text[8];
    climate_category_t temp_category;
    bool has_hum;
    char hum_text[8];
    climate_category_t hum_category;
} agenda_climate_t;

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
 * `events_c`/`events_d`/`events_e` are three additional, independently
 * enabled ICS sources (e.g. holidays/school-holidays/other special-days
 * feeds) - unlike A/B they are never auto-refreshed, only fetched/uploaded
 * on an explicit user action (see agenda_manager.c), but they merge into
 * the exact same day-grouped, sorted list as A/B and follow the same
 * multi-day display mode. May be NULL/empty like A/B.
 *
 * Color-coded per docs/AGENDA_COLORS.html. ToDo rows color priority,
 * +project/@context tags, and due-date urgency independently within one
 * row. Calendar rows are colored by origin (events_a/b/c/d/e) via
 * calendar_source_color() - plain colored text (blue for A, green for B,
 * red for C, yellow for D, red for E), no background chip, on any page
 * background. The shared background
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
 * @param climate Optional (NULL if the opt-in
 * config_manager_get_climate_agenda_header_enabled() setting is off, or
 * both sensor reads failed) - when present, a small right-aligned readout
 * is drawn into BOTH column headers (ToDo and Calendar alike), in the free
 * space after the existing header text.
 * @return ESP_ERR_INVALID_ARG on bad arguments, ESP_ERR_INVALID_STATE if
 * `todo`, `events_a`, and `events_b` are all NULL/empty (nothing to render -
 * callers should check this first rather than relying on it),
 * ESP_ERR_NO_MEM if the canvas buffer can't be allocated, otherwise
 * whatever image_processor_write_rgb_to_fmt() returns.
 */
esp_err_t agenda_renderer_render(const todo_list_t *todo, const ics_event_list_t *events_a,
                                 const ics_event_list_t *events_b, const ics_event_list_t *events_c,
                                 const ics_event_list_t *events_d, const ics_event_list_t *events_e,
                                 const weather_forecast_t *cal_weather, int lookahead_days,
                                 const char *output_path, image_format_t out_format,
                                 const agenda_climate_t *climate);

#endif
