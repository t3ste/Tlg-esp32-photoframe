#include "mic_monitor.h"

#include <string.h>

#include "board_hal.h"
#include "mic_level.h"

#if !BOARD_HAL_VOICE_ENABLED

// Compiled out to stubs unless this is an Alarm Clock build on a board with speaker + microphone.
bool mic_monitor_available(void)
{
    return false;
}

esp_err_t mic_monitor_start(uint32_t seconds, bool play_tones)
{
    (void) seconds;
    (void) play_tones;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t mic_monitor_play_tones(uint8_t volume_percent)
{
    (void) volume_percent;
    return ESP_ERR_NOT_SUPPORTED;
}

void mic_monitor_get_status(mic_monitor_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->rms_dbfs = MIC_LEVEL_FLOOR_DBFS;
    out->peak_dbfs = MIC_LEVEL_FLOOR_DBFS;
    out->mic_dbfs = MIC_LEVEL_FLOOR_DBFS;
    out->mic2_dbfs = MIC_LEVEL_FLOOR_DBFS;
    out->floor_dbfs = MIC_LEVEL_FLOOR_DBFS;
    out->threshold_dbfs = MIC_THRESHOLD_DEFAULT_DBFS;
    out->auto_threshold = true;
}

void mic_monitor_stop(void) {}

void mic_monitor_get_settings(mic_settings_t *out)
{
    out->auto_threshold = true;
    out->threshold_dbfs = MIC_THRESHOLD_DEFAULT_DBFS;
}

esp_err_t mic_monitor_set_settings(bool auto_threshold, int threshold_dbfs)
{
    (void) auto_threshold;
    (void) threshold_dbfs;
    return ESP_ERR_NOT_SUPPORTED;
}

#else

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mic_detect.h"
#include "nvs.h"
#include "power_manager.h"

static const char *TAG = "mic";

#define WINDOW_FRAMES 3200  // 200 ms at 16 kHz -> five lines per second
#define BAR_WIDTH 30
#define BAR_FLOOR_DBFS (-60.0f)

// 1.2 s of silence first (the listening side sets its noise floor meanwhile),
// then MIC_MONITOR_TEST_BURSTS beeps with pauses. Two pitches so a steady
// hum can't pass for it.
static const board_hal_note_t TEST_TONES[] = {
    {0.0f, 1200},   {1000.0f, 400}, {0.0f, 600},    {1000.0f, 400}, {0.0f, 600},
    {1500.0f, 400}, {0.0f, 600},    {1500.0f, 400}, {0.0f, 600},
};
#define TEST_TONE_COUNT ((int) (sizeof(TEST_TONES) / sizeof(TEST_TONES[0])))

static volatile bool s_running = false;
static volatile bool s_tones_running = false;
static bool s_play_tones = false;
static volatile float s_rms_dbfs = MIC_LEVEL_FLOOR_DBFS;
static volatile float s_peak_dbfs = MIC_LEVEL_FLOOR_DBFS;
static volatile float s_mic_dbfs = MIC_LEVEL_FLOOR_DBFS;
static volatile float s_mic2_dbfs = MIC_LEVEL_FLOOR_DBFS;
static volatile float s_floor_dbfs = MIC_LEVEL_FLOOR_DBFS;
static volatile bool s_stop = false;

// Sensitivity settings, persisted in their own small NVS namespace.
#define MIC_NVS_NAMESPACE "mic"
#define MIC_NVS_AUTO_KEY "auto"
#define MIC_NVS_THRESHOLD_KEY "thr"
static bool s_settings_loaded = false;
static mic_settings_t s_settings = {.auto_threshold = true,
                                    .threshold_dbfs = MIC_THRESHOLD_DEFAULT_DBFS};

static void load_settings(void)
{
    if (s_settings_loaded) {
        return;
    }
    s_settings_loaded = true;
    nvs_handle_t h;
    if (nvs_open(MIC_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t auto_v = 1;
    int8_t thr = MIC_THRESHOLD_DEFAULT_DBFS;
    if (nvs_get_u8(h, MIC_NVS_AUTO_KEY, &auto_v) == ESP_OK) {
        s_settings.auto_threshold = auto_v != 0;
    }
    if (nvs_get_i8(h, MIC_NVS_THRESHOLD_KEY, &thr) == ESP_OK && thr >= MIC_THRESHOLD_MIN_DBFS &&
        thr <= MIC_THRESHOLD_MAX_DBFS) {
        s_settings.threshold_dbfs = thr;
    }
    nvs_close(h);
}
static mic_monitor_status_t s_result;  // written once per finished run

typedef struct {
    mic_level_acc_t acc;
    mic_floor_t floor;  // noise floor of microphone 1, for the live meter
    mic_detect_t mic;   // left channel (microphone)
    mic_detect_t mic2;  // right channel (second microphone)
    float loudest_peak_dbfs;
} monitor_ctx_t;

static bool on_block(const int16_t *samples, size_t frames, void *user)
{
    monitor_ctx_t *ctx = user;
    mic_level_acc_add(&ctx->acc, samples, frames);
    if (ctx->acc.frames < WINDOW_FRAMES) {
        return true;
    }

    mic_level_t level = mic_level_acc_result(&ctx->acc);
    mic_level_t left = mic_level_acc_channel(&ctx->acc, 0);
    mic_level_t right = mic_level_acc_channel(&ctx->acc, 1);
    mic_level_acc_reset(&ctx->acc);
    mic_detect_add_window(&ctx->mic, left.rms_dbfs);
    mic_detect_add_window(&ctx->mic2, right.rms_dbfs);
    mic_floor_update(&ctx->floor, left.rms_dbfs);
    s_mic_dbfs = left.rms_dbfs;
    s_mic2_dbfs = right.rms_dbfs;
    s_floor_dbfs = ctx->floor.floor_dbfs;

    s_rms_dbfs = level.rms_dbfs;
    s_peak_dbfs = level.peak_dbfs;
    if (level.peak_dbfs > ctx->loudest_peak_dbfs) {
        ctx->loudest_peak_dbfs = level.peak_dbfs;
    }

    char bar[BAR_WIDTH + 3];
    mic_level_bar(level.rms_dbfs, BAR_FLOOR_DBFS, bar, BAR_WIDTH);
    ESP_LOGI(TAG, "%s %6.1f dBFS  (peak %6.1f)  mic %6.1f  mic2 %6.1f", bar,
             (double) level.rms_dbfs, (double) level.peak_dbfs, (double) left.rms_dbfs,
             (double) right.rms_dbfs);

    power_manager_reset_sleep_timer();  // don't auto-sleep mid-test
    return !s_stop;
}

static void monitor_task(void *arg)
{
    uint32_t seconds = (uint32_t) (uintptr_t) arg;
    const bool play = s_play_tones;
    ESP_LOGI(TAG, "Microphone level monitor for %u s%s - make some noise (bar: %d..0 dBFS)",
             (unsigned) seconds, play ? " with speaker test tones at 100 %" : "",
             (int) BAR_FLOOR_DBFS);

    monitor_ctx_t ctx;
    mic_level_acc_reset(&ctx.acc);
    mic_floor_init(&ctx.floor);
    mic_detect_init(&ctx.mic);
    mic_detect_init(&ctx.mic2);
    load_settings();
    if (!s_settings.auto_threshold) {
        mic_detect_set_manual(&ctx.mic, (float) s_settings.threshold_dbfs);
        mic_detect_set_manual(&ctx.mic2, (float) s_settings.threshold_dbfs);
    }
    ctx.loudest_peak_dbfs = MIC_LEVEL_FLOOR_DBFS;

    esp_err_t err = play ? board_hal_mic_capture_with_tones(seconds * 1000u, on_block, &ctx,
                                                            TEST_TONES, TEST_TONE_COUNT, 100, 0)
                         : board_hal_mic_capture(seconds * 1000u, on_block, &ctx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Microphone capture failed: %s", esp_err_to_name(err));
    } else {
        memset(&s_result, 0, sizeof(s_result));
        s_result.have_result = true;
        s_result.result_with_tones = play;
        s_result.baseline_dbfs = ctx.mic.baseline_dbfs;
        s_result.result_threshold_dbfs = mic_detect_threshold_dbfs(&ctx.mic);
        s_result.mic_peak_dbfs = ctx.mic.peak_dbfs;
        s_result.mic_bursts = ctx.mic.bursts;
        s_result.mic2_peak_dbfs = ctx.mic2.peak_dbfs;
        s_result.mic2_bursts = ctx.mic2.bursts;
        // Allow one burst to be missed (a pause edge, room noise).
        s_result.heard = ctx.mic.bursts + 1 >= MIC_MONITOR_TEST_BURSTS &&
                         ctx.mic.peak_dbfs - ctx.mic.baseline_dbfs >= MIC_DETECT_RISE_DB;
        ESP_LOGI(TAG,
                 "Monitor done: loudest peak %.1f dBFS | microphone: floor %.1f dBFS, peak %.1f, "
                 "%u/%d bursts%s | microphone 2: peak %.1f, %u bursts | %s",
                 (double) ctx.loudest_peak_dbfs, (double) ctx.mic.baseline_dbfs,
                 (double) ctx.mic.peak_dbfs, ctx.mic.bursts, MIC_MONITOR_TEST_BURSTS,
                 s_result.heard ? " (HEARD)" : "", (double) ctx.mic2.peak_dbfs, ctx.mic2.bursts,
                 s_result.heard ? "microphone hears the tones" : "no tones detected");
    }
    s_running = false;
    vTaskDelete(NULL);
}

static void tones_task(void *arg)
{
    uint8_t volume = (uint8_t) (uintptr_t) arg;
    ESP_LOGI(TAG, "Playing the self-test tones at %u %%", (unsigned) volume);
    power_manager_reset_sleep_timer();
    esp_err_t err = board_hal_play_notes(TEST_TONES, TEST_TONE_COUNT, volume);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Playing the test tones failed: %s", esp_err_to_name(err));
    }
    s_tones_running = false;
    vTaskDelete(NULL);
}

bool mic_monitor_available(void)
{
    return board_hal_has_microphone();
}

esp_err_t mic_monitor_start(uint32_t seconds, bool play_tones)
{
    if (seconds == 0 || seconds > MIC_MONITOR_MAX_SECONDS) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_running || s_tones_running) {
        return ESP_ERR_INVALID_STATE;
    }
    s_running = true;
    s_stop = false;
    s_play_tones = play_tones;
    s_rms_dbfs = MIC_LEVEL_FLOOR_DBFS;
    s_peak_dbfs = MIC_LEVEL_FLOOR_DBFS;
    s_mic_dbfs = MIC_LEVEL_FLOOR_DBFS;
    s_mic2_dbfs = MIC_LEVEL_FLOOR_DBFS;
    s_floor_dbfs = MIC_LEVEL_FLOOR_DBFS;
    s_result.have_result = false;
    if (xTaskCreate(monitor_task, "mic_monitor", 8192, (void *) (uintptr_t) seconds, 5, NULL) !=
        pdPASS) {
        s_running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t mic_monitor_play_tones(uint8_t volume_percent)
{
    if (!board_hal_has_speaker()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (volume_percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_running || s_tones_running) {
        return ESP_ERR_INVALID_STATE;
    }
    s_tones_running = true;
    if (xTaskCreate(tones_task, "mic_tones", 6144, (void *) (uintptr_t) volume_percent, 5, NULL) !=
        pdPASS) {
        s_tones_running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void mic_monitor_get_status(mic_monitor_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (s_result.have_result) {
        *out = s_result;
    }
    out->running = s_running;
    out->tones_running = s_tones_running;
    out->rms_dbfs = s_rms_dbfs;
    out->peak_dbfs = s_peak_dbfs;

    load_settings();
    out->mic_dbfs = s_mic_dbfs;
    out->mic2_dbfs = s_mic2_dbfs;
    out->floor_dbfs = s_floor_dbfs;
    out->auto_threshold = s_settings.auto_threshold;
    out->threshold_dbfs = s_settings.auto_threshold ? mic_detect_auto_threshold_dbfs(s_floor_dbfs)
                                                    : (float) s_settings.threshold_dbfs;
}

void mic_monitor_stop(void)
{
    s_stop = true;
}

void mic_monitor_get_settings(mic_settings_t *out)
{
    load_settings();
    *out = s_settings;
}

esp_err_t mic_monitor_set_settings(bool auto_threshold, int threshold_dbfs)
{
    if (threshold_dbfs < MIC_THRESHOLD_MIN_DBFS || threshold_dbfs > MIC_THRESHOLD_MAX_DBFS) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(MIC_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, MIC_NVS_AUTO_KEY, auto_threshold ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_set_i8(h, MIC_NVS_THRESHOLD_KEY, (int8_t) threshold_dbfs);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        load_settings();
        s_settings.auto_threshold = auto_threshold;
        s_settings.threshold_dbfs = threshold_dbfs;
    }
    return err;
}

#endif  // BOARD_HAL_VOICE_ENABLED
