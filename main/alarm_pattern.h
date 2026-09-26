#ifndef ALARM_PATTERN_H
#define ALARM_PATTERN_H

/*
 * The ring pattern of the bedside alarm: four notes (G4 C5 E5 C5) followed by a
 * silent pause, repeating. Mirrors board_hal_play_alarm() in the board HAL
 * (which plays the same pattern with its own constants - keep them in sync; a
 * host test pins the cycle length). Used by the voice stop: while notes sound
 * the microphone is deaf to speech, so the stop word is only listened for in
 * the pauses.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ALARM_PATTERN_NOTE_COUNT 4
#define ALARM_PATTERN_NOTE_MS 300
#define ALARM_PATTERN_PAUSE_MS 5000
#define ALARM_PATTERN_CYCLE_MS \
    (ALARM_PATTERN_NOTE_COUNT * ALARM_PATTERN_NOTE_MS + ALARM_PATTERN_PAUSE_MS)

/** How long after a note ends the room still rings (echo/DMA delay) and audio is ignored. */
#define ALARM_PATTERN_TAIL_MS 250

// Selectable alarm melodies (four notes each, repeated). Numbering is stored in NVS
// (config: alarm_tune) and must stay stable; 0 is the default.
#define ALARM_TUNE_COUNT 6
#define ALARM_TUNE_DEFAULT 0

typedef struct {
    float freq_hz;  // 0 = silence
    int duration_ms;
} alarm_note_t;

/** The four note frequencies (Hz) of a melody; an unknown number gives the default melody. */
const float *alarm_pattern_tune(int tune);

/**
 * Writes the notes of melody @p tune (and pauses as freq 0) needed to ring for @p total_ms
 * into @p out, at most @p max entries. Returns the number written.
 */
int alarm_pattern_build(alarm_note_t *out, int max, uint32_t total_ms, int tune);

/** Number of entries alarm_pattern_build() needs for @p total_ms. */
int alarm_pattern_count(uint32_t total_ms);

/** True while a note sounds (or its echo rings) at @p t_ms after the ring started. */
bool alarm_pattern_tone_sounding(uint32_t t_ms);

#ifdef __cplusplus
}
#endif

#endif  // ALARM_PATTERN_H
