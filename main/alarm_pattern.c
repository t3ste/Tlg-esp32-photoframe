#include "alarm_pattern.h"

static const float TUNES_HZ[ALARM_TUNE_COUNT][ALARM_PATTERN_NOTE_COUNT] = {
    {392.0f, 523.0f, 659.0f, 523.0f},  // 0 G4 C5 E5 C5 - classic (default)
    {523.0f, 659.0f, 784.0f, 659.0f},  // 1 C5 E5 G5 E5 - bright and friendly
    {440.0f, 523.0f, 659.0f, 523.0f},  // 2 A4 C5 E5 C5 - soft and pleasant
    {392.0f, 587.0f, 494.0f, 587.0f},  // 3 G4 D5 B4 D5 - clear and attention-grabbing
    {349.0f, 440.0f, 523.0f, 440.0f},  // 4 F4 A4 C5 A4 - warm and calm
    {523.0f, 392.0f, 659.0f, 523.0f},  // 5 C5 G4 E5 C5 - distinctive, a little more dynamic
};

const float *alarm_pattern_tune(int tune)
{
    if (tune < 0 || tune >= ALARM_TUNE_COUNT) {
        tune = ALARM_TUNE_DEFAULT;
    }
    return TUNES_HZ[tune];
}

int alarm_pattern_count(uint32_t total_ms)
{
    int cycles = (int) ((total_ms + ALARM_PATTERN_CYCLE_MS - 1) / ALARM_PATTERN_CYCLE_MS);
    return cycles * (ALARM_PATTERN_NOTE_COUNT + 1);
}

int alarm_pattern_build(alarm_note_t *out, int max, uint32_t total_ms, int tune)
{
    const float *notes = alarm_pattern_tune(tune);
    int n = 0;
    uint32_t elapsed = 0;
    while (elapsed < total_ms) {
        for (int i = 0; i < ALARM_PATTERN_NOTE_COUNT; i++) {
            if (n >= max) {
                return n;
            }
            out[n].freq_hz = notes[i];
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
