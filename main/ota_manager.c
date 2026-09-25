#include "ota_manager.h"

#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "board_hal.h"
#include "cJSON.h"
#include "chime.h"
#include "config.h"
#include "config_manager.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ha_integration.h"
#include "http_fetch.h"
#include "nvs.h"
#include "periodic_tasks.h"
#include "power_manager.h"

static const char *TAG = "ota_manager";
#define OTA_NVS_NAMESPACE "ota"
#define OTA_NVS_LATEST_VERSION_KEY "latest_ver"
#define OTA_NVS_STATE_KEY "state"
#define OTA_NVS_CHANNEL_KEY "channel"
#define OTA_NVS_ALARM_KEY "alarm"
#define OTA_CHECK_INTERVAL_SECONDS (24 * 60 * 60)  // 24 hours

static ota_status_t ota_status = {.state = OTA_STATE_IDLE,
                                  .current_version = "",
                                  .latest_version = "",
                                  .error_message = "",
                                  .progress_percent = 0};

#ifdef CONFIG_ALARM_CLOCK_ENABLED
#define RUNNING_ALARMCLOCK true
#else
#define RUNNING_ALARMCLOCK false
#endif

// Which release / firmware variant the next check uses (persisted, see
// ota_set_options()). The variant defaults to whatever this build already is,
// so an Alarm Clock build keeps updating to the Alarm Clock firmware.
static ota_channel_t s_channel = OTA_CHANNEL_STABLE;
static bool s_alarm_variant = RUNNING_ALARMCLOCK;

static SemaphoreHandle_t ota_status_mutex = NULL;
static bool update_available = false;
static char firmware_url[256] = "";

// Forward declarations
static void ota_save_status_to_nvs(void);
static void ota_load_status_from_nvs(void);
static void ota_load_options_from_nvs(void);
static esp_err_t ota_check_periodic_callback(void);

static void set_ota_state(ota_state_t state, const char *error_msg)
{
    if (ota_status_mutex && xSemaphoreTake(ota_status_mutex, portMAX_DELAY) == pdTRUE) {
        ota_status.state = state;
        if (error_msg) {
            snprintf(ota_status.error_message, sizeof(ota_status.error_message), "%s", error_msg);
        } else {
            ota_status.error_message[0] = '\0';
        }
        xSemaphoreGive(ota_status_mutex);
    }
}

static int version_compare(const char *v1, const char *v2)
{
    // Simple version comparison
    // Handles formats like "v1.2.3" or "1.2.3" or "dev-abc123"

    // Skip 'v' prefix if present
    if (v1[0] == 'v')
        v1++;
    if (v2[0] == 'v')
        v2++;

    // Dev versions are always considered older than release versions
    bool v1_is_dev = (strncmp(v1, "dev-", 4) == 0);
    bool v2_is_dev = (strncmp(v2, "dev-", 4) == 0);

    if (v1_is_dev && !v2_is_dev) {
        return -1;  // v1 (dev) is older than v2 (release)
    }
    if (!v1_is_dev && v2_is_dev) {
        return 1;  // v1 (release) is newer than v2 (dev)
    }
    if (v1_is_dev && v2_is_dev) {
        return strcmp(v1, v2);  // Both dev, compare strings
    }

    // Parse version numbers for release versions
    int v1_major = 0, v1_minor = 0, v1_patch = 0;
    int v2_major = 0, v2_minor = 0, v2_patch = 0;

    sscanf(v1, "%d.%d.%d", &v1_major, &v1_minor, &v1_patch);
    sscanf(v2, "%d.%d.%d", &v2_major, &v2_minor, &v2_patch);

    if (v1_major != v2_major)
        return v1_major - v2_major;
    if (v1_minor != v2_minor)
        return v1_minor - v2_minor;
    return v1_patch - v2_patch;
}

static bool want_alarm_variant(void)
{
    return s_alarm_variant && board_hal_has_speaker();
}

