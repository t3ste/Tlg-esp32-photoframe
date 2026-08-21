#ifndef WEATHER_H
#define WEATHER_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define WEATHER_FORECAST_DAYS 3

typedef struct {
    char date[11];  // "YYYY-MM-DD"
    float temp_max_c;
    float temp_min_c;
    int weather_code;  // WMO code, see https://open-meteo.com/en/docs
} weather_day_t;

typedef struct {
    bool valid;
    int count;  // number of populated entries in days[], up to WEATHER_FORECAST_DAYS
    weather_day_t days[WEATHER_FORECAST_DAYS];
} weather_forecast_t;

/**
 * @brief Fetches a WEATHER_FORECAST_DAYS-day forecast (daily min/max
 * temperature + condition) from Open-Meteo (free, no API key) for the
 * configured location. The response's per-day bucketing uses
 * Open-Meteo's "timezone=auto" (resolves the correct local timezone from
 * the coordinates server-side) rather than the device's own configured
 * timezone, which is a POSIX TZ string (e.g. "UTC0") incompatible with
 * Open-Meteo's IANA-style timezone parameter.
 *
 * Resolution order: if both config_manager_get_weather_lat()/_lon() are set,
 * uses them directly. Otherwise, geocodes config_manager_get_weather_location_name()
 * via Open-Meteo's free geocoding endpoint - but only when that name differs
 * from config_manager_get_weather_geocoded_name() (the cached result of the
 * last successful geocode); a match reuses the cached lat/lon, so geocoding
 * only costs a request once per location-name change, not every call.
 *
 * Best-effort: any network/parse failure returns an error and leaves *out
 * zeroed (valid = false) - callers should treat this as "no weather this
 * cycle", never fatal to the caller's own flow.
 */
esp_err_t weather_fetch_forecast(weather_forecast_t *out);

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

#endif
