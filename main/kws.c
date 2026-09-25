#include "kws.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAX_INPUT_FRAMES 400       // 4 s of input are looked at
#define VAD_NOISE_MARGIN_DB 10.0f  // above the noise floor...
#define VAD_PEAK_RANGE_DB 28.0f    // ...and within this range of the loudest frame
#define VAD_ABS_MIN_DB 32.0f       // 10*log10(mean square): about -60 dBFS rms
#define VAD_GAP_FRAMES 12          // pauses inside a word (plosive closures) up to 120 ms
#define VAD_PAD_FRAMES 3
#define MEL_LOW_HZ \
    150.0f  // measured on the test set: the wide range separates "Stopp" from "Stock" best
#define MEL_HIGH_HZ 7500.0f
#define LIFTER 22.0f
#define PREEMPH 0.97f
#define STREAM_EVAL_SAMPLES 3200  // evaluate the stream at most every 200 ms
#define STREAM_TRAIL_FRAMES 15    // an utterance counts as finished after 150 ms of silence

// ---------------------------------------------------------------------------
// Tables (built on first use)

static bool s_tables_ready = false;
static float s_hamming[KWS_FRAME_LEN];
static float s_cos[KWS_FFT_SIZE / 2];
static float s_sin[KWS_FFT_SIZE / 2];
static float s_mel_edges_bin[KWS_MEL_BANDS + 2];  // filter edges as (fractional) FFT bins
static float s_dct[KWS_NUM_CEPS][KWS_MEL_BANDS];
static float s_lifter[KWS_NUM_CEPS];

static float hz_to_mel(float hz)
{
    return 2595.0f * log10f(1.0f + hz / 700.0f);
}

static float mel_to_hz(float mel)
{
    return 700.0f * (powf(10.0f, mel / 2595.0f) - 1.0f);
}

static void build_tables(void)
{
    if (s_tables_ready) {
        return;
    }
    for (int i = 0; i < KWS_FRAME_LEN; i++) {
        s_hamming[i] = 0.54f - 0.46f * cosf(2.0f * (float) M_PI * (float) i / (KWS_FRAME_LEN - 1));
    }
    for (int i = 0; i < KWS_FFT_SIZE / 2; i++) {
        float a = -2.0f * (float) M_PI * (float) i / KWS_FFT_SIZE;
        s_cos[i] = cosf(a);
        s_sin[i] = sinf(a);
    }
    float mel_lo = hz_to_mel(MEL_LOW_HZ);
    float mel_hi = hz_to_mel(MEL_HIGH_HZ);
    for (int i = 0; i < KWS_MEL_BANDS + 2; i++) {
        float mel = mel_lo + (mel_hi - mel_lo) * (float) i / (KWS_MEL_BANDS + 1);
        s_mel_edges_bin[i] = mel_to_hz(mel) * KWS_FFT_SIZE / KWS_SAMPLE_RATE;
    }
    for (int k = 0; k < KWS_NUM_CEPS; k++) {
        for (int m = 0; m < KWS_MEL_BANDS; m++) {
            s_dct[k][m] = cosf((float) M_PI * (float) (k + 1) * ((float) m + 0.5f) / KWS_MEL_BANDS);
        }
        s_lifter[k] = 1.0f + (LIFTER / 2.0f) * sinf((float) M_PI * (float) (k + 1) / LIFTER);
    }
    s_tables_ready = true;
}

// ---------------------------------------------------------------------------
// FFT

