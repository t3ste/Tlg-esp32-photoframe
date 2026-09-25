#include "mic_detect.h"

#include <math.h>

#include "mic_level.h"

void mic_detect_init(mic_detect_t *d)
{
    d->manual = false;
    d->manual_dbfs = MIC_DETECT_MIN_THRESHOLD_DBFS;
    d->windows = 0;
    d->baseline_power_sum = 0.0;
    d->baseline_count = 0;
    d->baseline_ready = false;
    d->baseline_dbfs = MIC_LEVEL_FLOOR_DBFS;
    d->in_burst = false;
    d->bursts = 0;
    d->peak_dbfs = MIC_LEVEL_FLOOR_DBFS;
}

void mic_detect_set_manual(mic_detect_t *d, float threshold_dbfs)
{
    d->manual = true;
    d->manual_dbfs = threshold_dbfs;
}

float mic_detect_auto_threshold_dbfs(float floor_dbfs)
{
    float thr = floor_dbfs + MIC_DETECT_RISE_DB;
    return thr < MIC_DETECT_MIN_THRESHOLD_DBFS ? MIC_DETECT_MIN_THRESHOLD_DBFS : thr;
}

float mic_detect_threshold_dbfs(const mic_detect_t *d)
{
    return d->manual ? d->manual_dbfs : mic_detect_auto_threshold_dbfs(d->baseline_dbfs);
}

void mic_floor_init(mic_floor_t *f)
{
    f->ready = false;
    f->floor_dbfs = MIC_LEVEL_FLOOR_DBFS;
    f->event_windows = 0;
}

void mic_floor_update(mic_floor_t *f, float rms_dbfs)
{
    if (!f->ready) {
        f->floor_dbfs = rms_dbfs;
        f->ready = true;
        return;
    }
    if (rms_dbfs > f->floor_dbfs + MIC_FLOOR_EVENT_DB) {
        // Loud event: don't let it raise the floor - unless it just stays that loud.
        if (++f->event_windows >= MIC_FLOOR_RELOCK_WINDOWS) {
            f->floor_dbfs = rms_dbfs;
            f->event_windows = 0;
        }
        return;
    }
    f->event_windows = 0;
    f->floor_dbfs += (rms_dbfs - f->floor_dbfs) * MIC_FLOOR_ALPHA;
}

void mic_detect_add_window(mic_detect_t *d, float rms_dbfs)
{
    d->windows++;

    if (!d->baseline_ready) {
        // Average in the power domain: one loud click must not dominate the floor
        // any more than it would in the real signal.
        d->baseline_power_sum += pow(10.0, (double) rms_dbfs / 10.0);
        if (++d->baseline_count >= MIC_DETECT_BASELINE_WINDOWS) {
            d->baseline_dbfs = (float) (10.0 * log10(d->baseline_power_sum / d->baseline_count));
            d->baseline_ready = true;
        }
        if (!d->manual) {
            return;  // auto mode: the baseline windows are not evaluated
        }
    }

    if (rms_dbfs > d->peak_dbfs) {
        d->peak_dbfs = rms_dbfs;
    }

    float thr = mic_detect_threshold_dbfs(d);
    if (!d->in_burst) {
        if (rms_dbfs > thr) {
            d->in_burst = true;
            d->bursts++;
        }
    } else if (rms_dbfs < thr - MIC_DETECT_RELEASE_DB) {
        d->in_burst = false;
    }
}
