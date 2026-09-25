#include "kws_service.h"

#include <string.h>

#include "board_hal.h"
#include "kws.h"

#if !BOARD_HAL_VOICE_ENABLED

// Compiled out to stubs unless this is an Alarm Clock build on a board with speaker + microphone.
esp_err_t kws_service_enroll(uint32_t seconds)
{
    (void) seconds;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t kws_service_clear(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t kws_service_test(uint32_t seconds)
{
    (void) seconds;
    return ESP_ERR_NOT_SUPPORTED;
}

void kws_service_get_status(kws_service_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->threshold = KWS_DEFAULT_FLOOR_THRESHOLD;
}

esp_err_t kws_service_set_alarm_stop(bool enabled)
{
    (void) enabled;
    return ESP_ERR_NOT_SUPPORTED;
}

bool kws_service_alarm_stop_ready(void)
{
    return false;
}

kws_listener_t *kws_service_listener_open(void)
{
    return NULL;
}

bool kws_service_listener_feed(kws_listener_t *l, const int16_t *stereo, size_t frames, bool mute)
{
    (void) l;
    (void) stereo;
    (void) frames;
    (void) mute;
    return false;
}

void kws_service_listener_close(kws_listener_t *l)
{
    (void) l;
}

#else

#include <stdio.h>
#include <stdlib.h>

#include "config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "power_manager.h"
#include "storage.h"

static const char *TAG = "kws";

#define TEMPLATE_FILE FS_MOUNT_POINT "/kws_templates.bin"
#define TEMPLATE_MAGIC 0x3153574Bu  // "KWS1"
#define STREAM_RING_SAMPLES 32000   // 2 s
#define TASK_STACK_BYTES 16384
#define MAX_ENROLL_SPREAD 8.0f  // enrolments further apart than this are not the same word

static kws_matcher_t *s_matcher = NULL;  // allocated on first use (PSRAM if available)
static volatile kws_service_mode_t s_mode = KWS_SERVICE_IDLE;
static kws_service_status_t s_last;  // enrolment/test results of the last runs

static kws_matcher_t *matcher(void)
{
    if (!s_matcher) {
        s_matcher = heap_caps_calloc(1, sizeof(kws_matcher_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_matcher) {
            s_matcher = calloc(1, sizeof(kws_matcher_t));
        }
        if (s_matcher) {
            kws_matcher_init(s_matcher);
        }
    }
    return s_matcher;
}

static void *big_alloc(size_t bytes)
{
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(bytes);
}

// ------------------------------------------------------------------ storage

static void save_templates(void)
{
    kws_matcher_t *m = matcher();
    if (!m || !storage_has_persistent_storage()) {
        return;
    }
    FILE *f = fopen(TEMPLATE_FILE, "wb");
    if (!f) {
        ESP_LOGW(TAG, "Could not write %s", TEMPLATE_FILE);
        return;
    }
    uint32_t magic = TEMPLATE_MAGIC;
    uint32_t count = (uint32_t) m->count;
    fwrite(&magic, sizeof(magic), 1, f);
    fwrite(&count, sizeof(count), 1, f);
    fwrite(&m->threshold, sizeof(m->threshold), 1, f);
    for (int i = 0; i < m->count; i++) {
        uint16_t frames = m->templates[i].frames;
        fwrite(&frames, sizeof(frames), 1, f);
        fwrite(m->templates[i].data, sizeof(float) * KWS_NUM_CEPS, frames, f);
    }
    fclose(f);
}

static void load_templates(void)
{
    kws_matcher_t *m = matcher();
    if (!m || !storage_has_persistent_storage()) {
        return;
    }
    FILE *f = fopen(TEMPLATE_FILE, "rb");
    if (!f) {
        return;
    }
    uint32_t magic = 0;
    uint32_t count = 0;
    float threshold = 0;
    if (fread(&magic, sizeof(magic), 1, f) == 1 && magic == TEMPLATE_MAGIC &&
        fread(&count, sizeof(count), 1, f) == 1 && count <= KWS_MAX_TEMPLATES &&
        fread(&threshold, sizeof(threshold), 1, f) == 1) {
        kws_matcher_init(m);
        for (uint32_t i = 0; i < count; i++) {
            uint16_t frames = 0;
            if (fread(&frames, sizeof(frames), 1, f) != 1 || frames == 0 ||
                frames > KWS_MAX_FRAMES) {
                break;
            }
            kws_pattern_t *p = &m->templates[m->count];
            p->frames = frames;
            if (fread(p->data, sizeof(float) * KWS_NUM_CEPS, frames, f) != frames) {
                break;
            }
            m->count++;
        }
        m->threshold = threshold;
    }
    fclose(f);
}

static void ensure_loaded(void)
{
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        load_templates();
    }
}

// ------------------------------------------------------------------ capture

typedef struct {
    int16_t *buf;  // mono samples (mean of both microphones)
    size_t cap;
    size_t len;
} enroll_ctx_t;

static bool enroll_block(const int16_t *stereo, size_t frames, void *user)
{
    enroll_ctx_t *c = user;
    for (size_t i = 0; i < frames && c->len < c->cap; i++) {
        c->buf[c->len++] = (int16_t) (((int) stereo[i * 2] + (int) stereo[i * 2 + 1]) / 2);
    }
    power_manager_reset_sleep_timer();
    return c->len < c->cap;
}

static void enroll_task(void *arg)
{
    uint32_t seconds = (uint32_t) (uintptr_t) arg;
    size_t cap = (size_t) seconds * KWS_SAMPLE_RATE;
    enroll_ctx_t ctx = {.buf = big_alloc(cap * sizeof(int16_t)), .cap = cap, .len = 0};
    kws_pattern_t *pattern = big_alloc(sizeof(kws_pattern_t));
    kws_matcher_t *m = matcher();

    s_last.have_enroll_result = true;
    s_last.enroll_status = KWS_ERR_ARG;
    s_last.enroll_frames = 0;
    if (!ctx.buf || !pattern || !m) {
        ESP_LOGE(TAG, "Out of memory for enrolment");
    } else {
        ESP_LOGI(TAG, "Enrolling: speak the word now (%u s)", (unsigned) seconds);
        esp_err_t err = board_hal_mic_capture(seconds * 1000u, enroll_block, &ctx);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Capture failed: %s", esp_err_to_name(err));
        } else {
            kws_status_t st = kws_extract(ctx.buf, ctx.len, pattern, NULL, NULL);
            s_last.enroll_status = st;
            if (st == KWS_OK) {
                // The new word must resemble the ones already enrolled.
                bool consistent = true;
                for (int i = 0; i < m->count; i++) {
                    if (kws_dtw_distance(pattern, &m->templates[i]) > MAX_ENROLL_SPREAD) {
                        consistent = false;
                    }
                }
                if (!consistent) {
                    ESP_LOGW(TAG, "Enrolment rejected: does not resemble the enrolled word");
                    s_last.enroll_status = KWS_ERR_NO_SPEECH;
                } else if (kws_matcher_add(m, pattern) >= 0) {
                    kws_matcher_calibrate(m, KWS_DEFAULT_MARGIN, KWS_DEFAULT_FLOOR_THRESHOLD);
                    save_templates();
                    s_last.enroll_frames = pattern->frames;
                    ESP_LOGI(TAG, "Enrolled template %d (%d frames), threshold %.2f", m->count,
                             pattern->frames, (double) m->threshold);
                }
            } else {
                ESP_LOGW(TAG, "No usable word heard (status %d)", (int) st);
            }
        }
    }
    free(ctx.buf);
    free(pattern);
    s_mode = KWS_SERVICE_IDLE;
    vTaskDelete(NULL);
}

// ------------------------------------------------------------------ alarm stop

#define KWS_NVS_NAMESPACE "kws"
#define KWS_NVS_ALARM_KEY "alarm"
static bool s_alarm_stop = false;
static bool s_alarm_stop_loaded = false;

static bool alarm_stop_enabled(void)
{
    if (!s_alarm_stop_loaded) {
        s_alarm_stop_loaded = true;
        nvs_handle_t h;
        if (nvs_open(KWS_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
            uint8_t v = 0;
            if (nvs_get_u8(h, KWS_NVS_ALARM_KEY, &v) == ESP_OK) {
                s_alarm_stop = v != 0;
            }
            nvs_close(h);
        }
    }
    return s_alarm_stop;
}

esp_err_t kws_service_set_alarm_stop(bool enabled)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(KWS_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, KWS_NVS_ALARM_KEY, enabled ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        s_alarm_stop_loaded = true;
        s_alarm_stop = enabled;
    }
    return err;
}

bool kws_service_alarm_stop_ready(void)
{
    kws_matcher_t *m = matcher();
    ensure_loaded();
    return alarm_stop_enabled() && m && m->count > 0;
}

// ------------------------------------------------------------------ listener

struct kws_listener {
    kws_stream_t stream;
    int16_t *ring;
    kws_pattern_t *scratch;
    unsigned seen;
};

static kws_listener_t *listener_open(void)
{
    kws_matcher_t *m = matcher();
    ensure_loaded();
    if (!m || m->count == 0) {
        return NULL;
    }
    kws_listener_t *l = calloc(1, sizeof(*l));
    if (!l) {
        return NULL;
    }
    l->ring = big_alloc(STREAM_RING_SAMPLES * sizeof(int16_t));
    l->scratch = big_alloc(sizeof(kws_pattern_t));
    if (!l->ring || !l->scratch) {
        free(l->ring);
        free(l->scratch);
        free(l);
        return NULL;
    }
    kws_stream_init(&l->stream, l->ring, STREAM_RING_SAMPLES, l->scratch);
    return l;
}

kws_listener_t *kws_service_listener_open(void)
{
    if (!kws_service_alarm_stop_ready()) {
        return NULL;
    }
    return listener_open();
}

// Returns true when the stop word was heard; @p scored_out counts utterances.
static bool listener_feed(kws_listener_t *l, const int16_t *stereo, size_t frames, bool mute)
{
    int16_t mono[256];
    if (frames > 256) {
        frames = 256;
    }
    for (size_t i = 0; i < frames; i++) {
        mono[i] = mute ? 0 : (int16_t) (((int) stereo[i * 2] + (int) stereo[i * 2 + 1]) / 2);
    }
    float score = 0;
    bool heard = kws_stream_push(&l->stream, s_matcher, mono, frames, &score);
    if (heard) {
        ESP_LOGI(TAG, "Keyword heard (distance %.2f < %.2f)", (double) score,
                 (double) s_matcher->threshold);
    }
    return heard;
}

bool kws_service_listener_feed(kws_listener_t *l, const int16_t *stereo, size_t frames, bool mute)
{
    return l ? listener_feed(l, stereo, frames, mute) : false;
}

void kws_service_listener_close(kws_listener_t *l)
{
    if (l) {
        free(l->ring);
        free(l->scratch);
        free(l);
    }
}

// Listens on the microphone and counts what the stop word detector hears.
static bool test_block(const int16_t *stereo, size_t frames, void *user)
{
    kws_listener_t *l = user;
    if (listener_feed(l, stereo, frames, false)) {
        s_last.test_detections++;
    }
    if (l->stream.utterances != l->seen) {
        l->seen = l->stream.utterances;
        s_last.test_utterances = l->seen;
        s_last.last_score = l->stream.last_score;
        if (l->stream.last_score < s_last.best_score) {
            s_last.best_score = l->stream.last_score;
        }
        ESP_LOGI(TAG, "Utterance %u: distance %.2f (threshold %.2f)", l->seen,
                 (double) l->stream.last_score, (double) s_matcher->threshold);
    }
    power_manager_reset_sleep_timer();
    return true;
}

static void test_task(void *arg)
{
    uint32_t seconds = (uint32_t) (uintptr_t) arg;
    kws_listener_t *l = listener_open();
    if (!l) {
        ESP_LOGE(TAG, "Out of memory for the test");
    } else {
        ESP_LOGI(TAG, "Listening for the stop word for %u s", (unsigned) seconds);
        esp_err_t err = board_hal_mic_capture(seconds * 1000u, test_block, l);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Capture failed: %s", esp_err_to_name(err));
        }
        ESP_LOGI(TAG, "Test done: %u utterances, %u keyword detections", s_last.test_utterances,
                 s_last.test_detections);
        kws_service_listener_close(l);
    }
    s_mode = KWS_SERVICE_IDLE;
    vTaskDelete(NULL);
}

