#ifndef MIC_DETECT_H
#define MIC_DETECT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Windows (~200 ms each) averaged into the noise-floor baseline before detection starts. */
#define MIC_DETECT_BASELINE_WINDOWS 5
/** A window counts as "sound" when it rises this far above the baseline... */
#define MIC_DETECT_RISE_DB 10.0f
/** ...but never below this absolute level (a silent room must not trigger on dither noise). */
#define MIC_DETECT_MIN_THRESHOLD_DBFS (-75.0f)
/** A burst ends once the level falls this far below the threshold (hysteresis). */
#define MIC_DETECT_RELEASE_DB 3.0f

/**
 * Counts sound bursts (a tone sequence with pauses) in a stream of per-window
 * RMS levels: the first MIC_DETECT_BASELINE_WINDOWS windows set the noise floor,
 * afterwards every rise above baseline + MIC_DETECT_RISE_DB is one burst.
 */
typedef struct {
    unsigned windows;
    double baseline_power_sum;
    unsigned baseline_count;
    bool baseline_ready;
    float baseline_dbfs;
    bool in_burst;
    unsigned bursts;
    float peak_dbfs;  // loudest window after the baseline was taken
} mic_detect_t;

void mic_detect_init(mic_detect_t *d);

/** Feed one window's RMS level in dBFS. */
void mic_detect_add_window(mic_detect_t *d, float rms_dbfs);

/** Level a window has to exceed to count as sound (only meaningful once baseline_ready). */
float mic_detect_threshold_dbfs(const mic_detect_t *d);

#ifdef __cplusplus
}
#endif

#endif  // MIC_DETECT_H