static esp_err_t fetch_github_release_info(char *latest_version, size_t version_len,
                                           char *download_url, size_t url_len, bool *prerelease_out)
{
    esp_err_t err = ESP_FAIL;
    char *response_buffer = NULL;

    // GitHub's releases/latest API can respond with Transfer-Encoding: chunked
    // rather than a fixed Content-Length - a fixed-length read (the previous
    // implementation here) then sees Content-Length 0 and fails every time.
    // http_fetch_get() accumulates the body via the HTTP client's own
    // event-driven callback regardless of encoding (already proven against
    // this exact class of API by weather.c/headlines.c) - reuse it instead of
    // a second, more fragile fetch implementation. This project's own release
    // (14 assets - 7 boards x merged+OTA binary; 18 with the Alarm Clock
    // variant) measured 34 KB of response
    // JSON (GitHub's per-asset metadata, e.g. the uploader object, is
    // verbose) - 64 KB leaves real headroom for more assets later.
    size_t response_len = 0;
    err =
        http_fetch_get(s_channel == OTA_CHANNEL_PRERELEASE ? GITHUB_API_URL_NEWEST : GITHUB_API_URL,
                       10000, 64 * 1024, &response_buffer, &response_len, NULL, "ESP32-PhotoFrame");
    if (err != ESP_OK || !response_buffer) {
        ESP_LOGE(TAG, "Failed to fetch release info: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }

    // Parse JSON response
    cJSON *json = cJSON_Parse(response_buffer);
    if (json == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        err = ESP_FAIL;
        goto cleanup;
    }

    // /releases/latest returns the release object, /releases?per_page=1 an
    // array holding it.
    cJSON *release = json;
    if (cJSON_IsArray(json)) {
        release = cJSON_GetArrayItem(json, 0);
        if (release == NULL) {
            ESP_LOGE(TAG, "No release in response");
            cJSON_Delete(json);
            err = ESP_FAIL;
            goto cleanup;
        }
    }

    // Get tag_name (version)
    cJSON *tag_name = cJSON_GetObjectItem(release, "tag_name");
    if (tag_name == NULL || !cJSON_IsString(tag_name)) {
        ESP_LOGE(TAG, "tag_name not found in response");
        cJSON_Delete(json);
        err = ESP_FAIL;
        goto cleanup;
    }

    snprintf(latest_version, version_len, "%s", tag_name->valuestring);
    if (prerelease_out) {
        *prerelease_out = cJSON_IsTrue(cJSON_GetObjectItem(release, "prerelease"));
    }

    // Get assets array and find .bin file
    cJSON *assets = cJSON_GetObjectItem(release, "assets");
    if (assets == NULL || !cJSON_IsArray(assets)) {
        ESP_LOGE(TAG, "assets not found in response");
        cJSON_Delete(json);
        err = ESP_FAIL;
        goto cleanup;
    }

    bool found_binary = false;
    cJSON *asset = NULL;

    const char *board_name = BOARD_HAL_NAME;

    // The Alarm Clock firmware is a separate "-alarmclock" release asset. By
    // default a build updates to its own variant (an Alarm Clock build must not
    // silently lose the feature); the Web UI can pick the other one.
    const char *variant_suffix = want_alarm_variant() ? "-alarmclock" : "";
    char target_binary[80];
    snprintf(target_binary, sizeof(target_binary), "esp32-photoframe-%s%s.bin", board_name,
             variant_suffix);
    ESP_LOGI(TAG, "Searching for board-specific OTA binary: %s", target_binary);

    cJSON_ArrayForEach(asset, assets)
    {
        cJSON *name = cJSON_GetObjectItem(asset, "name");
        if (name && cJSON_IsString(name)) {
            const char *asset_name = name->valuestring;
            // Look for board-specific binary
            if (strcmp(asset_name, target_binary) == 0) {
                cJSON *browser_download_url = cJSON_GetObjectItem(asset, "browser_download_url");
                if (browser_download_url && cJSON_IsString(browser_download_url)) {
                    snprintf(download_url, url_len, "%s", browser_download_url->valuestring);
                    found_binary = true;
                    ESP_LOGI(TAG, "Found firmware binary: %s", asset_name);
                    break;
                }
            }
        }
    }

    cJSON_Delete(json);

    if (!found_binary) {
        ESP_LOGE(TAG, "No .bin file found in release assets");
        err = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }

    err = ESP_OK;
    ESP_LOGI(TAG, "Latest version: %s", latest_version);
    ESP_LOGI(TAG, "Download URL: %s", download_url);

cleanup:
    free(response_buffer);
    return err;
}

static void ota_check_task(void *pvParameter)
{
    // pvParameter is a boolean: true = notify HA, false/NULL = don't notify
    bool notify_ha = (pvParameter != NULL);

    ESP_LOGI(TAG, "Checking for firmware updates...");

    set_ota_state(OTA_STATE_CHECKING, NULL);

    char latest_version[32] = {0};
    char download_url[256] = {0};

    bool prerelease = false;
    esp_err_t err = fetch_github_release_info(latest_version, sizeof(latest_version), download_url,
                                              sizeof(download_url), &prerelease);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to fetch release info");
        if (err == ESP_ERR_NOT_FOUND) {
            set_ota_state(OTA_STATE_ERROR, want_alarm_variant()
                                               ? "This release has no Alarm Clock firmware yet"
                                               : "This release has no firmware for this board");
        } else {
            set_ota_state(OTA_STATE_ERROR, "Failed to check for updates");
        }
        vTaskDelete(NULL);
        return;
    }

    // Installing the found release also changes the firmware variant?
    bool variant_switch = (want_alarm_variant() != RUNNING_ALARMCLOCK);

    // Store latest version and URL
    if (ota_status_mutex && xSemaphoreTake(ota_status_mutex, portMAX_DELAY) == pdTRUE) {
        snprintf(ota_status.latest_version, sizeof(ota_status.latest_version), "%s",
                 latest_version);
        ota_status.latest_prerelease = prerelease;
        ota_status.variant_switch = variant_switch;
        xSemaphoreGive(ota_status_mutex);
    }
    snprintf(firmware_url, sizeof(firmware_url), "%s", download_url);

    // Compare versions. Same version but the other variant (Alarm Clock <->
    // regular) is offered too; an older release is never offered as a "switch".
    int cmp = version_compare(ota_status.current_version, latest_version);

    if (cmp < 0 || (cmp == 0 && variant_switch)) {
        ESP_LOGI(TAG, "Update available: %s -> %s%s%s", ota_status.current_version, latest_version,
                 prerelease ? " (pre-release)" : "", variant_switch ? " (variant switch)" : "");
        update_available = true;
        set_ota_state(OTA_STATE_UPDATE_AVAILABLE, NULL);
    } else {
        ESP_LOGI(TAG, "Already on latest version: %s", ota_status.current_version);
        update_available = false;
        set_ota_state(OTA_STATE_IDLE, NULL);
    }

    // Update last check time after successful check
    ota_update_last_check_time();

    // Save OTA status to NVS for persistence across reboots
    ota_save_status_to_nvs();

    // Notify HA if requested
    if (notify_ha) {
        ESP_LOGI(TAG, "Notifying HA of OTA status update");
        ha_notify_update();
    }

    vTaskDelete(NULL);
}

static void ota_update_task(void *pvParameter)
{
    ESP_LOGI(TAG, "Starting OTA update...");

    // Reset sleep timer to prevent auto-sleep during OTA
    power_manager_reset_sleep_timer();

    set_ota_state(OTA_STATE_DOWNLOADING, NULL);
    if (ota_status_mutex && xSemaphoreTake(ota_status_mutex, portMAX_DELAY) == pdTRUE) {
        ota_status.progress_percent = 0;
        xSemaphoreGive(ota_status_mutex);
    }

    esp_http_client_config_t config = {
        .url = firmware_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
        .buffer_size = 8192,
        .buffer_size_tx = 4096,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };

    esp_https_ota_handle_t https_ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &https_ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
        set_ota_state(OTA_STATE_ERROR, "Failed to start OTA update");
        vTaskDelete(NULL);
        return;
    }

    int image_size = esp_https_ota_get_image_size(https_ota_handle);
    ESP_LOGI(TAG, "OTA image size: %d bytes", image_size);

    set_ota_state(OTA_STATE_INSTALLING, NULL);

    while (1) {
        err = esp_https_ota_perform(https_ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }

        int downloaded = esp_https_ota_get_image_len_read(https_ota_handle);
        if (image_size > 0) {
            int progress = (downloaded * 100) / image_size;
            if (ota_status_mutex && xSemaphoreTake(ota_status_mutex, portMAX_DELAY) == pdTRUE) {
                ota_status.progress_percent = progress;
                xSemaphoreGive(ota_status_mutex);
            }
            ESP_LOGI(TAG, "OTA progress: %d%%", progress);
        }

        // Reset sleep timer periodically during OTA to prevent auto-sleep
        power_manager_reset_sleep_timer();

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA perform failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(https_ota_handle);
        set_ota_state(OTA_STATE_ERROR, "OTA update failed");
        vTaskDelete(NULL);
        return;
    }

    err = esp_https_ota_finish(https_ota_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Image validation failed");
            set_ota_state(OTA_STATE_ERROR, "Firmware validation failed");
        } else {
            ESP_LOGE(TAG, "OTA finish failed: %s", esp_err_to_name(err));
            set_ota_state(OTA_STATE_ERROR, "Failed to finalize OTA update");
        }
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "OTA update successful! Rebooting in 3 seconds...");
    set_ota_state(OTA_STATE_SUCCESS, NULL);
    if (ota_status_mutex && xSemaphoreTake(ota_status_mutex, portMAX_DELAY) == pdTRUE) {
        ota_status.progress_percent = 100;
        xSemaphoreGive(ota_status_mutex);
    }

    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();

    vTaskDelete(NULL);
}