// ------------------------------------------------------------------ API

esp_err_t kws_service_enroll(uint32_t seconds)
{
    if (seconds == 0 || seconds > KWS_SERVICE_ENROLL_MAX_SECONDS) {
        return ESP_ERR_INVALID_ARG;
    }
    kws_matcher_t *m = matcher();
    if (!m) {
        return ESP_ERR_NO_MEM;
    }
    ensure_loaded();
    if (s_mode != KWS_SERVICE_IDLE || m->count >= KWS_MAX_TEMPLATES) {
        return ESP_ERR_INVALID_STATE;
    }
    s_mode = KWS_SERVICE_ENROLLING;
    if (xTaskCreate(enroll_task, "kws_enroll", TASK_STACK_BYTES, (void *) (uintptr_t) seconds, 5,
                    NULL) != pdPASS) {
        s_mode = KWS_SERVICE_IDLE;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t kws_service_clear(void)
{
    kws_matcher_t *m = matcher();
    if (!m) {
        return ESP_ERR_NO_MEM;
    }
    ensure_loaded();
    if (s_mode != KWS_SERVICE_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }
    kws_matcher_init(m);
    memset(&s_last, 0, sizeof(s_last));
    if (storage_has_persistent_storage()) {
        remove(TEMPLATE_FILE);
    }
    return ESP_OK;
}

esp_err_t kws_service_test(uint32_t seconds)
{
    if (seconds == 0 || seconds > KWS_SERVICE_TEST_MAX_SECONDS) {
        return ESP_ERR_INVALID_ARG;
    }
    kws_matcher_t *m = matcher();
    if (!m) {
        return ESP_ERR_NO_MEM;
    }
    ensure_loaded();
    if (s_mode != KWS_SERVICE_IDLE || m->count == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    s_last.test_utterances = 0;
    s_last.test_detections = 0;
    s_last.best_score = KWS_DTW_INFINITE;
    s_last.last_score = KWS_DTW_INFINITE;
    s_mode = KWS_SERVICE_TESTING;
    if (xTaskCreate(test_task, "kws_test", TASK_STACK_BYTES, (void *) (uintptr_t) seconds, 5,
                    NULL) != pdPASS) {
        s_mode = KWS_SERVICE_IDLE;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void kws_service_get_status(kws_service_status_t *out)
{
    kws_matcher_t *m = matcher();
    ensure_loaded();
    *out = s_last;
    out->available = true;
    out->mode = s_mode;
    out->templates = m ? m->count : 0;
    out->threshold = m ? m->threshold : KWS_DEFAULT_FLOOR_THRESHOLD;
    out->alarm_stop = alarm_stop_enabled();
}

#endif  // BOARD_HAL_VOICE_ENABLED
