#include "agenda_manager.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "agenda_renderer.h"
#include "board_hal.h"
#include "calendar_ics.h"
#include "chime.h"
#include "config.h"
#include "config_manager.h"
#include "cron.h"
#include "display_manager.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "todo.h"
#include "utils.h"
#include "weather.h"

static const char *TAG = "agenda_manager";

// Optional climate readout for both column headers - see
// config_manager_get_climate_agenda_header_enabled() and agenda_climate_t's
// doc comment. Same unit-conversion/formatting-owned-by-the-caller shape as
// overlay_manager.c's climate_badge_should_show() for the photo overlay;
// duplicated rather than shared since the two live in unrelated render
// paths with no common caller. Returns false (nothing to draw) if the
// setting is off or both sensor reads fail.
static bool build_agenda_climate(agenda_climate_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!config_manager_get_climate_agenda_header_enabled()) {
        return false;
    }

    float temp_c, humidity;
    out->has_temp = (board_hal_get_temperature(&temp_c) == ESP_OK);
    out->has_hum = (board_hal_get_humidity(&humidity) == ESP_OK);
    if (!out->has_temp && !out->has_hum) {
        return false;
    }

    climate_room_type_t room = config_manager_get_climate_room_type();
    if (out->has_temp) {
        out->temp_category = climate_classify_temperature(temp_c, room);
        if (config_manager_get_climate_temp_unit() == CLIMATE_UNIT_FAHRENHEIT) {
            snprintf(out->temp_text, sizeof(out->temp_text), "%dF",
                     climate_celsius_to_fahrenheit(temp_c));
        } else {
            snprintf(out->temp_text, sizeof(out->temp_text), "%dC", (int) lroundf(temp_c));
        }
    }
    if (out->has_hum) {
        out->hum_category = climate_classify_humidity(humidity, room);
        snprintf(out->hum_text, sizeof(out->hum_text), "%d%%", (int) lroundf(humidity));
    }
    return true;
}

// Chimes: a short, local re-derivation of the same "YYYY-MM-DD" string
// comparison agenda_renderer.c's (private) due_status() already does for
// per-element coloring - duplicated here rather than exposed from the
// renderer, since it's a 3-line comparison and this is the only other
// place that needs it. Repeats once per Agenda render while at least one
// item is still due/overdue, up to CHIME_REPEAT_MAX times, then goes quiet
// until nothing is due anymore (which resets the count - see
// chime_repeat_gate()) - so a later, different due item chimes again.
static void agenda_chime_due_todo_if_needed(const todo_list_t *todo)
{
    time_t now = time(NULL);
    struct tm now_tm;
    localtime_r(&now, &now_tm);
    // Oversized vs. the exact "YYYY-MM-DD" (10 chars) ever compared below -
    // silences -Wformat-truncation, which (correctly) can't prove
    // tm_year+1900 always fits in 4 digits from this call site alone.
    char today_str[32];
    snprintf(today_str, sizeof(today_str), "%04d-%02d-%02d", now_tm.tm_year + 1900,
             now_tm.tm_mon + 1, now_tm.tm_mday);

    bool has_due = false;
    for (int i = 0; i < todo->count && !has_due; i++) {
        const char *due = todo->items[i].due_date;
        if (due[0] != '\0' && strncmp(due, today_str, 10) <= 0) {
            has_due = true;
        }
    }

    if (chime_repeat_gate(CHIME_EVENT_AGENDA_DUE, has_due)) {
        chime_play_if_enabled(CHIME_EVENT_AGENDA_DUE);
    }
}

// If `list` has no event overlapping or after `now`, injects a single
// synthetic all-day event naming `display_name` so a source that will never
// refresh itself automatically (see NVS_AGENDA_CAL_C_URL_KEY etc. in
// config.h) stays visibly identifiable in the Calendar column every day
// until the user replaces it (new URL, "refresh now," or a fresh upload),
// instead of just silently going empty forever. Only used for the three
// extra sources below - Calendar A/B's emptiness is presumed transient (a
// fetch failure this wake only), not a permanent "this needs attention"
// signal.
static void inject_stale_reminder_if_needed(ics_event_list_t *list, const char *display_name,
                                            time_t now)
{
    if (calendar_ics_has_upcoming_event(list, now)) {
        return;  // still has upcoming content - nothing to flag
    }
    if (list->count >= ICS_MAX_EVENTS) {
        return;  // pathological - no room, leave as-is rather than drop a real event
    }

    struct tm tm_now;
    localtime_r(&now, &tm_now);
    tm_now.tm_hour = 0;
    tm_now.tm_min = 0;
    tm_now.tm_sec = 0;
    time_t today = mktime(&tm_now);

    ics_event_t *ev = &list->events[list->count++];
    memset(ev, 0, sizeof(*ev));
    ev->start = today;
    ev->end = today;
    ev->all_day = true;
    bool german = (strcmp(config_manager_get_overlay_language(), "de") == 0);
    if (german) {
        snprintf(ev->summary, sizeof(ev->summary),
                 "\xE2\x9A\xA0 %s: keine aktuellen Termine - bitte aktualisieren", display_name);
    } else {
        snprintf(ev->summary, sizeof(ev->summary),
                 "\xE2\x9A\xA0 %s: no upcoming events - please update", display_name);
    }
}

