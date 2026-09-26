#ifndef ALARM_RAMP_H
#define ALARM_RAMP_H

/*
 * Volume ramp-up of the ringing alarm. Pure arithmetic (no dependencies) so the board HAL and
 * the host tests share it: the alarm starts at ALARM_RAMP_START_GAIN of the set volume and
 * rises linearly to the full set volume after ramp_ms. ramp_ms == 0 = no ramp (full volume at
 * once). The start level (about -22 dB) is quiet but still audible, so the ramp is not
 * "silence for a while, then sound".
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ALARM_RAMP_START_GAIN 0.08f

static inline float alarm_ramp_gain(uint32_t elapsed_ms, uint32_t ramp_ms)
{
    if (ramp_ms == 0 || elapsed_ms >= ramp_ms) {
        return 1.0f;
    }
    return ALARM_RAMP_START_GAIN +
           (1.0f - ALARM_RAMP_START_GAIN) * ((float) elapsed_ms / (float) ramp_ms);
}

#ifdef __cplusplus
}
#endif

#endif  // ALARM_RAMP_H
