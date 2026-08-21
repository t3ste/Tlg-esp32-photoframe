#include "weather.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "config.h"
#include "config_manager.h"
#include "esp_log.h"
#include "http_fetch.h"

static const char *TAG = "weather";

#define WEATHER_HTTP_TIMEOUT_MS 10000
#define WEATHER_MAX_RESPONSE_BYTES (8 * 1024)
// Unlike Open-Meteo's compact "?forecast_days=3, daily-only" response, wttr.in's
// j1 format (current condition + nearest_area + 3 full days of 3-hourly
// breakdowns + astronomy) and yr.no's hourly timeseries (no day-count limit -
// the API always returns its full ~9-day forecast) are both much larger.
// Both are one JSON document each - a response truncated at the cap fails to
// parse at all (unlike headlines.c's RSS truncation, which tolerates losing
// trailing items), so these need to comfortably fit the whole body, not just
// the first 3 days' worth.
#define WEATHER_MAX_RESPONSE_BYTES_WTTR (48 * 1024)
#define WEATHER_MAX_RESPONSE_BYTES_YRNO (128 * 1024)

// Minimal percent-encoding, sufficient for a free-text location name
// (city/place names - letters, spaces, commas, occasional accented chars as
// UTF-8 bytes). Unreserved characters pass through; everything else
// (including UTF-8 continuation bytes, which are always >= 0x80) is
// percent-encoded byte-by-byte, which is valid and exactly what a UTF-8
// query parameter should look like on the wire.
static void url_encode(const char *in, char *out, size_t out_len)
{
    static const char *hex = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *) in; *p != '\0' && o + 4 < out_len; p++) {
        unsigned char c = *p;
        bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) {
            out[o++] = (char) c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0x0F];
        }
    }
    out[o] = '\0';
}

// Geocodes the configured location name, caching the result in NVS
// (weather_geocoded_name/lat/lon) so repeat calls with the same name skip
// the network round trip. Returns ESP_OK with *out_lat/*out_lon filled on
// success (either freshly geocoded or served from cache).
static esp_err_t resolve_lat_lon(char *out_lat, size_t out_lat_len, char *out_lon,
                                 size_t out_lon_len)
{
    const char *manual_lat = config_manager_get_weather_lat();
    const char *manual_lon = config_manager_get_weather_lon();
    if (manual_lat[0] != '\0' && manual_lon[0] != '\0') {
        strncpy(out_lat, manual_lat, out_lat_len - 1);
        out_lat[out_lat_len - 1] = '\0';
        strncpy(out_lon, manual_lon, out_lon_len - 1);
        out_lon[out_lon_len - 1] = '\0';
        return ESP_OK;
    }

    const char *location_name = config_manager_get_weather_location_name();
    if (location_name[0] == '\0') {
        ESP_LOGW(TAG, "No location configured (neither lat/lon nor a location name)");
        return ESP_ERR_INVALID_STATE;
    }

    const char *geocoded_name = config_manager_get_weather_geocoded_name();
    const char *cached_lat = config_manager_get_weather_lat();
    const char *cached_lon = config_manager_get_weather_lon();
    if (strcmp(location_name, geocoded_name) == 0 && cached_lat[0] != '\0' && cached_lon[0] != '\0') {
        strncpy(out_lat, cached_lat, out_lat_len - 1);
        out_lat[out_lat_len - 1] = '\0';
        strncpy(out_lon, cached_lon, out_lon_len - 1);
        out_lon[out_lon_len - 1] = '\0';
        return ESP_OK;
    }

    char encoded_name[WEATHER_LOCATION_NAME_MAX_LEN * 3];
    url_encode(location_name, encoded_name, sizeof(encoded_name));

    char url[300];
    snprintf(url, sizeof(url),
             "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=1&language=%s&format=json",
             encoded_name, config_manager_get_overlay_language());

    char *body = NULL;
    size_t body_len = 0;
    esp_err_t err = http_fetch_get(url, WEATHER_HTTP_TIMEOUT_MS, WEATHER_MAX_RESPONSE_BYTES, &body,
                                   &body_len, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Geocoding request failed: %s", esp_err_to_name(err));
        return err;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        ESP_LOGW(TAG, "Geocoding response not valid JSON");
        return ESP_FAIL;
    }

    cJSON *results = cJSON_GetObjectItem(root, "results");
    cJSON *first = (results && cJSON_IsArray(results)) ? cJSON_GetArrayItem(results, 0) : NULL;
    cJSON *lat_item = first ? cJSON_GetObjectItem(first, "latitude") : NULL;
    cJSON *lon_item = first ? cJSON_GetObjectItem(first, "longitude") : NULL;
    if (!lat_item || !lon_item || !cJSON_IsNumber(lat_item) || !cJSON_IsNumber(lon_item)) {
        ESP_LOGW(TAG, "Geocoding found no match for \"%s\"", location_name);
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }

    snprintf(out_lat, out_lat_len, "%.4f", lat_item->valuedouble);
    snprintf(out_lon, out_lon_len, "%.4f", lon_item->valuedouble);
    cJSON_Delete(root);

    // Cache: next call with the same location name skips this round trip.
    config_manager_set_weather_lat(out_lat);
    config_manager_set_weather_lon(out_lon);
    config_manager_set_weather_geocoded_name(location_name);

    ESP_LOGI(TAG, "Geocoded \"%s\" -> %s, %s", location_name, out_lat, out_lon);
    return ESP_OK;
}

