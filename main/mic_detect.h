#ifndef MIC_DETECT_H
#define MIC_DETECT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Windows (~200 ms each) averaged into the noise-floor baseline before detection starts. */
#define MIC_DETECT_BASELINE_WINDOWS 5
/**
 * A window counts as "sound" when it rises this far above the baseline...
 * (measured on the frames: room noise wobbles by up to ~15 dB window to window,
 * the self-test tones arrive 30 dB and more above it)
 */
#define MIC_DETECT_RISE_DB 20.0f
/** ...but never below this absolute level (ambient windows stay under about -50 dBFS). */
#define MIC_DETECT_MIN_THRESHOLD_DBFS (-45.0f)
/** A burst ends once the level falls this far below the threshold (hysteresis). */
#define MIC_DETECT_RELEASE_DB 3.0f

/**
 * Counts sound bursts (a tone sequence with pauses) in a stream of per-window
 * RMS levels: the first MIC_DETECT_BASELINE_WINDOWS windows set the noise floor,
 * afterwards every rise above baseline + MIC_DETECT_RISE_DB is one burst.
 */
typedef struct {
    bool manual;  // fixed threshold instead of "baseline + MIC_DETECT_RISE_DB"
    float manual_dbfs;
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

/**
 * Use a fixed threshold (dBFS) instead of the baseline-relative one. Call right
 * after mic_detect_init(). Detection then starts with the first window; the
 * baseline is still measured (for reporting).
 */
void mic_detect_set_manual(mic_detect_t *d, float threshold_dbfs);

/** Feed one window's RMS level in dBFS. */
void mic_detect_add_window(mic_detect_t *d, float rms_dbfs);

/** Level a window has to exceed to count as sound (auto: only meaningful once baseline_ready). */
float mic_detect_threshold_dbfs(const mic_detect_t *d);

/** The auto threshold for a given noise floor: floor + MIC_DETECT_RISE_DB, at least the minimum. */
float mic_detect_auto_threshold_dbfs(float floor_dbfs);

/**
 * Live noise-floor estimate ("Grundpegel") for the Web UI's level meter: follows
 * steady room noise slowly and ignores short loud events, so it is what the
 * automatic threshold is derived from while the meter runs.
 */
#define MIC_FLOOR_ALPHA 0.1f          // per ~200 ms window (about a 2 s time constant)
#define MIC_FLOOR_EVENT_DB 10.0f      // a window this far above the floor is an event...
#define MIC_FLOOR_RELOCK_WINDOWS 50u  // ...unless it lasts this long (then the room got louder)

typedef struct {
    bool ready;
    float floor_dbfs;
    unsigned event_windows;
} mic_floor_t;

void mic_floor_init(mic_floor_t *f);
void mic_floor_update(mic_floor_t *f, float rms_dbfs);

#ifdef __cplusplus
}
#endif

#endif  // MIC_DETECT_H
