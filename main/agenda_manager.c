#include "agenda_manager.h"

#include <stdlib.h>
#include <time.h>

#include "agenda_renderer.h"
#include "calendar_ics.h"
#include "config.h"
#include "config_manager.h"
#include "cron.h"
#include "display_manager.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "todo.h"
#include "utils.h"

static const char *TAG = "agenda_manager";

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
    if (!todo || !events_a || !events_b) {
        ESP_LOGE(TAG, "Failed to allocate agenda fetch buffers");
        heap_caps_free(todo);
        heap_caps_free(events_a);
        heap_caps_free(events_b);
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
            bool ok = (calendar_ics_fetch(url, 0, now, window_end, events_a) == ESP_OK);
            utils_record_internet_attempt(ok);
            have_events_a = ok;
            if (!ok) {
                ESP_LOGW(TAG, "Calendar fetch failed, that column will be omitted this cycle");
            }
        }
        if (url2[0] != '\0') {
            ESP_LOGI(TAG, "Free internal heap before Calendar 2 fetch: %u bytes",
                    (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            bool ok = (calendar_ics_fetch(url2, 0, now, window_end, events_b) == ESP_OK);
            utils_record_internet_attempt(ok);
            have_events_b = ok;
            if (!ok) {
                ESP_LOGW(TAG, "Calendar 2 fetch failed, that source will be omitted this cycle");
            }
        }
    }

    bool have_todo = false;
    if (want_todo) {
        const char *url = config_manager_get_agenda_todo_url();
        if (url[0] == '\0') {
            ESP_LOGW(TAG, "ToDo enabled but no URL configured");
        } else {
            ESP_LOGI(TAG, "Free internal heap before ToDo fetch: %u bytes",
                    (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            bool ok = (todo_fetch(url, 0, todo) == ESP_OK);
            utils_record_internet_attempt(ok);
            have_todo = ok;
            if (!ok) {
                ESP_LOGW(TAG, "ToDo fetch failed, that column will be omitted this cycle");
            }
        }
    }

    esp_err_t result;
    if (!have_todo && !have_events_a && !have_events_b) {
        ESP_LOGW(TAG, "Nothing to render this agenda cycle (no source fetched successfully)");
        result = ESP_FAIL;
    } else {
        result = agenda_renderer_render(have_todo ? todo : NULL, have_events_a ? events_a : NULL,
                                        have_events_b ? events_b : NULL, cal_days,
                                        AGENDA_OUTPUT_PATH, IMAGE_FORMAT_PNG);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "Failed to render agenda screen: %s", esp_err_to_name(result));
        } else {
            result = display_manager_show_image(AGENDA_OUTPUT_PATH);
        }
    }

    heap_caps_free(todo);
    heap_caps_free(events_a);
    heap_caps_free(events_b);
    return result;
}