static esp_err_t fetch_open_meteo(const char *lat, const char *lon, weather_forecast_t *out)
{
    char url[320];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s"
             "&daily=temperature_2m_max,temperature_2m_min,weather_code"
             "&forecast_days=%d&timezone=auto",
             lat, lon, WEATHER_FORECAST_DAYS);

    char *body = NULL;
    size_t body_len = 0;
    esp_err_t err = http_fetch_get(url, WEATHER_HTTP_TIMEOUT_MS, WEATHER_MAX_RESPONSE_BYTES, &body,
                                   &body_len, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Forecast request failed: %s", esp_err_to_name(err));
        return err;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        ESP_LOGW(TAG, "Forecast response not valid JSON");
        return ESP_FAIL;
    }

    cJSON *daily = cJSON_GetObjectItem(root, "daily");
    cJSON *time_arr = daily ? cJSON_GetObjectItem(daily, "time") : NULL;
    cJSON *tmax_arr = daily ? cJSON_GetObjectItem(daily, "temperature_2m_max") : NULL;
    cJSON *tmin_arr = daily ? cJSON_GetObjectItem(daily, "temperature_2m_min") : NULL;
    cJSON *code_arr = daily ? cJSON_GetObjectItem(daily, "weather_code") : NULL;
    if (!time_arr || !cJSON_IsArray(time_arr) || !tmax_arr || !tmin_arr || !code_arr) {
        ESP_LOGW(TAG, "Forecast response missing daily.time/temperature_2m_max/_min/weather_code");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    int n = cJSON_GetArraySize(time_arr);
    if (n > WEATHER_FORECAST_DAYS) {
        n = WEATHER_FORECAST_DAYS;
    }
    for (int i = 0; i < n; i++) {
        cJSON *t = cJSON_GetArrayItem(time_arr, i);
        cJSON *xmax = cJSON_GetArrayItem(tmax_arr, i);
        cJSON *xmin = cJSON_GetArrayItem(tmin_arr, i);
        cJSON *xcode = cJSON_GetArrayItem(code_arr, i);
        if (!t || !cJSON_IsString(t) || !xmax || !cJSON_IsNumber(xmax) || !xmin ||
            !cJSON_IsNumber(xmin) || !xcode || !cJSON_IsNumber(xcode)) {
            continue;  // a gap in one day's data shouldn't drop the others
        }
        weather_day_t *day = &out->days[out->count];
        strncpy(day->date, t->valuestring, sizeof(day->date) - 1);
        day->date[sizeof(day->date) - 1] = '\0';
        day->temp_max_c = (float) xmax->valuedouble;
        day->temp_min_c = (float) xmin->valuedouble;
        day->weather_code = xcode->valueint;
        out->count++;
    }
    cJSON_Delete(root);
    return ESP_OK;
}