// Loads one of the three extra, non-auto-refreshing ICS sources - no
// network involved either way, just local files (the raw source itself is
// only ever fetched/uploaded on an explicit user action, see
// apply_extra_ics_url() in utils.c and the /api/agenda/extra-ics upload
// endpoint in http_server.c). Two-tier cache to avoid re-parsing a
// potentially large raw .ics file on every agenda wake:
//   1. Fast path: read the flat, already-expanded cache
//      (calendar_ics_read_expanded_cache() - a cheap line-split, no ICS
//      parsing at all) and use it as-is if it still has upcoming content.
//   2. Slow path: only when that cache is missing or exhausted, re-parse
//      the raw source (calendar_ics_read_cache()) for the next
//      AGENDA_EXTRA_ICS_EXPAND_DAYS days and write the flat result back -
//      for a real feed this only happens roughly once a month, not on
//      every wake, which is the whole point for a large hand-curated or
//      exported file (e.g. a year of holidays/school-holidays).
// Either way, injects the stale reminder above if the (possibly freshly
// re-expanded) result still has no upcoming content. Returns false (and
// leaves `out` zeroed) if the source isn't enabled or both cache tiers
// come up empty - same fail-soft contract as the Calendar A/B fetch blocks.
static bool load_extra_ics_source(bool enabled, const char *raw_cache_path,
                                  const char *flat_cache_path, const char *display_name, time_t now,
                                  ics_event_list_t *out)
{
    if (!enabled) {
        return false;
    }

    if (calendar_ics_read_expanded_cache(flat_cache_path, out) == ESP_OK &&
        calendar_ics_has_upcoming_event(out, now)) {
        return true;  // fast path - still fresh, no re-parse needed
    }

    time_t expand_end = now + (time_t) AGENDA_EXTRA_ICS_EXPAND_DAYS * 86400;
    if (calendar_ics_read_cache(raw_cache_path, now, expand_end, out) != ESP_OK) {
        ESP_LOGW(TAG,
                 "Extra ICS source '%s' is enabled but has no saved URL/file - that column will "
                 "be omitted",
                 display_name);
        return false;  // source never configured, or the raw cache is missing/corrupt
    }
    calendar_ics_write_expanded_cache(flat_cache_path, out);
    ESP_LOGI(TAG, "Re-expanded extra ICS source '%s' for the next %d days", display_name,
             AGENDA_EXTRA_ICS_EXPAND_DAYS);
    inject_stale_reminder_if_needed(out, display_name, now);
    return true;
}

bool agenda_manager_is_enabled(void)
{
    if (!config_manager_get_agenda_todo_enabled() && !config_manager_get_agenda_cal_enabled()) {
        return false;
    }
    return config_manager_get_agenda_cron_rule_count() > 0;
}

bool agenda_manager_wake_matches_now(void)
{
    cron_rule_t rules[MAX_CRON_RULES];
    int n = config_manager_get_compiled_agenda_cron_rules(rules, MAX_CRON_RULES);
    if (n == 0) {
        return false;
    }

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    for (int i = 0; i < n; i++) {
        if (cron_match(&rules[i], &timeinfo)) {
            return true;
        }
    }
    return false;
}

int agenda_manager_seconds_until_next_wake(void)
{
    cron_rule_t rules[MAX_CRON_RULES];
    int n = config_manager_get_compiled_agenda_cron_rules(rules, MAX_CRON_RULES);
    if (n == 0) {
        return CRON_FALLBACK_SEC;
    }

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    return cron_seconds_until_next(&timeinfo, rules, n);
}

