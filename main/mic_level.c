#include "mic_level.h"

#include <math.h>

void mic_level_acc_reset(mic_level_acc_t *acc)
{
    for (int ch = 0; ch < 2; ch++) {
        acc->sum[ch] = 0;
        acc->sum_sq[ch] = 0;
        acc->max[ch] = INT32_MIN;
        acc->min[ch] = INT32_MAX;
    }
    acc->frames = 0;
}

void mic_level_acc_add(mic_level_acc_t *acc, const int16_t *stereo, size_t frames)
{
    for (size_t i = 0; i < frames; i++) {
        for (int ch = 0; ch < 2; ch++) {
            int32_t v = stereo[i * 2 + (size_t) ch];
            acc->sum[ch] += v;
            acc->sum_sq[ch] += (int64_t) v * v;
            if (v > acc->max[ch]) {
                acc->max[ch] = v;
            }
            if (v < acc->min[ch]) {
                acc->min[ch] = v;
            }
        }
    }
    acc->frames += (uint32_t) frames;
}

static float to_dbfs(double amplitude)
{
    if (amplitude <= 0.0) {
        return MIC_LEVEL_FLOOR_DBFS;
    }
    float db = (float) (20.0 * log10(amplitude / 32768.0));
    return db < MIC_LEVEL_FLOOR_DBFS ? MIC_LEVEL_FLOOR_DBFS : db;
}

mic_level_t mic_level_acc_result(const mic_level_acc_t *acc)
{
    mic_level_t out = {MIC_LEVEL_FLOOR_DBFS, MIC_LEVEL_FLOOR_DBFS};
    if (acc->frames == 0) {
        return out;
    }
    for (int ch = 0; ch < 2; ch++) {
        double mean = (double) acc->sum[ch] / (double) acc->frames;
        double var = (double) acc->sum_sq[ch] / (double) acc->frames - mean * mean;
        double rms = var > 0.0 ? sqrt(var) : 0.0;
        double peak_hi = (double) acc->max[ch] - mean;
        double peak_lo = mean - (double) acc->min[ch];
        double peak = peak_hi > peak_lo ? peak_hi : peak_lo;

        float rms_db = to_dbfs(rms);
        float peak_db = to_dbfs(peak);
        if (rms_db > out.rms_dbfs) {
            out.rms_dbfs = rms_db;
        }
        if (peak_db > out.peak_dbfs) {
            out.peak_dbfs = peak_db;
        }
    }
    return out;
}

void mic_level_bar(float dbfs, float floor_dbfs, char *out, size_t width)
{
    float fraction = (dbfs - floor_dbfs) / (0.0f - floor_dbfs);
    if (fraction < 0.0f) {
        fraction = 0.0f;
    }
    if (fraction > 1.0f) {
        fraction = 1.0f;
    }
    size_t filled = (size_t) (fraction * (float) width + 0.5f);
    out[0] = '[';
    for (size_t i = 0; i < width; i++) {
        out[1 + i] = i < filled ? '#' : '-';
    }
    out[width + 1] = ']';
    out[width + 2] = '\0';
}
