#include "mic_monitor.h"

#include <string.h>

#include "board_hal.h"
#include "mic_level.h"

#if !BOARD_HAL_HAS_MICROPHONE

// Compiled out to stubs on boards without a microphone.
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
}

#else

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mic_detect.h"
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
static mic_monitor_status_t s_result;  // written once per finished run

typedef struct {
    mic_level_acc_t acc;
    mic_detect_t mic;  // left channel (microphone)
    mic_detect_t ref;  // right channel (speaker reference)
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
    mic_detect_add_window(&ctx->ref, right.rms_dbfs);

    s_rms_dbfs = level.rms_dbfs;
    s_peak_dbfs = level.peak_dbfs;
    if (level.peak_dbfs > ctx->loudest_peak_dbfs) {
        ctx->loudest_peak_dbfs = level.peak_dbfs;
    }

    char bar[BAR_WIDTH + 3];
    mic_level_bar(level.rms_dbfs, BAR_FLOOR_DBFS, bar, BAR_WIDTH);
    ESP_LOGI(TAG, "%s %6.1f dBFS  (peak %6.1f)  mic %6.1f  ref %6.1f", bar, (double) level.rms_dbfs,
             (double) level.peak_dbfs, (double) left.rms_dbfs, (double) right.rms_dbfs);

    power_manager_reset_sleep_timer();  // don't auto-sleep mid-test
    return true;
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
    mic_detect_init(&ctx.mic);
    mic_detect_init(&ctx.ref);
    ctx.loudest_peak_dbfs = MIC_LEVEL_FLOOR_DBFS;

    esp_err_t err = play ? board_hal_mic_capture_with_tones(seconds * 1000u, on_block, &ctx,
                                                            TEST_TONES, TEST_TONE_COUNT, 100)
                         : board_hal_mic_capture(seconds * 1000u, on_block, &ctx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Microphone capture failed: %s", esp_err_to_name(err));
    } else {
        memset(&s_result, 0, sizeof(s_result));
        s_result.have_result = true;
        s_result.result_with_tones = play;
        s_result.baseline_dbfs = ctx.mic.baseline_dbfs;
        s_result.mic_peak_dbfs = ctx.mic.peak_dbfs;
        s_result.mic_bursts = ctx.mic.bursts;
        s_result.ref_peak_dbfs = ctx.ref.peak_dbfs;
        s_result.ref_bursts = ctx.ref.bursts;
        // Allow one burst to be missed (a pause edge, room noise).
        s_result.heard = ctx.mic.bursts + 1 >= MIC_MONITOR_TEST_BURSTS &&
                         ctx.mic.peak_dbfs - ctx.mic.baseline_dbfs >= MIC_DETECT_RISE_DB;
        ESP_LOGI(TAG,
                 "Monitor done: loudest peak %.1f dBFS | microphone: floor %.1f dBFS, peak %.1f, "
                 "%u/%d bursts%s | reference: peak %.1f, %u bursts | %s",
                 (double) ctx.loudest_peak_dbfs, (double) ctx.mic.baseline_dbfs,
                 (double) ctx.mic.peak_dbfs, ctx.mic.bursts, MIC_MONITOR_TEST_BURSTS,
                 s_result.heard ? " (HEARD)" : "", (double) ctx.ref.peak_dbfs, ctx.ref.bursts,
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
    s_play_tones = play_tones;
    s_rms_dbfs = MIC_LEVEL_FLOOR_DBFS;
    s_peak_dbfs = MIC_LEVEL_FLOOR_DBFS;
    s_result.have_result = false;
    if (xTaskCreate(monitor_task, "mic_monitor", 4096, (void *) (uintptr_t) seconds, 5, NULL) !=
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
    if (xTaskCreate(tones_task, "mic_tones", 4096, (void *) (uintptr_t) volume_percent, 5, NULL) !=
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
}

#endif  // BOARD_HAL_HAS_MICROPHONE
