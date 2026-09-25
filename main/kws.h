#ifndef KWS_H
#define KWS_H

/*
 * Keyword ("Wortmuster") spotting for switching off a ringing alarm by voice.
 *
 * Deliberately small and self-contained (no ML runtime, no ESP-IDF calls) so it
 * can be unit-tested on the host: the user enrols the stop word by speaking it
 * a few times; every enrolment becomes a template of MFCC feature frames, and
 * later utterances are compared with the templates by dynamic time warping
 * (DTW), which absorbs different speaking speeds. Nothing leaves the frame and
 * no audio is stored - only the feature templates.
 *
 * Pipeline: 16 kHz mono PCM -> voice-activity segmentation (loudest speech
 * region) -> 25 ms / 10 ms log-mel MFCC (12 coefficients, liftered, cepstral
 * mean normalised so overall level and microphone colouring drop out) -> DTW
 * distance to each template (Sakoe-Chiba band, length-normalised) -> best
 * distance; below the matcher's threshold counts as the keyword.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KWS_SAMPLE_RATE 16000
#define KWS_FRAME_LEN 400  // 25 ms
#define KWS_HOP 160        // 10 ms
#define KWS_FFT_SIZE 512
#define KWS_MEL_BANDS 24
#define KWS_NUM_CEPS 12

// Brings the DTW distances into a handy range: the same word from the same
// speaker lands at about 2..5, different words above about 6.5 (host test set).
#define KWS_FEATURE_SCALE 0.1f
#define KWS_MAX_FRAMES 150  // 1.5 s of speech per pattern
#define KWS_MIN_FRAMES 20   // 200 ms
#define KWS_MAX_TEMPLATES 5

typedef enum {
    KWS_OK = 0,
    KWS_ERR_NO_SPEECH = -1,  // nothing loud enough / too short
    KWS_ERR_TOO_LONG = -2,   // the speech region is longer than KWS_MAX_FRAMES
    KWS_ERR_ARG = -3,
} kws_status_t;

/** MFCC frames of one utterance. */
typedef struct {
    uint16_t frames;
    float data[KWS_MAX_FRAMES][KWS_NUM_CEPS];
} kws_pattern_t;

/**
 * Finds the speech in @p pcm (the loudest contiguous region, short pauses
 * inside a word tolerated) and extracts its MFCC pattern.
 *
 * @param start_sample,end_sample optional: sample range of the region found
 */
kws_status_t kws_extract(const int16_t *pcm, size_t n, kws_pattern_t *out, size_t *start_sample,
                         size_t *end_sample);

/**
 * Length-normalised DTW distance between two patterns; lower means more
 * alike. Returns a large value (>= KWS_DTW_INFINITE) when the durations differ
 * by more than a factor of two.
 */
#define KWS_DTW_INFINITE 1.0e9f
float kws_dtw_distance(const kws_pattern_t *a, const kws_pattern_t *b);

/** Enrolled templates plus the acceptance threshold. Large (about 36 KB): allocate it on the heap.
 */
typedef struct {
    int count;
    float threshold;  // distance below this is the keyword
    kws_pattern_t templates[KWS_MAX_TEMPLATES];
} kws_matcher_t;

void kws_matcher_init(kws_matcher_t *m);

/** Adds an enrolment; returns its index or -1 when the matcher is full. */
int kws_matcher_add(kws_matcher_t *m, const kws_pattern_t *pattern);

/** Best (lowest) distance of @p utterance to any template; KWS_DTW_INFINITE without templates. */
float kws_matcher_score(const kws_matcher_t *m, const kws_pattern_t *utterance);

/**
 * Sets the threshold from the enrolments: @p margin times the largest
 * nearest-neighbour distance among them (how far a repetition of the word lands
 * from its closest earlier example), but never below @p floor_threshold. With
 * fewer than two templates the threshold becomes @p floor_threshold.
 */
void kws_matcher_calibrate(kws_matcher_t *m, float margin, float floor_threshold);

/** Recommended calibration values (tuned on the host test set, see host_tests/test_kws.cpp). */
#define KWS_DEFAULT_MARGIN 1.2f
#define KWS_DEFAULT_FLOOR_THRESHOLD 4.0f

/**
 * Sliding detector for a live audio stream (e.g. the pauses of the alarm tone):
 * keeps the last few seconds, and whenever a new utterance has just ended
 * scores it against the matcher.
 */
typedef struct {
    int16_t *ring;           // caller-provided sample buffer
    size_t capacity;         // samples in @c ring
    kws_pattern_t *scratch;  // caller-provided work buffer (one pattern, 7 KB)
    size_t fill;             // valid samples (the newest are at the end)
    uint64_t total;          // samples pushed so far
    size_t since_check;      // samples since the last evaluation
    uint64_t last_end;       // absolute end sample of the last evaluated utterance
    float last_score;        // score of the last evaluated utterance
    unsigned utterances;     // utterances evaluated so far
} kws_stream_t;

/** @p ring should hold at least 2 s of audio (32000 samples). */
void kws_stream_init(kws_stream_t *s, int16_t *ring, size_t capacity, kws_pattern_t *scratch);

/**
 * Feeds audio. Returns true (and the score in @p score_out) when an utterance
 * that just ended matches the keyword. Non-matching utterances only update
 * last_score / utterances.
 */
bool kws_stream_push(kws_stream_t *s, const kws_matcher_t *m, const int16_t *pcm, size_t n,
                     float *score_out);

#ifdef __cplusplus
}
#endif

#endif  // KWS_H
