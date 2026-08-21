#include "overlay_manager.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "board_hal.h"
#include "config.h"
#include "config_manager.h"
#include "esp_log.h"
#include "headlines.h"
#include "image_processor.h"
#include "weather.h"

static const char *TAG = "overlay_manager";

// Weather (at most WEATHER_FORECAST_DAYS lines in multi-line mode) +
// headlines (at most HEADLINE_MAX_COUNT lines, or HEADLINES_WRAP_LINES_MAX
// lines when a single headline is word-wrapped) - either way, weather is
// forced back to a single combined line whenever headlines are enabled (see
// overlay_manager_apply()), so the two never actually add up to more than
// max(WEATHER_FORECAST_DAYS, 1 + HEADLINE_MAX_COUNT) at once.
#define OVERLAY_LINES_CAP \
    (WEATHER_FORECAST_DAYS > (1 + HEADLINE_MAX_COUNT) ? WEATHER_FORECAST_DAYS : (1 + HEADLINE_MAX_COUNT))

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
    if (!weather_on && !headlines_on) {
        return source_path;
    }

    image_format_t format = image_processor_detect_format(source_path);
    if (format != IMAGE_FORMAT_PNG || !image_processor_is_processed(source_path)) {
        ESP_LOGI(TAG, "Skipping overlay for %s: not a processed PNG", source_path);
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
        if (weather_fetch_forecast(&forecast) == ESP_OK) {
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
            if (headlines_fetch(rss_url, count, &headlines) == ESP_OK) {
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

    if (line_count == 0) {
        ESP_LOGI(TAG, "No overlay content available this cycle, showing %s unmodified", source_path);
        return source_path;
    }

    if (!copy_file(source_path, CURRENT_OVERLAY_PNG_PATH)) {
        return source_path;
    }

    bool invert_colors = config_manager_get_overlay_invert_colors();
    esp_err_t err = image_processor_add_overlay_to_file(CURRENT_OVERLAY_PNG_PATH, lines, line_count,
                                                         invert_colors);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to draw overlay onto scratch copy: %s", esp_err_to_name(err));
        return source_path;
    }

    return CURRENT_OVERLAY_PNG_PATH;
}
