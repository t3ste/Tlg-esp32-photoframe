#ifndef WEATHER_H
#define WEATHER_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

// Used by the plain photo-overlay weather line (weather_format_line()/
// weather_format_day_lines() below) - unaffected by Agenda grid mode's
// larger request, see WEATHER_FORECAST_DAYS_CAP.
#define WEATHER_FORECAST_DAYS 3

// Capacity of weather_forecast_t.days[] - the most days ANY caller can ever
// request via weather_fetch_forecast()'s max_days param (currently the
// Agenda 7-day grid layout, agenda_manager.c). Kept distinct from
// WEATHER_FORECAST_DAYS so raising it doesn't change the plain photo
// overlay's own 3-day formatting/line-count (weather_format_day_lines()'s
// array size, overlay_manager.c's OVERLAY_LINES_CAP) - only Agenda's grid
// mode ever asks for more than WEATHER_FORECAST_DAYS.
#define WEATHER_FORECAST_DAYS_CAP 7

typedef struct {
    char date[11];  // "YYYY-MM-DD"
    float temp_max_c;
    float temp_min_c;
    int weather_code;  // WMO code, see https://open-meteo.com/en/docs
} weather_day_t;

typedef struct {
    bool valid;
    int count;  // number of populated entries in days[], up to WEATHER_FORECAST_DAYS_CAP
    weather_day_t days[WEATHER_FORECAST_DAYS_CAP];
} weather_forecast_t;

/**
 * @brief Fetches up to `max_days` (clamped to [1, WEATHER_FORECAST_DAYS_CAP])
 * of daily min/max temperature + condition for the configured location, from
 * whichever provider config_manager_get_weather_provider() selects -
 * Open-Meteo (default), wttr.in, or yr.no (MET Norway). All three are
 * free/keyless; wttr.in and yr.no exist as user-selectable alternatives if
 * Open-Meteo isn't reachable/reliable for a given network/region - there is
 * no automatic runtime failover between them. Each provider's own condition
 * code/text is approximated into the same WMO-code vocabulary
 * condition_text() understands, so callers/formatting are provider-agnostic.
 * Open-Meteo's response uses "timezone=auto" (resolves the correct local
 * timezone from the coordinates server-side); yr.no has no such parameter
 * and buckets by UTC calendar date instead (see fetch_yrno() in weather.c
 * for the specific trade-offs that implies). wttr.in's free `j1` format may
 * not actually be able to return more than 3 days regardless of `max_days`
 * - see fetch_wttrin() in weather.c.
 *
 * Resolution order: if both config_manager_get_weather_lat()/_lon() are set,
 * uses them directly. Otherwise, geocodes config_manager_get_weather_location_name()
 * via Open-Meteo's free geocoding endpoint (used for geocoding regardless of
 * the selected forecast provider) - but only when that name differs
 * from config_manager_get_weather_geocoded_name() (the cached result of the
 * last successful geocode); a match reuses the cached lat/lon, so geocoding
 * only costs a request once per location-name change, not every call.
 *
 * On success, also records the provider used via
 * config_manager_set_weather_last_source() - lets /status (telegram_bot.c)
 * report which service actually produced the currently-displayed data,
 * distinct from config_manager_get_weather_provider() (the configured
 * preference, which may not match if the last attempt against it failed).
 *
 * Best-effort: any network/parse failure returns an error and leaves *out
 * zeroed (valid = false) - callers should treat this as "no weather this
 * cycle", never fatal to the caller's own flow.
 */
esp_err_t weather_fetch_forecast(weather_forecast_t *out, int max_days);

/**
 * @brief Formats one summary line, e.g.
 * "Wed sunny 16/24 | Thu partly cloudy 17/25 | Fri rain -5/3" (temperatures
 * as min/max, rounded), using config_manager_get_overlay_language() ("en" or
 * "de") for both the weekday abbreviation and the condition text. No-op
 * (empty string) if !f->valid.
 *
 * Note: even after abbreviating condition text as far as reasonably
 * legible, worst-case combinations (long condition word + 3-digit negative
 * temperatures on all 3 days) can still exceed a typical ~46-character
 * display line - see weather_format_day_lines() for a per-day, 3-line
 * layout that comfortably fits every combination instead.
 */
void weather_format_line(const weather_forecast_t *f, char *out, size_t out_len);

#define WEATHER_DAY_LINE_MAX_LEN 40

/**
 * @brief Same content as weather_format_line(), but as one independent line
 * per day (e.g. out_lines[0] = "Wed sunny 16/24") instead of one joined
 * line - reliably fits any single day's worst-case combination within a
 * ~46-character display line, unlike the combined 3-day line.
 * `*out_count` is set to f->count (0 if !f->valid).
 */
void weather_format_day_lines(const weather_forecast_t *f,
                              char out_lines[WEATHER_FORECAST_DAYS][WEATHER_DAY_LINE_MAX_LEN],
                              int *out_count);

/**
 * @brief Short weekday abbreviation (e.g. "Fri"/"Fr"), English/German - the
 * same table weather_format_line()/weather_format_day_lines() use
 * internally, exposed so a caller that needs just the weekday word for an
 * already-known date (agenda_renderer.c's Calendar day-divider label)
 * doesn't need its own copy. English abbreviations are 3 letters, German 2
 * (an existing asymmetry in this table, not introduced here). Unknown
 * `wday` (outside 0-6) returns "?".
 */
const char *weather_weekday_abbr(int wday, bool german);

/**
 * @brief Short condition text for one WMO weather code (e.g. "cloudy"/
 * "bedeckt"), the same abbreviated vocabulary weather_format_line()/
 * weather_format_day_lines() use internally - exposed so a caller that
 * wants only the condition word for one already-fetched day (agenda_renderer.c's
 * Calendar day-divider weather annotation) doesn't need its own copy of the
 * WMO-code table. Unmapped codes return "unknown"/"unbekannt".
 */
const char *weather_condition_text(int code, bool german);

// Reserved control-byte range embedded directly inside the lines
// weather_format_line()/weather_format_day_lines() build (in place of the
// condition word) when weather icon mode is active
// (config_manager_get_weather_icon_set() != "none"). These values are
// outside draw_glyph()'s printable-ASCII range (0x20-0x7E) and confirmed to
// survive image_processor.c's sanitize_caption_ascii() unchanged (it passes
// bytes < 0x80 through as-is) - render_text_bar()/
// image_processor_draw_overlay_bar() recognize a byte in this range and draw
// the matching icon bitmap (main/weather_icons_data.h) instead of a font
// glyph.
#define WEATHER_ICON_MARKER_BASE 0x01

/**
 * @brief Maps a WMO weather code to an icon id (see main/weather_icons_data.h
 * for the actual bitmaps, indexed identically for both selectable icon
 * sets), or -1 if unmapped - callers fall back to weather_condition_text()
 * for that day in that case. Table mirrors weather_condition_text()'s WMO
 * vocabulary but with full coverage of every code this project ever
 * produces, including a few (56/57/66/67/77) that fall through to "unknown"
 * as text but do have a dedicated icon.
 */
int weather_code_to_icon_id(int code);

#endif
