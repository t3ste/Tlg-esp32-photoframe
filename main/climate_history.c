#include "climate_history.h"

#include <stdbool.h>
#include <stdio.h>
#include <time.h>

#include "board_hal.h"
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

void climate_history_record(void)
{
    if (!config_manager_get_climate_logging_enabled()) {
        return;
    }
    float temp_c, humidity;
    if (board_hal_get_temperature(&temp_c) != ESP_OK ||
        board_hal_get_humidity(&humidity) != ESP_OK) {
        return;  // No sensor on this board, or a transient read error.
    }
    if (!storage_has_persistent_storage()) {
        return;
    }

    time_t oldest;
    if (peek_oldest_timestamp(&oldest)) {
        double age_days = difftime(time(NULL), oldest) / 86400.0;
        if (age_days > CLIMATE_HISTORY_MAX_AGE_DAYS) {
            remove(CLIMATE_HISTORY_PATH);
            ESP_LOGI(TAG, "Climate history reset (log too old)");
        }
    }

    FILE *f = fopen(CLIMATE_HISTORY_PATH, "a");
    if (!f) {
        ESP_LOGW(TAG, "Failed to open climate history for append");
        return;
    }
    fprintf(f, "%lld,%.1f,%.1f\n", (long long) time(NULL), (double) temp_c, (double) humidity);
    fclose(f);
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
