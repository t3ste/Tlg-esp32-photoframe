#pragma once

#include <hal/gpio_types.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "driver/gpio.h"
#include "epaper.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOARD_HAL_DISPLAY_WIDTH epaper_get_width()
#define BOARD_HAL_DISPLAY_HEIGHT epaper_get_height()

typedef enum {
    BOARD_TYPE_WAVESHARE_PHOTOPAINTER,
    BOARD_TYPE_SEEEDSTUDIO_XIAO_EE02,
    BOARD_TYPE_SEEEDSTUDIO_XIAO_EE03,
    BOARD_TYPE_SEEEDSTUDIO_XIAO_EE04,
    BOARD_TYPE_SEEEDSTUDIO_RETERMINAL_E1002,
    BOARD_TYPE_SEEEDSTUDIO_RETERMINAL_E1003,
    BOARD_TYPE_SEEEDSTUDIO_RETERMINAL_E1004,
    BOARD_TYPE_UNKNOWN
} board_type_t;

#ifdef CONFIG_BOARD_DRIVER_WAVESHARE_PHOTOPAINTER_73
#include "board_waveshare_photopainter_73.h"
#elif defined(CONFIG_BOARD_DRIVER_SEEEDSTUDIO_XIAO_EE02)
#include "board_seeedstudio_xiao_ee02.h"
#elif defined(CONFIG_BOARD_DRIVER_SEEEDSTUDIO_XIAO_EE03)
#include "board_seeedstudio_xiao_ee03.h"
#elif defined(CONFIG_BOARD_DRIVER_SEEEDSTUDIO_XIAO_EE04)
#include "board_seeedstudio_xiao_ee04.h"
#elif defined(CONFIG_BOARD_DRIVER_SEEEDSTUDIO_RETERMINAL_E1002)
#include "board_seeedstudio_reterminal_e1002.h"
#elif defined(CONFIG_BOARD_DRIVER_SEEEDSTUDIO_RETERMINAL_E1003)
#include "board_seeedstudio_reterminal_e1003.h"
#elif defined(CONFIG_BOARD_DRIVER_SEEEDSTUDIO_RETERMINAL_E1004)
#include "board_seeedstudio_reterminal_e1004.h"
#else
// Default definitions if no board selected (fallback)
#error "No board selected! Please define CONFIG_BOARD_DRIVER_..."
#endif

// Display color model reported to the server (selects the dithering palette).
// Boards override this in their header; Spectra-6 color panels use the default.
#ifndef BOARD_HAL_DISPLAY_TYPE
#define BOARD_HAL_DISPLAY_TYPE "spectra6"
#endif

// Boards with an ES8311/PA speaker path define BOARD_HAL_HAS_SPEAKER 1 in their own
// header (see board_waveshare_photopainter_73.h) before including this file.
#ifndef BOARD_HAL_HAS_SPEAKER
#define BOARD_HAL_HAS_SPEAKER 0
#endif

/**
 * @brief Initialize the Board HAL
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t board_hal_init(void);

/**
 * @brief Prepare the system for deep sleep
 *
 * This function should be called just before esp_deep_sleep_start().
 * It handles PMIC-specific shutdown sequences (e.g. disabling rails).
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t board_hal_prepare_for_sleep(void);

/**
 * @brief Is battery connected
 *
 * @return true if connected, false otherwise
 */
bool board_hal_is_battery_connected(void);

/**
 * @brief Get battery percentage
 *
 * @return int Battery percentage (0-100), or -1 if unknown
 */
int board_hal_get_battery_percent(void);

/**
 * @brief Get battery voltage in millivolts
 *
 * @return int Battery voltage in mV, or -1 if unknown
 */
int board_hal_get_battery_voltage(void);

/**
 * @brief Check if battery is currently charging
 *
 * @return true if charging, false otherwise
 */
bool board_hal_is_charging(void);

/**
 * @brief Check if USB power is connected
 *
 * @return true if USB connected, false otherwise
 */
bool board_hal_is_usb_connected(void);

/**
 * @brief Perform a hard shutdown (power off)
 *
 * Note: Behavior depends on hardware. Some PMICs can cut power completely.
 */
void board_hal_shutdown(void);

/**
 * @brief Get ambient temperature (if sensor available)
 *
 * @param[out] t Pointer to float to store temperature in Celsius
 * @return esp_err_t ESP_OK on success, ESP_ERR_NOT_SUPPORTED if no sensor
 */
esp_err_t board_hal_get_temperature(float *t);

/**
 * @brief Get ambient humidity (if sensor available)
 *
 * @param[out] h Pointer to float to store humidity in %RH
 * @return esp_err_t ESP_OK on success, ESP_ERR_NOT_SUPPORTED if no sensor
 */
esp_err_t board_hal_get_humidity(float *h);

/**
 * @brief Initialize the external RTC (if available)
 *
 * @return esp_err_t ESP_OK on success, ESP_ERR_NOT_SUPPORTED if no RTC, or other error
 */
esp_err_t board_hal_rtc_init(void);

/**
 * @brief Get time from external RTC
 *
 * @param[out] t Time value to populate
 * @return esp_err_t ESP_OK on success
 */
