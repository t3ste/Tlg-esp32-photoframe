#ifndef KWS_SERVICE_H
#define KWS_SERVICE_H

/*
 * Device side of the stop-word ("Wortmuster") recognition, see kws.h: enrolling
 * the word, keeping the templates, and testing detection on the microphone.
 * Belongs to the Alarm Clock: switching a ringing alarm off by voice. Compiled
 * in only in an Alarm Clock firmware on a board with speaker and microphone
 * (BOARD_HAL_VOICE_ENABLED); stubs elsewhere.
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "kws.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KWS_SERVICE_ENROLL_MAX_SECONDS 4
#define KWS_SERVICE_TEST_MAX_SECONDS 60

typedef enum {
    KWS_SERVICE_IDLE = 0,
    KWS_SERVICE_ENROLLING,
    KWS_SERVICE_TESTING,
} kws_service_mode_t;

typedef struct {
    bool available;  // the board has a microphone
    kws_service_mode_t mode;
    int templates;  // enrolled patterns
    float threshold;
    bool alarm_stop;  // the ringing alarm listens for the word

    // Last enrolment
    bool have_enroll_result;
    int enroll_status;  // kws_status_t: 0 = added, -1 = no speech heard, -2 = too long
    int enroll_frames;

    // Last (or running) test
    unsigned test_utterances;  // utterances heard and scored
    unsigned test_detections;  // of which accepted as the keyword
    float best_score;          // lowest distance seen (a match is below the threshold)
    float last_score;
} kws_service_status_t;

/**
 * Records up to @p seconds, finds the spoken word in it and adds it as a
 * template (up to 5; calibrates the threshold and stores the templates on the
 * storage). Speak the word once, shortly after the call. Returns immediately.
 *
 * ESP_ERR_NOT_SUPPORTED (no microphone), ESP_ERR_INVALID_STATE (busy or five
 * templates already), ESP_ERR_INVALID_ARG (bad duration).
 */
esp_err_t kws_service_enroll(uint32_t seconds);

/** Forgets all templates (also on the storage). */
esp_err_t kws_service_clear(void);

/**
 * Listens for @p seconds and counts how often the enrolled word is heard
 * (and the best score of everything else). Returns immediately.
 * ESP_ERR_INVALID_STATE without templates or while busy.
 */
esp_err_t kws_service_test(uint32_t seconds);

void kws_service_get_status(kws_service_status_t *out);

/** Switch: should a ringing alarm listen for the stop word? (persisted) */
esp_err_t kws_service_set_alarm_stop(bool enabled);

/** True when the alarm should listen: the switch is on and a word is enrolled. */
bool kws_service_alarm_stop_ready(void);

/**
 * Word listener for the ringing alarm (see alarm_manager.c). Holds the audio
 * window (about 70 KB, PSRAM if available) and works on the audio blocks the
 * microphone capture hands over.
 */
typedef struct kws_listener kws_listener_t;

/** NULL unless kws_service_alarm_stop_ready() (or out of memory). */
kws_listener_t *kws_service_listener_open(void);

/**
 * Feeds one block of interleaved stereo audio. @p mute replaces it by silence
 * (the alarm notes are sounding). Returns true when the stop word was heard.
 */
bool kws_service_listener_feed(kws_listener_t *l, const int16_t *stereo, size_t frames, bool mute);

void kws_service_listener_close(kws_listener_t *l);

#ifdef __cplusplus
}
#endif

#endif  // KWS_SERVICE_H
