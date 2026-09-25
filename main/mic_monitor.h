#ifndef MIC_MONITOR_H
#define MIC_MONITOR_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MIC_MONITOR_MAX_SECONDS 60

typedef struct {
    bool running;
    float rms_dbfs;   // last ~200 ms window
    float peak_dbfs;  // same window
} mic_monitor_status_t;

/** True if this board has an onboard microphone. */
bool mic_monitor_available(void);

/**
 * @brief Listen on the microphone for @p seconds and print the level to the
 * console (and the debug log) about five times a second, as a text bar plus
 * RMS/peak in dBFS. Runs in its own task and returns immediately; also keeps
 * the device from auto-sleeping meanwhile. First step towards voice control.
 *
 * @return ESP_OK, ESP_ERR_NOT_SUPPORTED (no microphone), ESP_ERR_INVALID_STATE
 *         (a monitor is already running), ESP_ERR_INVALID_ARG (bad duration)
 */
esp_err_t mic_monitor_start(uint32_t seconds);

void mic_monitor_get_status(mic_monitor_status_t *out);

#ifdef __cplusplus
}
#endif

#endif  // MIC_MONITOR_H
