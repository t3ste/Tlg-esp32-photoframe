#include "mic_monitor.h"

#include "board_hal.h"
#include "mic_level.h"

#if !BOARD_HAL_HAS_MICROPHONE

// Compiled out to stubs on boards without a microphone.
bool mic_monitor_available(void)
{
    return false;
}

esp_err_t mic_monitor_start(uint32_t seconds)
{
    (void) seconds;
    return ESP_ERR_NOT_SUPPORTED;
}

void mic_monitor_get_status(mic_monitor_status_t *out)
{
    out->running = false;
    out->rms_dbfs = MIC_LEVEL_FLOOR_DBFS;
    out->peak_dbfs = MIC_LEVEL_FLOOR_DBFS;
}

#else

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "power_manager.h"

static const char *TAG = "mic";

#define WINDOW_FRAMES 3200  // 200 ms at 16 kHz -> five lines per second
#define BAR_WIDTH 30
#define BAR_FLOOR_DBFS (-60.0f)

static volatile bool s_running = false;
static volatile float s_rms_dbfs = MIC_LEVEL_FLOOR_DBFS;
static volatile float s_peak_dbfs = MIC_LEVEL_FLOOR_DBFS;

typedef struct {
    mic_level_acc_t acc;
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
    mic_level_acc_reset(&ctx->acc);
    s_rms_dbfs = level.rms_dbfs;
    s_peak_dbfs = level.peak_dbfs;
    if (level.peak_dbfs > ctx->loudest_peak_dbfs) {
        ctx->loudest_peak_dbfs = level.peak_dbfs;
    }

    char bar[BAR_WIDTH + 3];
    mic_level_bar(level.rms_dbfs, BAR_FLOOR_DBFS, bar, BAR_WIDTH);
    ESP_LOGI(TAG, "%s %6.1f dBFS  (peak %6.1f)", bar, (double) level.rms_dbfs,
             (double) level.peak_dbfs);

    power_manager_reset_sleep_timer();  // don't auto-sleep mid-test
    return true;
}

static void monitor_task(void *arg)
{
    uint32_t seconds = (uint32_t) (uintptr_t) arg;
    ESP_LOGI(TAG, "Microphone level monitor for %u s - make some noise (bar: %d..0 dBFS)",
             (unsigned) seconds, (int) BAR_FLOOR_DBFS);

    monitor_ctx_t ctx;
    mic_level_acc_reset(&ctx.acc);
    ctx.loudest_peak_dbfs = MIC_LEVEL_FLOOR_DBFS;

    esp_err_t err = board_hal_mic_capture(seconds * 1000u, on_block, &ctx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Microphone capture failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Microphone level monitor done (loudest peak %.1f dBFS)",
                 (double) ctx.loudest_peak_dbfs);
    }
    s_running = false;
    vTaskDelete(NULL);
}

bool mic_monitor_available(void)
{
    return board_hal_has_microphone();
}

esp_err_t mic_monitor_start(uint32_t seconds)
{
    if (seconds == 0 || seconds > MIC_MONITOR_MAX_SECONDS) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_running) {
        return ESP_ERR_INVALID_STATE;
    }
    s_running = true;
    s_rms_dbfs = MIC_LEVEL_FLOOR_DBFS;
    s_peak_dbfs = MIC_LEVEL_FLOOR_DBFS;
    if (xTaskCreate(monitor_task, "mic_monitor", 4096, (void *) (uintptr_t) seconds, 5, NULL) !=
        pdPASS) {
        s_running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void mic_monitor_get_status(mic_monitor_status_t *out)
{
    out->running = s_running;
    out->rms_dbfs = s_rms_dbfs;
    out->peak_dbfs = s_peak_dbfs;
}

#endif  // BOARD_HAL_HAS_MICROPHONE
