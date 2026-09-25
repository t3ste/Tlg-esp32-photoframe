#include "alarm_pattern.h"

static const float NOTES_HZ[ALARM_PATTERN_NOTE_COUNT] = {392.0f, 523.0f, 659.0f, 523.0f};

int alarm_pattern_count(uint32_t total_ms)
{
    int cycles = (int) ((total_ms + ALARM_PATTERN_CYCLE_MS - 1) / ALARM_PATTERN_CYCLE_MS);
    return cycles * (ALARM_PATTERN_NOTE_COUNT + 1);
}

int alarm_pattern_build(alarm_note_t *out, int max, uint32_t total_ms)
{
    int n = 0;
    uint32_t elapsed = 0;
    while (elapsed < total_ms) {
        for (int i = 0; i < ALARM_PATTERN_NOTE_COUNT; i++) {
            if (n >= max) {
                return n;
            }
            out[n].freq_hz = NOTES_HZ[i];
            out[n].duration_ms = ALARM_PATTERN_NOTE_MS;
            n++;
        }
        if (n >= max) {
            return n;
        }
        out[n].freq_hz = 0.0f;
        out[n].duration_ms = ALARM_PATTERN_PAUSE_MS;
        n++;
        elapsed += ALARM_PATTERN_CYCLE_MS;
    }
    return n;
}

bool alarm_pattern_tone_sounding(uint32_t t_ms)
{
    uint32_t phase = t_ms % ALARM_PATTERN_CYCLE_MS;
    return phase <
           (uint32_t) (ALARM_PATTERN_NOTE_COUNT * ALARM_PATTERN_NOTE_MS + ALARM_PATTERN_TAIL_MS);
}