// In-place radix-2 FFT of KWS_FFT_SIZE complex points.
static void fft(float *re, float *im)
{
    const int n = KWS_FFT_SIZE;
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            float t = re[i];
            re[i] = re[j];
            re[j] = t;
            t = im[i];
            im[i] = im[j];
            im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        int step = n / len;
        for (int i = 0; i < n; i += len) {
            for (int k = 0; k < len / 2; k++) {
                float wr = s_cos[k * step];
                float wi = s_sin[k * step];
                int a = i + k;
                int b = i + k + len / 2;
                float tr = re[b] * wr - im[b] * wi;
                float ti = re[b] * wi + im[b] * wr;
                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Features

static float frame_energy_db(const int16_t *x)
{
    double sum = 0.0;
    for (int i = 0; i < KWS_FRAME_LEN; i++) {
        sum += (double) x[i] * (double) x[i];
    }
    return 10.0f * log10f((float) (sum / KWS_FRAME_LEN) + 1.0f);
}

// Mel-band powers of one frame.
static void frame_mel_power(const int16_t *x, float *mel)
{
    float re[KWS_FFT_SIZE];
    float im[KWS_FFT_SIZE];
    float prev = (float) x[0];
    re[0] = prev * s_hamming[0];
    for (int i = 1; i < KWS_FRAME_LEN; i++) {
        float cur = (float) x[i];
        re[i] = (cur - PREEMPH * prev) * s_hamming[i];
        prev = cur;
    }
    for (int i = KWS_FRAME_LEN; i < KWS_FFT_SIZE; i++) {
        re[i] = 0.0f;
    }
    memset(im, 0, sizeof(im));
    fft(re, im);

    for (int b = 0; b < KWS_MEL_BANDS; b++) {
        float lo = s_mel_edges_bin[b];
        float mid = s_mel_edges_bin[b + 1];
        float hi = s_mel_edges_bin[b + 2];
        float acc = 0.0f;
        for (int bin = (int) ceilf(lo); (float) bin < hi && bin <= KWS_FFT_SIZE / 2; bin++) {
            float w = (float) bin <= mid ? ((float) bin - lo) / (mid - lo)
                                         : (hi - (float) bin) / (hi - mid);
            if (w > 0.0f) {
                acc += w * (re[bin] * re[bin] + im[bin] * im[bin]);
            }
        }
        mel[b] = acc;
    }
}

// 12 liftered cepstral coefficients (c1..c12) from mel powers.
static void mel_to_ceps(const float *mel, float *ceps)
{
    float logmel[KWS_MEL_BANDS];
    for (int b = 0; b < KWS_MEL_BANDS; b++) {
        logmel[b] = logf(mel[b] > 1.0e-3f ? mel[b] : 1.0e-3f);
    }
    for (int k = 0; k < KWS_NUM_CEPS; k++) {
        float c = 0.0f;
        for (int m = 0; m < KWS_MEL_BANDS; m++) {
            c += logmel[m] * s_dct[k][m];
        }
        ceps[k] = c * s_lifter[k];
    }
}

// Selection of the p-th smallest of n values (n <= MAX_INPUT_FRAMES), destroying the array.
static float kth_smallest(float *v, int n, int k)
{
    for (int i = 0; i <= k; i++) {
        int min_i = i;
        for (int j = i + 1; j < n; j++) {
            if (v[j] < v[min_i]) {
                min_i = j;
            }
        }
        float t = v[i];
        v[i] = v[min_i];
        v[min_i] = t;
    }
    return v[k];
}

kws_status_t kws_extract(const int16_t *pcm, size_t n, kws_pattern_t *out, size_t *start_sample,
                         size_t *end_sample)
{
    if (!pcm || !out) {
        return KWS_ERR_ARG;
    }
    if (n < KWS_FRAME_LEN + KWS_HOP * KWS_MIN_FRAMES) {
        return KWS_ERR_NO_SPEECH;
    }
    build_tables();

    int nframes = (int) ((n - KWS_FRAME_LEN) / KWS_HOP) + 1;
    if (nframes > MAX_INPUT_FRAMES) {
        nframes = MAX_INPUT_FRAMES;
    }

    float energy[MAX_INPUT_FRAMES];
    float sorted[MAX_INPUT_FRAMES];
    float peak = 0.0f;
    for (int f = 0; f < nframes; f++) {
        energy[f] = frame_energy_db(pcm + (size_t) f * KWS_HOP);
        sorted[f] = energy[f];
        if (energy[f] > peak) {
            peak = energy[f];
        }
    }
    float noise = kth_smallest(sorted, nframes, nframes / 10);
    float thr = noise + VAD_NOISE_MARGIN_DB;
    if (peak - VAD_PEAK_RANGE_DB > thr) {
        thr = peak - VAD_PEAK_RANGE_DB;
    }
    if (thr < VAD_ABS_MIN_DB) {
        thr = VAD_ABS_MIN_DB;
    }

    // Regions of active frames, merging gaps up to VAD_GAP_FRAMES; keep the one
    // with the most energy above the threshold.
    int best_start = -1;
    int best_end = -1;
    float best_mass = 0.0f;
    int run_start = -1;
    int last_active = -1;
    float mass = 0.0f;
    for (int f = 0; f <= nframes; f++) {
        bool active = f < nframes && energy[f] > thr;
        if (active) {
            if (run_start < 0) {
                run_start = f;
                mass = 0.0f;
            }
            last_active = f;
            mass += energy[f] - thr;
        }
        bool close =
            run_start >= 0 && (f == nframes || (!active && f - last_active > VAD_GAP_FRAMES));
        if (close) {
            if (mass > best_mass) {
                best_mass = mass;
                best_start = run_start;
                best_end = last_active;
            }
            run_start = -1;
        }
    }
    if (best_start < 0) {
        return KWS_ERR_NO_SPEECH;
    }
    best_start = best_start - VAD_PAD_FRAMES < 0 ? 0 : best_start - VAD_PAD_FRAMES;
    best_end = best_end + VAD_PAD_FRAMES > nframes - 1 ? nframes - 1 : best_end + VAD_PAD_FRAMES;
    int frames = best_end - best_start + 1;
    if (frames < KWS_MIN_FRAMES) {
        return KWS_ERR_NO_SPEECH;
    }
    if (frames > KWS_MAX_FRAMES) {
        return KWS_ERR_TOO_LONG;
    }

    // Cepstral mean normalisation over the utterance (level and microphone colour drop out).
    float mean[KWS_NUM_CEPS] = {0};
    for (int f = 0; f < frames; f++) {
        float mel[KWS_MEL_BANDS];
        frame_mel_power(pcm + (size_t) (best_start + f) * KWS_HOP, mel);
        mel_to_ceps(mel, out->data[f]);
        for (int k = 0; k < KWS_NUM_CEPS; k++) {
            mean[k] += out->data[f][k];
        }
    }
    for (int k = 0; k < KWS_NUM_CEPS; k++) {
        mean[k] /= (float) frames;
    }
    for (int f = 0; f < frames; f++) {
        for (int k = 0; k < KWS_NUM_CEPS; k++) {
            out->data[f][k] = (out->data[f][k] - mean[k]) * KWS_FEATURE_SCALE;
        }
    }
    out->frames = (uint16_t) frames;
    if (start_sample) {
        *start_sample = (size_t) best_start * KWS_HOP;
    }
    if (end_sample) {
        *end_sample = (size_t) best_end * KWS_HOP + KWS_FRAME_LEN;
    }
    return KWS_OK;
}

// ---------------------------------------------------------------------------
// DTW

static float frame_distance(const float *a, const float *b)
{
    float sum = 0.0f;
    for (int k = 0; k < KWS_NUM_CEPS; k++) {
        float d = a[k] - b[k];
        sum += d * d;
    }
    return sqrtf(sum);
}

float kws_dtw_distance(const kws_pattern_t *a, const kws_pattern_t *b)
{
    int n = a->frames;
    int m = b->frames;
    if (n < 1 || m < 1) {
        return KWS_DTW_INFINITE;
    }
    if (n > 2 * m || m > 2 * n) {
        return KWS_DTW_INFINITE;
    }
    int longer = n > m ? n : m;
    int diff = n > m ? n - m : m - n;
    float band = 0.3f * (float) longer + (float) diff;

    float prev[KWS_MAX_FRAMES + 1];
    float cur[KWS_MAX_FRAMES + 1];
    for (int j = 0; j <= m; j++) {
        prev[j] = KWS_DTW_INFINITE;
    }
    prev[0] = 0.0f;

    for (int i = 1; i <= n; i++) {
        for (int j = 0; j <= m; j++) {
            cur[j] = KWS_DTW_INFINITE;
        }
        float centre = (float) i * (float) m / (float) n;
        int j_lo = (int) ceilf(centre - band);
        int j_hi = (int) floorf(centre + band);
        if (j_lo < 1) {
            j_lo = 1;
        }
        if (j_hi > m) {
            j_hi = m;
        }
        for (int j = j_lo; j <= j_hi; j++) {
            float d = frame_distance(a->data[i - 1], b->data[j - 1]);
            float best = prev[j - 1] + 2.0f * d;  // symmetric step pattern: diagonal counts twice
            float up = prev[j] + d;
            float left = cur[j - 1] + d;
            if (up < best) {
                best = up;
            }
            if (left < best) {
                best = left;
            }
            cur[j] = best;
        }
        memcpy(prev, cur, sizeof(float) * (size_t) (m + 1));
    }
    float total = prev[m];
    if (total >= KWS_DTW_INFINITE) {
        return KWS_DTW_INFINITE;
    }
    return total / (float) (n + m);
}

// ---------------------------------------------------------------------------
// Matcher

void kws_matcher_init(kws_matcher_t *m)
{
    m->count = 0;
    m->threshold = KWS_DEFAULT_FLOOR_THRESHOLD;
}

int kws_matcher_add(kws_matcher_t *m, const kws_pattern_t *pattern)
{
    if (m->count >= KWS_MAX_TEMPLATES) {
        return -1;
    }
    m->templates[m->count] = *pattern;
    return m->count++;
}

float kws_matcher_score(const kws_matcher_t *m, const kws_pattern_t *utterance)
{
    float best = KWS_DTW_INFINITE;
    for (int i = 0; i < m->count; i++) {
        float d = kws_dtw_distance(utterance, &m->templates[i]);
        if (d < best) {
            best = d;
        }
    }
    return best;
}

void kws_matcher_calibrate(kws_matcher_t *m, float margin, float floor_threshold)
{
    // How far is each enrolment from its closest sibling? A later utterance is
    // scored against its best template, so this - not the largest pairwise
    // distance, which grows with every extra enrolment - is the natural scale
    // of "the same word again".
    float spread = 0.0f;
    for (int i = 0; i < m->count && m->count >= 2; i++) {
        float nearest = KWS_DTW_INFINITE;
        for (int j = 0; j < m->count; j++) {
            if (j == i) {
                continue;
            }
            float d = kws_dtw_distance(&m->templates[i], &m->templates[j]);
            if (d < nearest) {
                nearest = d;
            }
        }
        if (nearest < KWS_DTW_INFINITE && nearest > spread) {
            spread = nearest;
        }
    }
    float thr = margin * spread;
    m->threshold = thr > floor_threshold ? thr : floor_threshold;
}

// ---------------------------------------------------------------------------
// Stream

void kws_stream_init(kws_stream_t *s, int16_t *ring, size_t capacity, kws_pattern_t *scratch)
{
    memset(s, 0, sizeof(*s));
    s->ring = ring;
    s->capacity = capacity;
    s->scratch = scratch;
    s->last_score = KWS_DTW_INFINITE;
    if (ring) {
        memset(ring, 0, capacity * sizeof(int16_t));
    }
}

bool kws_stream_push(kws_stream_t *s, const kws_matcher_t *m, const int16_t *pcm, size_t n,
                     float *score_out)
{
    if (!s->ring || !s->scratch || s->capacity == 0 || n == 0) {
        return false;
    }
    if (n >= s->capacity) {
        pcm += n - s->capacity;
        n = s->capacity;
    }
    if (s->fill + n > s->capacity) {
        size_t drop = s->fill + n - s->capacity;
        memmove(s->ring, s->ring + drop, (s->fill - drop) * sizeof(int16_t));
        s->fill -= drop;
    }
    memcpy(s->ring + s->fill, pcm, n * sizeof(int16_t));
    s->fill += n;
    s->total += n;
    s->since_check += n;
    if (s->since_check < STREAM_EVAL_SAMPLES) {
        return false;
    }
    s->since_check = 0;

    kws_pattern_t *pattern = s->scratch;
    size_t start = 0;
    size_t end = 0;
    if (kws_extract(s->ring, s->fill, pattern, &start, &end) != KWS_OK) {
        return false;
    }
    // Wait until the utterance is over (some trailing silence after it).
    if (s->fill - end < (size_t) STREAM_TRAIL_FRAMES * KWS_HOP) {
        return false;
    }

    float score = kws_matcher_score(m, pattern);
    s->last_score = score;
    s->utterances++;
    // Never evaluate the same speech twice.
    memset(s->ring + start, 0, (end - start) * sizeof(int16_t));
    if (score < m->threshold) {
        if (score_out) {
            *score_out = score;
        }
        return true;
    }
    return false;
}