esp_err_t ota_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing OTA manager");

    // Zero out the entire ota_status struct to prevent garbage data
    memset(&ota_status, 0, sizeof(ota_status_t));
    ota_status.state = OTA_STATE_IDLE;

    // Create mutex for ota_status protection
    ota_status_mutex = xSemaphoreCreateMutex();
    if (ota_status_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create OTA status mutex");
        return ESP_ERR_NO_MEM;
    }

    // Get current firmware version
    const esp_app_desc_t *app_desc = esp_app_get_description();
    snprintf(ota_status.current_version, sizeof(ota_status.current_version), "%s",
             app_desc->version);

    ESP_LOGI(TAG, "Current firmware version: %s", ota_status.current_version);

    // Load last known OTA status from NVS (latest_version, state)
    ota_load_status_from_nvs();
    ota_load_options_from_nvs();

    // Mark current partition as valid (for rollback support)
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "First boot after OTA update, marking as valid");
            esp_ota_mark_app_valid_cancel_rollback();
            // Post-reboot, on the new firmware - board_hal_init() (and thus
            // the speaker hardware) already ran earlier in app_main().
            chime_play_if_enabled(CHIME_EVENT_OTA_SUCCESS);
        }
    }

    // Register OTA check as a periodic task (24 hours)
    esp_err_t err = periodic_tasks_register(OTA_CHECK_TASK_NAME, ota_check_periodic_callback,
                                            OTA_CHECK_INTERVAL_SECONDS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register OTA periodic task: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

esp_err_t ota_check_for_update(bool *update_available_out, int timeout)
{
    if (ota_status.state == OTA_STATE_CHECKING || ota_status.state == OTA_STATE_DOWNLOADING ||
        ota_status.state == OTA_STATE_INSTALLING) {
        return ESP_ERR_INVALID_STATE;
    }

    update_available = false;
    // Enter CHECKING here rather than in the task: the wait loop below (and the
    // caller's HTTP response) would otherwise see the old state before the
    // task has run and report "no update" straight away.
    set_ota_state(OTA_STATE_CHECKING, NULL);
    xTaskCreate(&ota_check_task, "ota_check_task", 12288, NULL, 5, NULL);

    // Wait for check to complete (with timeout)
    while (timeout > 0 && ota_status.state == OTA_STATE_CHECKING) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        timeout--;
    }

    if (update_available_out) {
        *update_available_out = update_available;
    }

    return ESP_OK;
}