esp_err_t board_hal_rtc_get_time(time_t *t);

/**
 * @brief Set time to external RTC
 *
 * @param t Time value to set
 * @return esp_err_t ESP_OK on success
 */
esp_err_t board_hal_rtc_set_time(time_t t);

/**
 * @brief Check if external RTC is available/initialized
 *
 * @return true if available
 */
bool board_hal_rtc_is_available(void);

typedef enum {
    BOARD_HAL_LED_POWER,     // Power/status indicator (red on waveshare, no-op if not present)
    BOARD_HAL_LED_ACTIVITY,  // Activity indicator (green on waveshare, single LED on reterminal)
} board_hal_led_t;

/**
 * @brief Set an onboard LED state
 *
 * @param led Which LED to control
 * @param on true to turn LED on, false to turn off
 */
void board_hal_led_set(board_hal_led_t led, bool on);

/**
 * @brief Whether this board has an on-device speaker / DAC path
 *
 * PhotoPainter 7.3" exposes ES8311 + NS4150B PA. Other boards return false.
 */
bool board_hal_has_speaker(void);

// Short, synthesized (no WAV/melody file) beep patterns for the Chimes feature -
// one built-in tone per severity, not a per-event sound library.
typedef enum {
    BOARD_HAL_CHIME_SUCCESS,  // 1 short beep
    BOARD_HAL_CHIME_WARNING,  // 2 short beeps
    BOARD_HAL_CHIME_ERROR,    // 3 short beeps
} board_hal_chime_kind_t;

/**
 * @brief Play a short synthesized beep pattern on the onboard speaker
 *
 * Waveshare PhotoPainter: ES8311 DAC over I2S with PA GPIO enable, tones
 * generated on-device (no WAV/melody data). Other boards return
 * ESP_ERR_NOT_SUPPORTED.
 *
 * @param kind Which of the 3 built-in patterns to play
 * @param volume_percent 0-100, linearly mapped to the codec's DAC volume
 *        register - applies equally to every kind (urgency is conveyed by
 *        which pattern/how often it repeats, not by loudness)
 * @return ESP_OK on success, ESP_ERR_NOT_SUPPORTED if no speaker, or another
 *         error if the codec / I2S path failed
 */
esp_err_t board_hal_play_beep_pattern(board_hal_chime_kind_t kind, uint8_t volume_percent);

/**
 * @brief Play the repeating bedside-alarm tone until stopped or time runs out
 *
 * Fixed note sequence G4-C5-E5-C5 (392/523/659/523 Hz), 300ms each, followed
 * by a 5s silent pause, repeating - see docs/ALARMCLOCK_FEASIBILITY.md. Unlike
 * board_hal_play_beep_pattern() this can run for minutes, so it needs a way
 * to stop early: @p should_stop is polled once per note and several times
 * during each silent pause (not just once every 5s), so a stop request is
 * noticed within roughly one polling interval, not a hard real-time
 * deadline. Blocking; shares the same serializing mutex as
 * board_hal_play_beep_pattern() (the two can't run at once).
 *
 * @param volume_percent 0-100, same DAC volume mapping as board_hal_play_beep_pattern()
 * @param total_duration_ms Give up and stop after this long even if should_stop() never fires
 * @param should_stop Polled periodically during playback; returning true stops the alarm early.
 *        May be NULL to only ever stop via total_duration_ms.
 * @return ESP_OK on success (whether it ended via timeout or should_stop()), ESP_ERR_NOT_SUPPORTED
 *         if no speaker, or another error if the codec / I2S path failed
 */
esp_err_t board_hal_play_alarm(uint8_t volume_percent, uint32_t total_duration_ms,
                               bool (*should_stop)(void));

// One note in a board_hal_play_notes() sequence: `freq_hz` 0 plays
// `duration_ms` of silence instead of a tone (used for the gap between
// beeps in a counted sequence).
typedef struct {
    float freq_hz;
    int duration_ms;
} board_hal_note_t;

/**
 * @brief Play an arbitrary sequence of tones/silences within one audio session
 *
 * General-purpose primitive behind the alarm-setting button UI's feedback
 * sounds (hour/minute counted beeps at different pitches, the midnight/
 * on-the-hour long tones, the armed/disarmed confirmation sequences - see
 * docs/ALARMCLOCK_FEASIBILITY.md) - unlike calling board_hal_play_beep_pattern()
 * once per note, the whole sequence plays inside a single opened session, so
 * the ~250ms PA settle delay (see audio_chime.c's own comment on this) is
 * paid once for the whole sequence, not once per note - several short beeps
 * "hintereinander folgend" would otherwise have an audible gap before each one.
 *
 * @param notes Sequence to play in order
 * @param count Number of entries in `notes`
 * @param volume_percent 0-100, same DAC volume mapping as board_hal_play_beep_pattern()
 * @return ESP_OK on success, ESP_ERR_NOT_SUPPORTED if no speaker, or another
 *         error if the codec / I2S path failed
 */
esp_err_t board_hal_play_notes(const board_hal_note_t *notes, int count, uint8_t volume_percent);

#ifdef __cplusplus
}
#endif
