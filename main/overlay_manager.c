#include "overlay_manager.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "board_hal.h"
#include "chime.h"
#include "climate.h"
#include "config.h"
#include "config_manager.h"
#include "esp_log.h"
#include "exif_reader.h"
#include "headlines.h"
#include "image_processor.h"
#include "utils.h"
#include "weather.h"

static const char *TAG = "overlay_manager";

// Weather (at most WEATHER_FORECAST_DAYS lines in multi-line mode) +
// headlines (at most HEADLINE_MAX_COUNT lines, or HEADLINES_WRAP_LINES_MAX
// lines when a single headline is word-wrapped) - either way, weather is
// forced back to a single combined line whenever headlines are enabled (see
// overlay_manager_apply()), so the two never actually add up to more than
// max(WEATHER_FORECAST_DAYS, 1 + HEADLINE_MAX_COUNT) at once.
#define OVERLAY_LINES_CAP                                                     \
    (WEATHER_FORECAST_DAYS > (1 + HEADLINE_MAX_COUNT) ? WEATHER_FORECAST_DAYS \
                                                      : (1 + HEADLINE_MAX_COUNT))

// Hysteresis-debounced low-battery corner badge check - mirrors
// check_and_warn_low_battery()'s exact shape (telegram_bot.c), but
// Telegram-independent and NVS-persisted under its own key
// (config_manager_get/set_low_battery_overlay_active), since this is
// consulted from every render path, not just Telegram polls, and its state
// must survive deep sleep (which reboots the device every wake).
static bool low_battery_overlay_should_show(int *out_percent)
{
    if (!config_manager_get_low_battery_overlay_enabled()) {
        return false;
    }
    if (!board_hal_is_battery_connected()) {
        return false;
    }
    int percent = board_hal_get_battery_percent();
    if (percent < 0) {
        return false;
    }

    int low = config_manager_get_low_battery_overlay_threshold();
    int clear = low + LOW_BATTERY_OVERLAY_CLEAR_MARGIN;

    bool active = config_manager_get_low_battery_overlay_active();
    if (!active && percent < low) {
        active = true;
        config_manager_set_low_battery_overlay_active(true);
    } else if (active && percent > clear) {
        active = false;
        config_manager_set_low_battery_overlay_active(false);
    }

    // Repeats once per render while still below threshold (not just on the
    // first crossing), up to CHIME_REPEAT_MAX times, then goes quiet until
    // the level actually recovers - see chime_repeat_gate().
    if (chime_repeat_gate(CHIME_EVENT_LOW_BATTERY, active)) {
        chime_play_if_enabled(CHIME_EVENT_LOW_BATTERY);
    }

    if (active && out_percent) {
        *out_percent = percent;
    }
    return active;
}

// Decorative (unlike the battery badge above, which is a safety
// notification) - respects the normal EPDGZ-overlay-disabled gate in
// overlay_manager_apply() rather than forcing through. Owns unit
// conversion/text formatting itself (image_processor.c stays unit-
// agnostic, just draws whatever short string it's given) and classifies
// each channel independently - temperature and humidity are never merged
// into one combined verdict (by design). Returns false (nothing to draw)
// if the feature is off or both sensor reads fail; a caller should still
// check each `out_*_text` for an empty string, since one channel can
// succeed while the other fails.
static bool climate_badge_should_show(char *out_temp_text, size_t temp_text_len,
                                      climate_category_t *out_temp_category, char *out_hum_text,
                                      size_t hum_text_len, climate_category_t *out_hum_category)
{
    out_temp_text[0] = '\0';
    out_hum_text[0] = '\0';
    if (!config_manager_get_climate_overlay_enabled()) {
        return false;
    }

    float temp_c, humidity;
    bool have_temp = (climate_read_temperature(&temp_c) == ESP_OK);
    bool have_hum = (climate_read_humidity(&humidity) == ESP_OK);
    if (!have_temp && !have_hum) {
        return false;
    }

    climate_room_type_t room = config_manager_get_climate_room_type();
    if (have_temp) {
        *out_temp_category = climate_classify_temperature(temp_c, room);
        if (config_manager_get_climate_temp_unit() == CLIMATE_UNIT_FAHRENHEIT) {
            snprintf(out_temp_text, temp_text_len, "%dF", climate_celsius_to_fahrenheit(temp_c));
        } else {
            snprintf(out_temp_text, temp_text_len, "%dC", (int) lroundf(temp_c));
        }
    }
    if (have_hum) {
        *out_hum_category = climate_classify_humidity(humidity, room);
        snprintf(out_hum_text, hum_text_len, "%d%%", (int) lroundf(humidity));
    }
    return true;
}