esp_err_t ota_start_update(void)
{
    if (!update_available) {
        ESP_LOGW(TAG, "No update available");
        return ESP_ERR_INVALID_STATE;
    }

    if (ota_status.state == OTA_STATE_DOWNLOADING || ota_status.state == OTA_STATE_INSTALLING) {
        ESP_LOGW(TAG, "Update already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    xTaskCreate(&ota_update_task, "ota_update_task", 12288, NULL, 5, NULL);

    return ESP_OK;
}

void ota_get_status(ota_status_t *status)
{
    if (status && ota_status_mutex) {
        if (xSemaphoreTake(ota_status_mutex, portMAX_DELAY) == pdTRUE) {
            memcpy(status, &ota_status, sizeof(ota_status_t));
            xSemaphoreGive(ota_status_mutex);
        }
    }
}

const char *ota_get_current_version(void)
{
    return ota_status.current_version;
}

bool ota_should_check_daily(void)
{
    return periodic_tasks_should_run(OTA_CHECK_TASK_NAME);
}

void ota_update_last_check_time(void)
{
    periodic_tasks_update_last_run(OTA_CHECK_TASK_NAME);
}

static esp_err_t ota_check_periodic_callback(void)
{
    if (!config_manager_get_ota_check_enabled()) {
        ESP_LOGI(TAG, "Automatic OTA check disabled, skipping periodic check");
        // Still counts as "run" so it doesn't retry every wake while disabled.
        ota_update_last_check_time();
        return ESP_OK;
    }

    if (ota_status.state == OTA_STATE_CHECKING || ota_status.state == OTA_STATE_DOWNLOADING ||
        ota_status.state == OTA_STATE_INSTALLING) {
        // ota_check_for_update()/ota_start_update() both refuse to start a
        // second check/update while one is already in progress, but this
        // periodic path used to skip that guard entirely and could spawn a
        // second concurrent ota_check_task, corrupting shared state
        // (ota_status, firmware_url) mid-update. Skip this cycle instead;
        // the next periodic tick retries.
        ESP_LOGI(TAG, "OTA check/update already in progress, skipping periodic check");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Periodic OTA check triggered");

    // Check for updates without notifying HA (HA will poll for status)
    xTaskCreate(&ota_check_task, "ota_check_task", 12288, NULL, 5, NULL);

    return ESP_OK;
}

static void ota_load_options_from_nvs(void)
{
    s_channel = OTA_CHANNEL_STABLE;
    s_alarm_variant = RUNNING_ALARMCLOCK;

    nvs_handle_t nvs_handle;
    if (nvs_open(OTA_NVS_NAMESPACE, NVS_READONLY, &nvs_handle) != ESP_OK) {
        return;
    }
    uint8_t value = 0;
    if (nvs_get_u8(nvs_handle, OTA_NVS_CHANNEL_KEY, &value) == ESP_OK &&
        value <= OTA_CHANNEL_PRERELEASE) {
        s_channel = (ota_channel_t) value;
    }
    if (nvs_get_u8(nvs_handle, OTA_NVS_ALARM_KEY, &value) == ESP_OK) {
        s_alarm_variant = (value != 0);
    }
    nvs_close(nvs_handle);
}

void ota_get_options(ota_options_t *out)
{
    out->channel = s_channel;
    out->alarmclock = want_alarm_variant();
    out->alarmclock_available = board_hal_has_speaker();
    out->running_alarmclock = RUNNING_ALARMCLOCK;
}

esp_err_t ota_set_options(ota_channel_t channel, bool alarmclock)
{
    if (channel != OTA_CHANNEL_STABLE && channel != OTA_CHANNEL_PRERELEASE) {
        return ESP_ERR_INVALID_ARG;
    }
    if (alarmclock && !board_hal_has_speaker()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (ota_status.state == OTA_STATE_CHECKING || ota_status.state == OTA_STATE_DOWNLOADING ||
        ota_status.state == OTA_STATE_INSTALLING) {
        return ESP_ERR_INVALID_STATE;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(OTA_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(nvs_handle, OTA_NVS_CHANNEL_KEY, (uint8_t) channel);
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs_handle, OTA_NVS_ALARM_KEY, alarmclock ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    s_channel = channel;
    s_alarm_variant = alarmclock;

    // The last check answered a different question - forget it.
    update_available = false;
    if (ota_status_mutex && xSemaphoreTake(ota_status_mutex, portMAX_DELAY) == pdTRUE) {
        ota_status.latest_version[0] = '\0';
        ota_status.latest_prerelease = false;
        ota_status.variant_switch = false;
        xSemaphoreGive(ota_status_mutex);
    }
    set_ota_state(OTA_STATE_IDLE, NULL);
    ota_save_status_to_nvs();
    return ESP_OK;
}

static void ota_save_status_to_nvs(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(OTA_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for saving OTA status: %s", esp_err_to_name(err));
        return;
    }

    // Save latest_version and state
    if (ota_status_mutex && xSemaphoreTake(ota_status_mutex, portMAX_DELAY) == pdTRUE) {
        err = nvs_set_str(nvs_handle, OTA_NVS_LATEST_VERSION_KEY, ota_status.latest_version);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save latest_version to NVS: %s", esp_err_to_name(err));
        }

        err = nvs_set_u8(nvs_handle, OTA_NVS_STATE_KEY, (uint8_t) ota_status.state);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save state to NVS: %s", esp_err_to_name(err));
        }

        xSemaphoreGive(ota_status_mutex);
    }

    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
}

static void ota_load_status_from_nvs(void)
{
    // Initialize to safe defaults first
    ota_status.latest_version[0] = '\0';
    ota_status.state = OTA_STATE_IDLE;

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(OTA_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No saved OTA status in NVS (first boot or cleared), using defaults");
        return;
    }

    // Load latest_version
    size_t required_size = sizeof(ota_status.latest_version);
    err = nvs_get_str(nvs_handle, OTA_NVS_LATEST_VERSION_KEY, ota_status.latest_version,
                      &required_size);
    if (err != ESP_OK) {
        ota_status.latest_version[0] = '\0';
    }

    // Load state
    uint8_t saved_state = 0;
    err = nvs_get_u8(nvs_handle, OTA_NVS_STATE_KEY, &saved_state);
    if (err == ESP_OK) {
        ota_status.state = (ota_state_t) saved_state;
    } else {
        ota_status.state = OTA_STATE_IDLE;
    }

    nvs_close(nvs_handle);
}
