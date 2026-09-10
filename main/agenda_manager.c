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

    todo_list_t todo = {0};
    bool have_todo = false;
    if (want_todo) {
        const char *url = config_manager_get_agenda_todo_url();
        if (url[0] == '\0') {
            ESP_LOGW(TAG, "ToDo enabled but no URL configured");
        } else {
            bool ok = (todo_fetch(url, 0, &todo) == ESP_OK);
            utils_record_internet_attempt(ok);
            have_todo = ok;
            if (!ok) {
                ESP_LOGW(TAG, "ToDo fetch failed, that column will be omitted this cycle");
            }
        }
    }

    ics_event_list_t events = {0};
    bool have_events = false;
    int cal_days = config_manager_get_agenda_cal_days();
    if (want_cal) {
        const char *url = config_manager_get_agenda_cal_url();
        if (url[0] == '\0') {
            ESP_LOGW(TAG, "Calendar enabled but no URL configured");
        } else {
            time_t now = time(NULL);
            time_t window_end = now + (time_t) cal_days * 86400;
            bool ok = (calendar_ics_fetch(url, 0, now, window_end, &events) == ESP_OK);
            utils_record_internet_attempt(ok);
            have_events = ok;
            if (!ok) {
                ESP_LOGW(TAG, "Calendar fetch failed, that column will be omitted this cycle");
            }
        }
    }

    if (!have_todo && !have_events) {
        ESP_LOGW(TAG, "Nothing to render this agenda cycle (no source fetched successfully)");
        return ESP_FAIL;
    }

    esp_err_t err = agenda_renderer_render(have_todo ? &todo : NULL, have_events ? &events : NULL,
                                           cal_days, AGENDA_OUTPUT_PATH, IMAGE_FORMAT_PNG);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to render agenda screen: %s", esp_err_to_name(err));
        return err;
    }

    return display_manager_show_image(AGENDA_OUTPUT_PATH);
}