static bool copy_file(const char *src_path, const char *dst_path)
{
    FILE *src = fopen(src_path, "rb");
    if (!src) {
        ESP_LOGW(TAG, "Failed to open %s for overlay scratch copy", src_path);
        return false;
    }
    FILE *dst = fopen(dst_path, "wb");
    if (!dst) {
        fclose(src);
        ESP_LOGW(TAG, "Failed to open %s for overlay scratch copy", dst_path);
        return false;
    }
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        fwrite(buf, 1, n, dst);
    }
    fclose(src);
    fclose(dst);
    return true;
}

const char *overlay_manager_apply(const char *source_path)
{
    if (!source_path) {
        return source_path;
    }

    bool weather_on = config_manager_get_weather_overlay_enabled();
    bool headlines_on = config_manager_get_headlines_overlay_enabled();
    int battery_percent = 0;
    bool battery_badge_due = low_battery_overlay_should_show(&battery_percent);
    // Informational (like weather/headlines), not a safety notification (like
    // the battery badge) - reads a sidecar process-cli writes alongside a
    // Storage/Auto-Rotate album image (see exif_reader.h), gated on the same
    // setting that already governs this for Telegram-received photos.
    char exif_caption[32] = {0};
    bool exif_caption_due =
        config_manager_get_show_exif_datetime_enabled() &&
        capture_date_sidecar_read(source_path, exif_caption, sizeof(exif_caption));
    // Decorative like weather/headlines/the capture-date caption above, not
    // a safety notification like the battery badge - see
    // climate_badge_should_show()'s doc comment.
    char climate_temp_text[8] = {0};
    char climate_hum_text[8] = {0};
    climate_category_t climate_temp_category = CLIMATE_CATEGORY_GOOD;
    climate_category_t climate_hum_category = CLIMATE_CATEGORY_GOOD;
    bool climate_badge_due = climate_badge_should_show(
        climate_temp_text, sizeof(climate_temp_text), &climate_temp_category, climate_hum_text,
        sizeof(climate_hum_text), &climate_hum_category);
    if (!weather_on && !headlines_on && !exif_caption_due && !battery_badge_due &&
        !climate_badge_due) {
        return source_path;
    }

    image_format_t format = image_processor_detect_format(source_path);
    bool is_epdgz = (format == IMAGE_FORMAT_EPD_GZ);
    if (is_epdgz && !config_manager_get_overlay_epdgz_enabled()) {
        if (!battery_badge_due) {
            ESP_LOGI(TAG, "Skipping overlay for %s: EPDGZ overlay support is disabled",
                     source_path);
            return source_path;
        }
        // The battery badge is a safety notification, not a decorative
        // overlay - draws even on EPDGZ regardless of this (otherwise
        // unrelated) setting, but weather/headlines/the capture-date caption
        // still respect it: don't let them piggyback onto this pass just
        // because the badge needed it.
        weather_on = false;
        headlines_on = false;
        exif_caption_due = false;
        climate_badge_due = false;
    }
    if (!is_epdgz && (format != IMAGE_FORMAT_PNG || !image_processor_is_processed(source_path))) {
        ESP_LOGI(TAG, "Skipping overlay for %s: not a processed PNG or EPDGZ", source_path);
        return source_path;
    }

    // Fetch first (before touching disk) - if nothing ends up fetchable,
    // there's no point creating a scratch copy at all.
    const char *lines[OVERLAY_LINES_CAP];
    int line_count = 0;

    // Backing storage - must outlive the image_processor_add_overlay_to_file()
    // call below, since `lines[]` only holds pointers into these.
    char weather_line[220] = {0};
    char weather_day_lines[WEATHER_FORECAST_DAYS][WEATHER_DAY_LINE_MAX_LEN];
    char headline_wrapped[OVERLAY_LINES_CAP][OVERLAY_LINE_MAX_CHARS];

    if (weather_on) {
        weather_forecast_t forecast;
        bool weather_ok = (weather_fetch_forecast(&forecast) == ESP_OK);
        utils_record_internet_attempt(weather_ok);
        if (weather_ok) {
            // Multi-line (one line per day) only when headlines won't also
            // be claiming lines - together they could otherwise grow to an
            // unreasonably tall overlay (see NVS_WEATHER_MULTILINE_KEY).
            bool use_multiline = config_manager_get_weather_multiline_enabled() && !headlines_on;
            if (use_multiline) {
                int day_count = 0;
                weather_format_day_lines(&forecast, weather_day_lines, &day_count);
                for (int i = 0; i < day_count && line_count < OVERLAY_LINES_CAP; i++) {
                    lines[line_count++] = weather_day_lines[i];
                }
            } else {
                weather_format_line(&forecast, weather_line, sizeof(weather_line));
                if (weather_line[0] != '\0') {
                    lines[line_count++] = weather_line;
                }
            }
        } else {
            ESP_LOGW(TAG, "Weather fetch failed, skipping weather overlay this cycle");
        }
    }

    headlines_result_t headlines = {0};
    if (headlines_on) {
        const char *rss_url = config_manager_get_headlines_rss_url();
        if (rss_url[0] == '\0') {
            ESP_LOGW(TAG, "Headlines overlay enabled but no RSS feed URL configured");
        } else {
            int count = config_manager_get_headlines_count();
            bool headlines_ok = (headlines_fetch(rss_url, count, &headlines) == ESP_OK);
            utils_record_internet_attempt(headlines_ok);
            if (headlines_ok) {
                // A single fetched headline can optionally be word-wrapped
                // across multiple display lines instead of hard-truncated to
                // one - only meaningful/offered when exactly one headline
                // was requested (with more than one, each already gets its
                // own line and wrapping would make the total unpredictable).
                int wrap_lines = config_manager_get_headlines_wrap_lines();
                if (headlines.count == 1 && wrap_lines > 1) {
                    int wrapped_count = image_processor_wrap_text(
                        headlines.titles[0], BOARD_HAL_DISPLAY_WIDTH, wrap_lines, headline_wrapped);
                    for (int i = 0; i < wrapped_count && line_count < OVERLAY_LINES_CAP; i++) {
                        lines[line_count++] = headline_wrapped[i];
                    }
                } else {
                    for (int i = 0; i < headlines.count && line_count < OVERLAY_LINES_CAP; i++) {
                        lines[line_count++] = headlines.titles[i];
                    }
                }
            } else {
                ESP_LOGW(TAG, "Headlines fetch failed, skipping headlines overlay this cycle");
            }
        }
    }

    if (line_count == 0 && !exif_caption_due && !battery_badge_due && !climate_badge_due) {
        ESP_LOGI(TAG, "No overlay content available this cycle, showing %s unmodified",
                 source_path);
        return source_path;
    }

    // Static, not a stack local: the returned pointer must stay valid after
    // this function returns (its callers use it immediately afterward,
    // single-threaded on the main task only, same assumption already used
    // elsewhere in this codebase for a static scratch/persist buffer).
    static char scratch_path[64];
    strncpy(scratch_path, is_epdgz ? CURRENT_OVERLAY_EPDGZ_PATH : CURRENT_OVERLAY_PNG_PATH,
            sizeof(scratch_path) - 1);
    scratch_path[sizeof(scratch_path) - 1] = '\0';

    if (!copy_file(source_path, scratch_path)) {
        return source_path;
    }

    bool invert_colors = config_manager_get_overlay_invert_colors();
    esp_err_t err = image_processor_add_overlay_to_file(
        scratch_path, lines, line_count, invert_colors, battery_badge_due, battery_percent,
        exif_caption_due ? exif_caption : NULL, climate_badge_due && climate_temp_text[0] != '\0',
        climate_temp_text, climate_temp_category, climate_badge_due && climate_hum_text[0] != '\0',
        climate_hum_text, climate_hum_category);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to draw overlay onto scratch copy: %s", esp_err_to_name(err));
        return source_path;
    }

    return scratch_path;
}