// wttr.in's ?format=j1 uses worldweatheronline.com's numeric weather codes,
// a different scheme than Open-Meteo's WMO codes. Approximates each into the
// closest WMO code from condition_text()'s table above, rather than
// introducing a second condition-text vocabulary - close enough for a short
// display line, not claiming meteorological precision.
static int wttrin_code_to_wmo(int code)
{
    switch (code) {
        case 113: return 0;    // Sunny/Clear
        case 116: return 2;    // Partly cloudy
        case 119: case 122: return 3;  // Cloudy/Overcast
        case 143: case 248: case 260: return 45;  // Mist/Fog/Freezing fog
        case 263: return 51;   // Patchy light drizzle
        case 266: return 53;   // Light drizzle
        case 281: return 53;   // Freezing drizzle
        case 284: return 55;   // Heavy freezing drizzle
        case 176: case 293: case 296: case 311: return 61;  // Light/patchy rain
        case 299: case 302: return 63;  // Moderate rain
        case 305: case 308: case 314: return 65;  // Heavy rain
        case 317: case 323: case 326: return 71;  // Light snow/sleet
        case 320: case 329: case 332: case 350: return 73;  // Moderate snow/sleet
        case 335: case 338: return 75;  // Heavy snow
        case 227: return 73;    // Blowing snow
        case 230: return 75;    // Blizzard
        case 353: case 362: case 368: case 374: return 80;  // Light showers
        case 356: case 365: case 371: case 377: return 81;  // Showers
        case 359: return 82;    // Torrential rain shower
        case 200: case 386: case 392: return 95;  // Thundery outbreaks
        case 389: return 96;    // Rain with thunder
        case 395: return 99;    // Heavy snow with thunder
        default: return -1;     // Unmapped - condition_text() falls back to "unknown"
    }
}

