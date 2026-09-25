#ifndef MIC_MONITOR_H
#define MIC_MONITOR_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MIC_MONITOR_MAX_SECONDS 60

/** Tone bursts in the self-test sequence (see mic_monitor.c). */
#define MIC_MONITOR_TEST_BURSTS 4

/** Sensitivity of the tone/sound detection, persisted on the device. */
#define MIC_THRESHOLD_MIN_DBFS (-90)
#define MIC_THRESHOLD_MAX_DBFS 0
#define MIC_THRESHOLD_DEFAULT_DBFS (-45)

typedef struct {
    bool auto_threshold;  // threshold = noise floor + 20 dB (at least -45 dBFS)
    int threshold_dbfs;   // fixed threshold used when auto_threshold is off
} mic_settings_t;

typedef struct {
    bool running;        // a level monitor is running
    bool tones_running;  // this device is playing the self-test tones (speaker only)
    float rms_dbfs;      // last ~200 ms window, louder channel
    float peak_dbfs;

    // Live values for the Web UI's level meter (last ~200 ms window).
    float mic_dbfs;        // microphone 1 (left)
    float mic2_dbfs;       // microphone 2 (right)
    float floor_dbfs;      // running noise-floor estimate ("Grundpegel")
    float threshold_dbfs;  // the threshold currently in effect (auto: derived from the floor)
    bool auto_threshold;

    // Result of the last finished run (left channel = microphone 1, right channel =
    // microphone 2).
    bool have_result;
    bool result_with_tones;       // the run played the tones on this device's own speaker
    float baseline_dbfs;          // microphone noise floor before the tones
    float result_threshold_dbfs;  // threshold that run used
    float mic_peak_dbfs;
    unsigned mic_bursts;
    float mic2_peak_dbfs;
    unsigned mic2_bursts;
    bool heard;  // the microphone picked up (nearly) all tone bursts
} mic_monitor_status_t;

/** True if this board has an onboard microphone. */
bool mic_monitor_available(void);

/**
 * @brief Listen on the microphone for @p seconds and print the level to the
 * console (and the debug log) about five times a second, as a text bar plus
 * RMS/peak in dBFS. Runs in its own task and returns immediately; also keeps
 * the device from auto-sleeping meanwhile. Sound bursts are counted as well
 * (see mic_detect.h), so the run doubles as the listener of the speaker/mic
 * self-test.
 *
 * @param play_tones also play the self-test tone sequence on this device's own
 *        speaker (100 % volume) while listening
 * @return ESP_OK, ESP_ERR_NOT_SUPPORTED (no microphone), ESP_ERR_INVALID_STATE
 *         (a monitor or the tones are already running), ESP_ERR_INVALID_ARG (bad duration)
 */
esp_err_t mic_monitor_start(uint32_t seconds, bool play_tones);

/**
 * @brief Play the self-test tone sequence (about 5 s: 1.2 s silence, then four
 * 400 ms beeps with 600 ms pauses) on the speaker only, e.g. for another
 * device's microphone to listen to. Returns immediately.
 */
esp_err_t mic_monitor_play_tones(uint8_t volume_percent);

void mic_monitor_get_status(mic_monitor_status_t *out);

/** Ends a running monitor early (no-op when none runs). */
void mic_monitor_stop(void);

/** Loads the persisted sensitivity settings (defaults: automatic). */
void mic_monitor_get_settings(mic_settings_t *out);

/**
 * Persists the sensitivity settings; they apply to the next monitor run (and
 * immediately to the live threshold shown by the meter).
 * ESP_ERR_INVALID_ARG if threshold_dbfs is outside MIC_THRESHOLD_MIN/MAX_DBFS.
 */
esp_err_t mic_monitor_set_settings(bool auto_threshold, int threshold_dbfs);

#ifdef __cplusplus
}
#endif

#endif  // MIC_MONITOR_H
