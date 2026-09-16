#include "climate_history.h"

#include <stdbool.h>
#include <stdio.h>
#include <time.h>

#include "climate.h"
#include "config.h"
#include "config_manager.h"
#include "esp_log.h"
#include "storage.h"

static const char *TAG = "climate_history";

// Peeks the timestamp of the first (oldest) line in the history file -
// same shape as battery_history.c's own peek_oldest_timestamp().
static bool peek_oldest_timestamp(time_t *out_ts)
{
    FILE *f = fopen(CLIMATE_HISTORY_PATH, "r");
    if (!f) {
        return false;
    }
    char line[64];
    bool found = false;
    if (fgets(line, sizeof(line), f)) {
        long long ts = 0;
        if (sscanf(line, "%lld,", &ts) == 1) {
            *out_ts = (time_t) ts;
            found = true;
        }
    }
    fclose(f);
    return found;
}

// Copies the about-to-be-discarded history log to a uniquely named file
// (named after the date range it actually covers) before the age-based
// reset below removes the original - same approach as
// battery_history.c's backup_history_before_reset(). Best-effort: any
// failure here just means no backup, never blocks the reset itself.
static void backup_history_before_reset(void)
{
    FILE *src = fopen(CLIMATE_HISTORY_PATH, "r");
    if (!src) {
        return;
    }
    char line[64];
    time_t oldest = 0, newest = 0;
    bool have_range = false;
    while (fgets(line, sizeof(line), src)) {
        long long ts = 0;
        if (sscanf(line, "%lld,", &ts) == 1) {
            if (!have_range) {
                oldest = (time_t) ts;
                have_range = true;
            }
            newest = (time_t) ts;
        }
    }
    fclose(src);
    if (!have_range) {
        return;
    }

    struct tm tm_old, tm_new;
    localtime_r(&oldest, &tm_old);
    localtime_r(&newest, &tm_new);
    char dst_path[96];
    snprintf(dst_path, sizeof(dst_path),
             FS_MOUNT_POINT "/climate_history_backup_%04d%02d%02d-%04d%02d%02d.csv",
             tm_old.tm_year + 1900, tm_old.tm_mon + 1, tm_old.tm_mday, tm_new.tm_year + 1900,
             tm_new.tm_mon + 1, tm_new.tm_mday);

    src = fopen(CLIMATE_HISTORY_PATH, "r");
    if (!src) {
        return;
    }
    FILE *dst = fopen(dst_path, "w");
    if (!dst) {
        ESP_LOGW(TAG, "Failed to create climate history backup at %s", dst_path);
        fclose(src);
        return;
    }
    while (fgets(line, sizeof(line), src)) {
        fputs(line, dst);
    }
    fclose(src);
    fclose(dst);
    ESP_LOGI(TAG, "Climate history backed up to %s before reset", dst_path);
}

void climate_history_record(void)
{
    if (!config_manager_get_climate_logging_enabled()) {
        return;
    }

    // Shared debounce for the two callers (every wake, main.c; every
    // CLIMATE_ACTIVE_LOG_INTERVAL_SEC while always-on, power_manager.c) -
    // persisted in NVS (not a static/RTC variable) since a deep-sleep wake
    // wipes RAM between calls. Checked before the sensor read itself so a
    // wake that's skipped doesn't even wake the SHTC3 for nothing.
    time_t now = time(NULL);
    int64_t last_log = config_manager_get_climate_last_log_time();
    if (last_log != 0 && difftime(now, (time_t) last_log) < CLIMATE_LOG_MIN_INTERVAL_SEC) {
        return;
    }

    float temp_c, humidity;
    if (climate_read_temperature(&temp_c) != ESP_OK || climate_read_humidity(&humidity) != ESP_OK) {
        return;  // No sensor on this board, or a transient read error.
    }
    if (!storage_has_persistent_storage()) {
        return;
    }

    time_t oldest;
    if (peek_oldest_timestamp(&oldest)) {
        double age_days = difftime(now, oldest) / 86400.0;
        if (age_days > CLIMATE_HISTORY_MAX_AGE_DAYS) {
            if (config_manager_get_climate_history_backup_enabled()) {
                backup_history_before_reset();
            }
            remove(CLIMATE_HISTORY_PATH);
            ESP_LOGI(TAG, "Climate history reset (log too old)");
        }
    }

    FILE *f = fopen(CLIMATE_HISTORY_PATH, "a");
    if (!f) {
        ESP_LOGW(TAG, "Failed to open climate history for append");
        return;
    }
    fprintf(f, "%lld,%.1f,%.1f\n", (long long) now, (double) temp_c, (double) humidity);
    fclose(f);

    config_manager_set_climate_last_log_time((int64_t) now);
}

cJSON *climate_history_build_json(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON *entries = cJSON_AddArrayToObject(root, "entries");
    if (!entries) {
        cJSON_Delete(root);
        return NULL;
    }

    climate_room_type_t room = config_manager_get_climate_room_type();

    if (storage_has_persistent_storage()) {
        FILE *f = fopen(CLIMATE_HISTORY_PATH, "r");
        if (f) {
            char line[64];
            while (fgets(line, sizeof(line), f)) {
                long long ts = 0;
                float temp_c = 0, humidity = 0;
                if (sscanf(line, "%lld,%f,%f", &ts, &temp_c, &humidity) == 3) {
                    cJSON *e = cJSON_CreateObject();
                    if (e) {
                        cJSON_AddNumberToObject(e, "t", (double) ts);
                        cJSON_AddNumberToObject(e, "temp_c", temp_c);
                        cJSON_AddNumberToObject(e, "hum", humidity);
                        cJSON_AddNumberToObject(e, "tcat",
                                                climate_classify_temperature(temp_c, room));
                        cJSON_AddNumberToObject(e, "hcat",
                                                climate_classify_humidity(humidity, room));
                        cJSON_AddItemToArray(entries, e);
                    }
                }
            }
            fclose(f);
        }
    }

    return root;
}

void climate_history_reset(void)
{
    if (remove(CLIMATE_HISTORY_PATH) == 0) {
        ESP_LOGI(TAG, "Climate history reset (user-requested)");
    }
}
