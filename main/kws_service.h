#ifndef KWS_SERVICE_H
#define KWS_SERVICE_H

/*
 * Device side of the stop-word ("Wortmuster") recognition, see kws.h: enrolling
 * the word, keeping the templates, and testing detection on the microphone.
 * First step towards switching a ringing alarm off by voice - it is not wired
 * into the alarm yet. Only boards with a microphone; stubs elsewhere.
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

#ifdef __cplusplus
}
#endif

#endif  // KWS_SERVICE_H
