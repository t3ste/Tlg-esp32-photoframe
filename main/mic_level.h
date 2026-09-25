#ifndef MIC_LEVEL_H
#define MIC_LEVEL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Level reported for digital silence (and the floor for anything quieter). */
#define MIC_LEVEL_FLOOR_DBFS (-96.0f)

/** Running sums over any number of interleaved stereo blocks. */
typedef struct {
    int64_t sum[2];
    int64_t sum_sq[2];
    int32_t max[2];
    int32_t min[2];
    uint32_t frames;
} mic_level_acc_t;

typedef struct {
    float rms_dbfs;   // RMS of the signal with its DC offset removed
    float peak_dbfs;  // largest excursion from that DC offset
} mic_level_t;

void mic_level_acc_reset(mic_level_acc_t *acc);

/** Adds @p frames interleaved signed 16-bit stereo frames (L,R,L,R,...). */
void mic_level_acc_add(mic_level_acc_t *acc, const int16_t *stereo, size_t frames);

/**
 * Level of everything added since the last reset, in dBFS (0 = full scale).
 * The codec delivers mono in one slot, so the louder of the two channels is
 * reported. Returns MIC_LEVEL_FLOOR_DBFS for both values when empty.
 */
mic_level_t mic_level_acc_result(const mic_level_acc_t *acc);

/**
 * Level of a single channel (0 = left / MIC1, 1 = right / MIC3, which on this
 * board is the speaker-amp reference signal).
 */
mic_level_t mic_level_acc_channel(const mic_level_acc_t *acc, int channel);

/**
 * Renders a text level bar into @p out (needs width + 3 bytes): "[####------]",
 * mapping @p floor_dbfs..0 dBFS onto @p width cells.
 */
void mic_level_bar(float dbfs, float floor_dbfs, char *out, size_t width);

#ifdef __cplusplus
}
#endif

#endif  // MIC_LEVEL_H