static esp_err_t fetch_wttrin(const char *lat, const char *lon, weather_forecast_t *out)
{
    char url[160];
    snprintf(url, sizeof(url), "https://wttr.in/%s,%s?format=j1", lat, lon);

    char *body = NULL;
    size_t body_len = 0;
    // wttr.in's response can run well past a single day's forecast; the
    // per-day fields we need are all near the front of each "weather" entry,
    // so a truncated tail (later days, or the "nearest_area"/astronomy
    // blocks) is fine to lose.
    esp_err_t err = http_fetch_get(url, WEATHER_HTTP_TIMEOUT_MS, WEATHER_MAX_RESPONSE_BYTES_WTTR,
                                   &body, &body_len, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wttr.in request failed: %s", esp_err_to_name(err));
        return err;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        ESP_LOGW(TAG, "wttr.in response not valid JSON (likely truncated)");
        return ESP_FAIL;
    }

    cJSON *weather_arr = cJSON_GetObjectItem(root, "weather");
    if (!weather_arr || !cJSON_IsArray(weather_arr)) {
        ESP_LOGW(TAG, "wttr.in response missing \"weather\" array");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    int n = cJSON_GetArraySize(weather_arr);
    if (n > WEATHER_FORECAST_DAYS) {
        n = WEATHER_FORECAST_DAYS;
    }
    for (int i = 0; i < n; i++) {
        cJSON *day_obj = cJSON_GetArrayItem(weather_arr, i);
        cJSON *date_item = day_obj ? cJSON_GetObjectItem(day_obj, "date") : NULL;
        cJSON *tmax_item = day_obj ? cJSON_GetObjectItem(day_obj, "maxtempC") : NULL;
        cJSON *tmin_item = day_obj ? cJSON_GetObjectItem(day_obj, "mintempC") : NULL;
        cJSON *hourly_arr = day_obj ? cJSON_GetObjectItem(day_obj, "hourly") : NULL;
        if (!date_item || !cJSON_IsString(date_item) || !tmax_item || !cJSON_IsString(tmax_item) ||
            !tmin_item || !cJSON_IsString(tmin_item)) {
            continue;
        }
        // wttr.in represents each numeric field as a string, unlike
        // Open-Meteo's native JSON numbers.
        int wmo_code = -1;
        if (hourly_arr && cJSON_IsArray(hourly_arr)) {
            // Hourly entries are 3-hour steps ("0", "300", ... "2100") -
            // index 4 is the "1200" (noon) entry, a representative
            // condition for the whole day.
            int noon_idx = cJSON_GetArraySize(hourly_arr) > 4 ? 4 : 0;
            cJSON *noon = cJSON_GetArrayItem(hourly_arr, noon_idx);
            cJSON *code_item = noon ? cJSON_GetObjectItem(noon, "weatherCode") : NULL;
            if (code_item && cJSON_IsString(code_item)) {
                wmo_code = wttrin_code_to_wmo(atoi(code_item->valuestring));
            }
        }
        weather_day_t *day = &out->days[out->count];
        strncpy(day->date, date_item->valuestring, sizeof(day->date) - 1);
        day->date[sizeof(day->date) - 1] = '\0';
        day->temp_max_c = (float) atof(tmax_item->valuestring);
        day->temp_min_c = (float) atof(tmin_item->valuestring);
        day->weather_code = wmo_code;
        out->count++;
    }
    cJSON_Delete(root);
    return ESP_OK;
}

// MET Norway's Locationforecast symbol_code strings (e.g. "partlycloudy_day",
// "heavyrainandthunder") - suffix ("_day"/"_night"/"_polartwilight") is
// irrelevant for a daily-summary line, so substring matching on the
// day-independent stem is enough. Approximates into the same WMO code space
// as wttrin_code_to_wmo() above - order matters (most specific substring
// checked first, e.g. "heavysnow" before "snow").
static int yrno_symbol_to_wmo(const char *symbol)
{
    if (!symbol) {
        return -1;
    }
    if (strstr(symbol, "thunder")) {
        return strstr(symbol, "heavy") ? 99 : 95;
    }
    if (strstr(symbol, "heavysnow")) {
        return 75;
    }
    if (strstr(symbol, "lightsnow") || strstr(symbol, "sleet")) {
        return 71;
    }
    if (strstr(symbol, "snowshowers")) {
        return 81;
    }
    if (strstr(symbol, "snow")) {
        return 73;
    }
    if (strstr(symbol, "heavyrainshowers")) {
        return 82;
    }
    if (strstr(symbol, "lightrainshowers")) {
        return 80;
    }
    if (strstr(symbol, "rainshowers")) {
        return 81;
    }
    if (strstr(symbol, "heavyrain")) {
        return 65;
    }
    if (strstr(symbol, "lightrain")) {
        return 61;
    }
    if (strstr(symbol, "rain")) {
        return 63;
    }
    if (strstr(symbol, "fog")) {
        return 45;
    }
    if (strstr(symbol, "partlycloudy") || strstr(symbol, "fair")) {
        return 2;
    }
    if (strstr(symbol, "cloudy")) {
        return 3;
    }
    if (strstr(symbol, "clearsky")) {
        return 0;
    }
    return -1;
}

// MET Norway has no "N-day daily forecast" endpoint like Open-Meteo - only an
// hourly timeseries. Buckets entries by their UTC calendar date (the
// timeseries timestamps are UTC; MET doesn't return a timezone/offset for
// the queried coordinates, and full IANA timezone handling isn't worth the
// added complexity here) - close to local-day boundaries for most
// longitudes, but can misattribute a few hours near midnight for locations
// far from UTC. "Today" is necessarily a partial day (only hours from now
// onward are in the forecast), so its min/max can undercount relative to
// Open-Meteo's full-calendar-day figures.
static esp_err_t fetch_yrno(const char *lat, const char *lon, weather_forecast_t *out)
{
    char url[200];
    snprintf(url, sizeof(url),
             "https://api.met.no/weatherapi/locationforecast/2.0/compact?lat=%s&lon=%s", lat, lon);

    char *body = NULL;
    size_t body_len = 0;
    // MET Norway's usage terms require a real, identifying User-Agent -
    // requests without one are throttled/rejected. Includes this project's
    // repo URL as the required contact info.
    esp_err_t err =
        http_fetch_get(url, WEATHER_HTTP_TIMEOUT_MS, WEATHER_MAX_RESPONSE_BYTES_YRNO, &body,
                      &body_len, NULL, "esp32-photoframe/1.0 github.com/t3ste/Tlg-esp32-photoframe");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "yr.no request failed: %s", esp_err_to_name(err));
        return err;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        ESP_LOGW(TAG, "yr.no response not valid JSON (likely truncated)");
        return ESP_FAIL;
    }

    cJSON *properties = cJSON_GetObjectItem(root, "properties");
    cJSON *timeseries = properties ? cJSON_GetObjectItem(properties, "timeseries") : NULL;
    if (!timeseries || !cJSON_IsArray(timeseries)) {
        ESP_LOGW(TAG, "yr.no response missing properties.timeseries");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    char current_date[11] = {0};
    float day_min = 0, day_max = 0;
    bool have_temp = false;
    const char *day_symbol = NULL;   // owned by an entry inside `root`, valid until cJSON_Delete
    int best_hour_distance = 999;    // tracks closest-to-noon entry seen for `day_symbol`

    int n = cJSON_GetArraySize(timeseries);
    for (int i = 0; i < n && out->count < WEATHER_FORECAST_DAYS; i++) {
        cJSON *entry = cJSON_GetArrayItem(timeseries, i);
        cJSON *time_item = entry ? cJSON_GetObjectItem(entry, "time") : NULL;
        if (!time_item || !cJSON_IsString(time_item) || strlen(time_item->valuestring) < 13) {
            continue;
        }
        char date[11];
        strncpy(date, time_item->valuestring, 10);
        date[10] = '\0';
        int hour = atoi(time_item->valuestring + 11);

        if (current_date[0] == '\0') {
            strcpy(current_date, date);
        } else if (strcmp(date, current_date) != 0) {
            // Day boundary: flush the completed bucket, start a new one.
            if (have_temp) {
                weather_day_t *day = &out->days[out->count];
                strncpy(day->date, current_date, sizeof(day->date) - 1);
                day->date[sizeof(day->date) - 1] = '\0';
                day->temp_max_c = day_max;
                day->temp_min_c = day_min;
                day->weather_code = yrno_symbol_to_wmo(day_symbol);
                out->count++;
            }
            strcpy(current_date, date);
            have_temp = false;
            day_symbol = NULL;
            best_hour_distance = 999;
            if (out->count >= WEATHER_FORECAST_DAYS) {
                break;
            }
        }

        cJSON *data = cJSON_GetObjectItem(entry, "data");
        cJSON *instant = data ? cJSON_GetObjectItem(data, "instant") : NULL;
        cJSON *details = instant ? cJSON_GetObjectItem(instant, "details") : NULL;
        cJSON *temp_item = details ? cJSON_GetObjectItem(details, "air_temperature") : NULL;
        if (temp_item && cJSON_IsNumber(temp_item)) {
            float t = (float) temp_item->valuedouble;
            if (!have_temp) {
                day_min = day_max = t;
                have_temp = true;
            } else {
                if (t < day_min) day_min = t;
                if (t > day_max) day_max = t;
            }
        }

        // Prefer whichever forecast window is present (near-term entries
        // have next_1_hours; further out, only next_6_hours/next_12_hours
        // exist) from the entry closest to local noon, as a representative
        // condition for the day.
        const char *symbol = NULL;
        if (data) {
            const char *window_keys[] = {"next_1_hours", "next_6_hours", "next_12_hours"};
            for (size_t w = 0; w < sizeof(window_keys) / sizeof(window_keys[0]) && !symbol; w++) {
                cJSON *window = cJSON_GetObjectItem(data, window_keys[w]);
                cJSON *summary = window ? cJSON_GetObjectItem(window, "summary") : NULL;
                cJSON *symbol_item = summary ? cJSON_GetObjectItem(summary, "symbol_code") : NULL;
                if (symbol_item && cJSON_IsString(symbol_item)) {
                    symbol = symbol_item->valuestring;
                }
            }
        }
        if (symbol) {
            int distance = abs(hour - 12);
            if (distance < best_hour_distance) {
                best_hour_distance = distance;
                day_symbol = symbol;
            }
        }
    }
    // Flush the final in-progress bucket (loop above only flushes on a day
    // boundary, so the last day never gets flushed inside the loop).
    if (have_temp && out->count < WEATHER_FORECAST_DAYS) {
        weather_day_t *day = &out->days[out->count];
        strncpy(day->date, current_date, sizeof(day->date) - 1);
        day->date[sizeof(day->date) - 1] = '\0';
        day->temp_max_c = day_max;
        day->temp_min_c = day_min;
        day->weather_code = yrno_symbol_to_wmo(day_symbol);
        out->count++;
    }

    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t weather_fetch_forecast(weather_forecast_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    char lat[WEATHER_LATLON_MAX_LEN];
    char lon[WEATHER_LATLON_MAX_LEN];
    esp_err_t err = resolve_lat_lon(lat, sizeof(lat), lon, sizeof(lon));
    if (err != ESP_OK) {
        return err;
    }

    const char *provider = config_manager_get_weather_provider();
    if (strcmp(provider, WEATHER_PROVIDER_WTTR_IN) == 0) {
        err = fetch_wttrin(lat, lon, out);
    } else if (strcmp(provider, WEATHER_PROVIDER_YR_NO) == 0) {
        err = fetch_yrno(lat, lon, out);
    } else {
        err = fetch_open_meteo(lat, lon, out);
    }
    if (err != ESP_OK) {
        return err;
    }

    if (out->count == 0) {
        ESP_LOGW(TAG, "Forecast response (%s) had no usable daily entries", provider);
        return ESP_FAIL;
    }
    out->valid = true;
    return ESP_OK;
}

// Sakamoto's algorithm: day-of-week for a Gregorian date, pure calendar
// arithmetic - deliberately independent of the device's own configured
// timezone/libc calendar handling (mktime/localtime round-tripping here
// could shift the date near a UTC-offset boundary). Returns 0=Sunday..6=Saturday,
// matching both weekday_en/de below and JavaScript's Date.getDay() convention
// (the same one used by the reference implementation this was matched against).
static int weekday_from_date(int year, int month, int day)
{
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (month < 3) {
        year -= 1;
    }
    return (year + year / 4 - year / 100 + year / 400 + t[month - 1] + day) % 7;
}

static const char *weekday_name(int wday, bool german)
{
    static const char *de[7] = {"So", "Mo", "Di", "Mi", "Do", "Fr", "Sa"};
    static const char *en[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    if (wday < 0 || wday > 6) {
        return "?";
    }
    return german ? de[wday] : en[wday];
}

// WMO weather code -> short condition text, English/German - abbreviated as
// far as reasonably legible (the display's ~46 char/line budget can't fit 3
// full days on one line otherwise; see weather_format_day_lines() for the
// robust fix). Based on an existing reference implementation's wording, with
// the longest entries (Gewitter/storm variants) tightened further: spaces
// after "st./lgt./hvy." and around "+" dropped, "Gewitter" -> "Gew.".
// Unlisted codes (e.g. 56/57 freezing drizzle, 66/67 freezing rain, 77 snow
// grains) intentionally fall through to the "unknown" fallback instead of
// being folded into a nearby bucket.
static const char *condition_text(int code, bool german)
{
    typedef struct {
        int code;
        const char *de;
        const char *en;
    } code_entry_t;
    static const code_entry_t table[] = {
        {0, "Sonne", "sunny"},
        {1, "klar", "clear"},
        {2, "wolkig", "p.cloudy"},
        {3, "bedeckt", "cloudy"},
        {45, "Nebel", "fog"},
        {48, "Nebel", "fog"},
        {51, "Niesel", "lgt.drizzle"},
        {53, "Niesel", "drizzle"},
        {55, "st.Niesel", "hvy.drizzle"},
        {61, "Schauer", "lgt.rain"},
        {63, "Regen", "rain"},
        {65, "st.Regen", "hvy.rain"},
        {71, "Schnee", "lgt.snow"},
        {73, "Schnee", "snow"},
        {75, "st.Schnee", "hvy.snow"},
        {80, "Schauer", "lgt.showers"},
        {81, "Schauer", "showers"},
        {82, "st.Schauer", "hvy.showers"},
        {95, "Gew.", "storm"},
        {96, "Gew.+Hagel", "storm+hail"},
        {99, "st.Gew.+Hagel", "hvy.storm+hail"},
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (table[i].code == code) {
            return german ? table[i].de : table[i].en;
        }
    }
    return german ? "unbekannt" : "unknown";
}

static void format_day_line(const weather_day_t *day, bool german, char *out, size_t out_len)
{
    int year = 0, month = 0, mday = 0;
    sscanf(day->date, "%d-%d-%d", &year, &month, &mday);
    const char *wd = weekday_name(weekday_from_date(year, month, mday), german);
    const char *cond = condition_text(day->weather_code, german);
    int tmin = (int) lroundf(day->temp_min_c);
    int tmax = (int) lroundf(day->temp_max_c);
    snprintf(out, out_len, "%s %s %d/%d", wd, cond, tmin, tmax);
}

void weather_format_line(const weather_forecast_t *f, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!f || !f->valid || f->count == 0) {
        return;
    }

    bool german = (strcmp(config_manager_get_overlay_language(), "de") == 0);
    size_t o = 0;
    for (int i = 0; i < f->count; i++) {
        char day_line[WEATHER_DAY_LINE_MAX_LEN];
        format_day_line(&f->days[i], german, day_line, sizeof(day_line));

        char part[WEATHER_DAY_LINE_MAX_LEN + 4];
        int part_len = snprintf(part, sizeof(part), "%s%s", (i > 0) ? " | " : "", day_line);
        if (part_len < 0 || o + (size_t) part_len >= out_len) {
            break;  // would overflow - stop here, draw_overlay_bar truncates/ellipsizes further anyway
        }
        memcpy(out + o, part, (size_t) part_len);
        o += (size_t) part_len;
        out[o] = '\0';
    }
}

void weather_format_day_lines(const weather_forecast_t *f,
                              char out_lines[WEATHER_FORECAST_DAYS][WEATHER_DAY_LINE_MAX_LEN],
                              int *out_count)
{
    *out_count = 0;
    if (!f || !f->valid) {
        return;
    }
    bool german = (strcmp(config_manager_get_overlay_language(), "de") == 0);
    for (int i = 0; i < f->count && i < WEATHER_FORECAST_DAYS; i++) {
        format_day_line(&f->days[i], german, out_lines[i], WEATHER_DAY_LINE_MAX_LEN);
    }
    *out_count = (f->count < WEATHER_FORECAST_DAYS) ? f->count : WEATHER_FORECAST_DAYS;
}