esp_err_t agenda_manager_run(void)
{
    bool want_todo = config_manager_get_agenda_todo_enabled();
    bool want_cal = config_manager_get_agenda_cal_enabled();

    // Heap/PSRAM-allocated rather than stack locals: todo_list_t and
    // ics_event_list_t are ~17.7KB and ~4.4KB respectively (24 items each,
    // with per-item fixed-size text/tag arrays) - together well over the
    // entire 12KB stack of the dedicated deep_sleep_wake task this runs on
    // (main.c's deep_sleep_wake_task). As stack locals this was a
    // guaranteed, coredump-confirmed stack overflow (vApplicationStackOverflowHook)
    // on every single agenda wake, sometimes surfacing as a clean panic and
    // sometimes as corrupted-looking heap/task state elsewhere (whatever
    // memory happened to sit past the stack's end).
    todo_list_t *todo = heap_caps_calloc(1, sizeof(todo_list_t), MALLOC_CAP_SPIRAM);
    ics_event_list_t *events_a = heap_caps_calloc(1, sizeof(ics_event_list_t), MALLOC_CAP_SPIRAM);
    ics_event_list_t *events_b = heap_caps_calloc(1, sizeof(ics_event_list_t), MALLOC_CAP_SPIRAM);
    ics_event_list_t *events_c = heap_caps_calloc(1, sizeof(ics_event_list_t), MALLOC_CAP_SPIRAM);
    ics_event_list_t *events_d = heap_caps_calloc(1, sizeof(ics_event_list_t), MALLOC_CAP_SPIRAM);
    ics_event_list_t *events_e = heap_caps_calloc(1, sizeof(ics_event_list_t), MALLOC_CAP_SPIRAM);
    if (!todo || !events_a || !events_b || !events_c || !events_d || !events_e) {
        ESP_LOGE(TAG, "Failed to allocate agenda fetch buffers");
        heap_caps_free(todo);
        heap_caps_free(events_a);
        heap_caps_free(events_b);
        heap_caps_free(events_c);
        heap_caps_free(events_d);
        heap_caps_free(events_e);
        return ESP_ERR_NO_MEM;
    }

    // Calendar fetched before ToDo (reversed from this feature's original
    // order): live testing found Calendar consistently failing to connect
    // (ESP_ERR_HTTP_CONNECT) while ToDo succeeded every time in the same
    // cycle, matching the internal-SRAM-exhaustion class of bug already
    // root-caused once in this project (f22e2e1 - a first TLS fetch can
    // leave too little contiguous internal heap for a second, more
    // demanding handshake, e.g. a longer certificate chain, to succeed).
    // Both fetches log free internal heap right before connecting (a byte
    // count only, never anything about the URL/host) so this can be
    // confirmed from the debug log; running the apparently more demanding
    // one first, while heap is freshest, is a safe, low-risk mitigation
    // regardless of the exact numbers.
    bool have_events_a = false, have_events_b = false;
    int cal_days = config_manager_get_agenda_cal_days();
    if (want_cal) {
        const char *url = config_manager_get_agenda_cal_url();
        const char *url2 = config_manager_get_agenda_cal_url2();
        if (url[0] == '\0' && url2[0] == '\0') {
            ESP_LOGW(TAG, "Calendar enabled but no URL configured");
        }
        time_t now = time(NULL);
        time_t window_end = now + (time_t) cal_days * 86400;
        if (url[0] != '\0') {
            ESP_LOGI(TAG, "Free internal heap before Calendar fetch: %u bytes",
                     (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            char etag_out[HTTP_ETAG_MAX_LEN];
            bool ok = (calendar_ics_fetch(url, 0, now, window_end, AGENDA_CAL_CACHE_PATH,
                                          config_manager_get_agenda_cal_etag(), etag_out,
                                          sizeof(etag_out), events_a) == ESP_OK);
            utils_record_internet_attempt(ok);
            have_events_a = ok;
            if (ok) {
                config_manager_set_agenda_cal_etag(etag_out);
            } else {
                ESP_LOGW(TAG, "Calendar fetch failed, that column will be omitted this cycle");
            }
        }
        if (url2[0] != '\0') {
            ESP_LOGI(TAG, "Free internal heap before Calendar 2 fetch: %u bytes",
                     (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            char etag_out[HTTP_ETAG_MAX_LEN];
            bool ok = (calendar_ics_fetch(url2, 0, now, window_end, AGENDA_CAL_CACHE_PATH2,
                                          config_manager_get_agenda_cal_etag2(), etag_out,
                                          sizeof(etag_out), events_b) == ESP_OK);
            utils_record_internet_attempt(ok);
            have_events_b = ok;
            if (ok) {
                config_manager_set_agenda_cal_etag2(etag_out);
            } else {
                ESP_LOGW(TAG, "Calendar 2 fetch failed, that source will be omitted this cycle");
            }
        }
    }

    // Three extra, user-supplied ICS sources (e.g. holidays/school-holidays/
    // other special-days feeds) - unlike A/B above, these are never fetched
    // here: they were already downloaded/uploaded once, ahead of time (see
    // utils.c's apply_config_from_json() and the /api/agenda/extra-ics
    // upload endpoint in http_server.c), so this is a pure local read, no
    // network, no ETag - see load_extra_ics_source()'s own comment for the
    // two-tier flat-cache/raw-reparse split. Gated on want_cal per the same
    // "only matters if the Calendar column is actually showing" logic as
    // A/B.
    bool have_events_c = false, have_events_d = false, have_events_e = false;
    if (want_cal) {
        time_t now = time(NULL);
        const char *name_c = config_manager_get_agenda_cal_c_name();
        const char *name_d = config_manager_get_agenda_cal_d_name();
        const char *name_e = config_manager_get_agenda_cal_e_name();
        have_events_c = load_extra_ics_source(config_manager_get_agenda_cal_c_enabled(),
                                              AGENDA_CAL_CACHE_PATH_C, AGENDA_CAL_CACHE_PATH_C_FLAT,
                                              name_c[0] ? name_c : "Calendar C", now, events_c);
        have_events_d = load_extra_ics_source(config_manager_get_agenda_cal_d_enabled(),
                                              AGENDA_CAL_CACHE_PATH_D, AGENDA_CAL_CACHE_PATH_D_FLAT,
                                              name_d[0] ? name_d : "Calendar D", now, events_d);
        have_events_e = load_extra_ics_source(config_manager_get_agenda_cal_e_enabled(),
                                              AGENDA_CAL_CACHE_PATH_E, AGENDA_CAL_CACHE_PATH_E_FLAT,
                                              name_e[0] ? name_e : "Calendar E", now, events_e);
    }

    // Opt-in per-day forecast annotation on the Calendar column's day
    // dividers - reuses the exact same weather_fetch_forecast() the photo
    // weather overlay already calls (same location/provider settings, own
    // toggle since this is a separate display path). Small enough
    // (WEATHER_FORECAST_DAYS=3 days of a few fields each) to keep as a
    // stack local, unlike todo/events above - no risk of repeating that
    // stack-overflow bug. Skipped entirely if neither calendar source
    // actually fetched anything, since there would be no day divider to
    // annotate either way.
    weather_forecast_t cal_weather;
    memset(&cal_weather, 0, sizeof(cal_weather));
    bool have_cal_weather = false;
    if ((have_events_a || have_events_b || have_events_c || have_events_d || have_events_e) &&
        config_manager_get_agenda_cal_weather_enabled()) {
        bool ok = (weather_fetch_forecast(&cal_weather) == ESP_OK);
        utils_record_internet_attempt(ok);
        have_cal_weather = ok && cal_weather.valid;
    }

    bool have_todo = false;
    if (want_todo) {
        const char *url = config_manager_get_agenda_todo_url();
        if (url[0] == '\0') {
            ESP_LOGW(TAG, "ToDo enabled but no URL configured");
        } else {
            ESP_LOGI(TAG, "Free internal heap before ToDo fetch: %u bytes",
                     (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            char etag_out[HTTP_ETAG_MAX_LEN];
            bool ok =
                (todo_fetch(url, 0, AGENDA_TODO_CACHE_PATH, config_manager_get_agenda_todo_etag(),
                            etag_out, sizeof(etag_out), todo) == ESP_OK);
            utils_record_internet_attempt(ok);
            have_todo = ok;
            if (ok) {
                config_manager_set_agenda_todo_etag(etag_out);
            } else {
                ESP_LOGW(TAG, "ToDo fetch failed, that column will be omitted this cycle");
            }
        }
    }

    if (have_todo) {
        agenda_chime_due_todo_if_needed(todo);
    }

    agenda_climate_t climate;
    bool have_climate = build_agenda_climate(&climate);

    esp_err_t result;
    if (!have_todo && !have_events_a && !have_events_b && !have_events_c && !have_events_d &&
        !have_events_e) {
        ESP_LOGW(TAG, "Nothing to render this agenda cycle (no source fetched successfully)");
        result = ESP_FAIL;
    } else {
        result = agenda_renderer_render(
            have_todo ? todo : NULL, have_events_a ? events_a : NULL,
            have_events_b ? events_b : NULL, have_events_c ? events_c : NULL,
            have_events_d ? events_d : NULL, have_events_e ? events_e : NULL,
            have_cal_weather ? &cal_weather : NULL, cal_days, AGENDA_OUTPUT_PATH, IMAGE_FORMAT_PNG,
            have_climate ? &climate : NULL);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "Failed to render agenda screen: %s", esp_err_to_name(result));
        } else {
            result = display_manager_show_image(AGENDA_OUTPUT_PATH);
        }
    }

    heap_caps_free(todo);
    heap_caps_free(events_a);
    heap_caps_free(events_b);
    heap_caps_free(events_c);
    heap_caps_free(events_d);
    heap_caps_free(events_e);
    return result;
}
