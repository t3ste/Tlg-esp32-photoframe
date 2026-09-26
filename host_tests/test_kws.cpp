#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include "alarm_pattern.h"
#include "kws.h"
}

#ifndef KWS_TEST_DATA_DIR
#error "KWS_TEST_DATA_DIR must point at host_tests/data/kws"
#endif

// Speech samples made with the Windows text-to-speech voices (see
// scripts/generate_kws_test_audio.ps1): the stop word "Stopp" at several
// speaking speeds, and other words - unrelated ones and hard ones such as
// "Stock", "Spott" and "Stoppuhr".

namespace
{

using Pcm = std::vector<int16_t>;

Pcm load_wav(const std::string &name)
{
    std::string path = std::string(KWS_TEST_DATA_DIR) + "/" + name + ".wav";
    FILE *f = fopen(path.c_str(), "rb");
    Pcm v;
    if (!f) {
        ADD_FAILURE() << "missing test sample " << path;
        return v;
    }
    fseek(f, 44, SEEK_SET);  // canonical 44-byte WAV header
    int16_t s;
    while (fread(&s, sizeof(s), 1, f) == 1) {
        v.push_back(s);
    }
    fclose(f);
    return v;
}

std::unique_ptr<kws_pattern_t> pattern_of(const Pcm &pcm)
{
    auto p = std::make_unique<kws_pattern_t>();
    kws_status_t st = kws_extract(pcm.data(), pcm.size(), p.get(), nullptr, nullptr);
    EXPECT_EQ(st, KWS_OK);
    return p;
}

std::unique_ptr<kws_pattern_t> word(const std::string &name)
{
    return pattern_of(load_wav(name));
}

Pcm silence(size_t n)
{
    return Pcm(n, 0);
}

// Deterministic white noise.
Pcm noise(size_t n, double rms, unsigned seed)
{
    Pcm v(n);
    unsigned state = seed;
    for (auto &s : v) {
        double sum = 0;
        for (int i = 0; i < 12; i++) {  // sum of uniforms ~ Gaussian
            state = state * 1664525u + 1013904223u;
            sum += (double) (state >> 8) / (double) (1u << 24);
        }
        s = (int16_t) std::lround((sum - 6.0) * rms);
    }
    return v;
}

Pcm scaled(const Pcm &in, double gain)
{
    Pcm v(in.size());
    for (size_t i = 0; i < in.size(); i++) {
        double x = in[i] * gain;
        v[i] = (int16_t) std::max(-32768.0, std::min(32767.0, x));
    }
    return v;
}

Pcm mixed(const Pcm &a, const Pcm &b)
{
    Pcm v(std::max(a.size(), b.size()), 0);
    for (size_t i = 0; i < v.size(); i++) {
        int x = (i < a.size() ? a[i] : 0) + (i < b.size() ? b[i] : 0);
        v[i] = (int16_t) std::max(-32768, std::min(32767, x));
    }
    return v;
}

Pcm concat(std::initializer_list<Pcm> parts)
{
    Pcm v;
    for (const auto &p : parts) {
        v.insert(v.end(), p.begin(), p.end());
    }
    return v;
}

// A matcher enrolled with the keyword at two speeds, calibrated like the device does.
std::unique_ptr<kws_matcher_t> enrolled_matcher()
{
    auto m = std::make_unique<kws_matcher_t>();
    kws_matcher_init(m.get());
    EXPECT_EQ(kws_matcher_add(m.get(), word("stopp_hedda_r0").get()), 0);
    EXPECT_EQ(kws_matcher_add(m.get(), word("stopp_hedda_r2").get()), 1);
    kws_matcher_calibrate(m.get(), KWS_DEFAULT_MARGIN, KWS_DEFAULT_FLOOR_THRESHOLD);
    return m;
}

const char *const OTHER_WORDS[] = {"neg_alarm",     "neg_danke", "neg_guten_morgen", "neg_hallo",
                                   "neg_licht_aus", "neg_nein",  "neg_spott",        "neg_stock",
                                   "neg_stoppuhr",  "neg_weiter"};

}  // namespace

TEST(KwsExtract, SilenceAndTooShortInputHaveNoSpeech)
{
    kws_pattern_t p;
    EXPECT_EQ(kws_extract(silence(16000).data(), 16000, &p, nullptr, nullptr), KWS_ERR_NO_SPEECH);
    Pcm tiny(200, 1000);
    EXPECT_EQ(kws_extract(tiny.data(), tiny.size(), &p, nullptr, nullptr), KWS_ERR_NO_SPEECH);
    EXPECT_EQ(kws_extract(nullptr, 100, &p, nullptr, nullptr), KWS_ERR_ARG);
}

TEST(KwsExtract, QuietRoomNoiseIsNotSpeech)
{
    Pcm n = noise(32000, 12.0, 1);  // about -69 dBFS, like the frames' rooms
    kws_pattern_t p;
    EXPECT_EQ(kws_extract(n.data(), n.size(), &p, nullptr, nullptr), KWS_ERR_NO_SPEECH);
}

TEST(KwsExtract, FindsTheWordInsideSilence)
{
    Pcm w = load_wav("stopp_hedda_r0");
    Pcm clip = concat({silence(20000), w, silence(15000)});
    kws_pattern_t p;
    size_t start = 0, end = 0;
    ASSERT_EQ(kws_extract(clip.data(), clip.size(), &p, &start, &end), KWS_OK);
    EXPECT_GE(p.frames, 20);
    EXPECT_LE(p.frames, 60);
    // the region lies around the word (20000 .. 20000 + w.size())
    EXPECT_NEAR((double) start, 20000.0, 4000.0);
    EXPECT_NEAR((double) end, 20000.0 + (double) w.size(), 4000.0);
}

TEST(KwsExtract, OverLongSpeechIsRejected)
{
    // 1.9 s of continuous loud sound inside silence: longer than one pattern holds.
    Pcm long_sound = concat({silence(5000), noise(30000, 3000.0, 5), silence(5000)});
    kws_pattern_t p;
    EXPECT_EQ(kws_extract(long_sound.data(), long_sound.size(), &p, nullptr, nullptr),
              KWS_ERR_TOO_LONG);
}

TEST(KwsDtw, IdenticalPatternsHaveZeroDistance)
{
    auto a = word("stopp_hedda_r0");
    EXPECT_NEAR(kws_dtw_distance(a.get(), a.get()), 0.0f, 1.0e-4f);
}

TEST(KwsDtw, IsSymmetric)
{
    auto a = word("stopp_hedda_r0");
    auto b = word("neg_stock");
    EXPECT_NEAR(kws_dtw_distance(a.get(), b.get()), kws_dtw_distance(b.get(), a.get()), 1.0e-3f);
}

TEST(KwsDtw, SpeakingSpeedBarelyMattersButAnotherWordDoes)
{
    auto normal = word("stopp_hedda_r0");
    auto slower = word("stopp_hedda_r-2");
    auto faster = word("stopp_hedda_r2");
    auto stock = word("neg_stock");
    float same_slow = kws_dtw_distance(normal.get(), slower.get());
    float same_fast = kws_dtw_distance(normal.get(), faster.get());
    float other = kws_dtw_distance(normal.get(), stock.get());
    EXPECT_LT(same_slow, other);
    EXPECT_LT(same_fast, other);
}

TEST(KwsDtw, VeryDifferentDurationsAreRejectedOutright)
{
    auto stopp = word("stopp_hedda_r3");     // ~25 frames
    auto morgen = word("neg_guten_morgen");  // ~93 frames
    EXPECT_GE(kws_dtw_distance(stopp.get(), morgen.get()), KWS_DTW_INFINITE);
}

TEST(KwsMatcher, AcceptsTheKeywordAtOtherSpeedsAndRejectsOtherWords)
{
    auto m = enrolled_matcher();
    // Not enrolled: the slowest and the fastest reading of the keyword.
    for (const char *name : {"stopp_hedda_r-2", "stopp_hedda_r3"}) {
        float score = kws_matcher_score(m.get(), word(name).get());
        EXPECT_LT(score, m->threshold) << name << " score " << score << " thr " << m->threshold;
    }
    for (const char *name : OTHER_WORDS) {
        float score = kws_matcher_score(m.get(), word(name).get());
        EXPECT_GE(score, m->threshold) << name << " score " << score << " thr " << m->threshold;
    }
}

TEST(KwsMatcher, AnotherSpeakerIsNotAcceptedByAnEnrolledVoice)
{
    // Speaker-dependent by design: the English voice saying "Stop" must not pass
    // for the enrolled German voice's "Stopp".
    auto m = enrolled_matcher();
    float score = kws_matcher_score(m.get(), word("stopp_zira_r0").get());
    EXPECT_GE(score, m->threshold);
}

TEST(KwsMatcher, EnrollingTheSpeakerThemselvesWorks)
{
    kws_matcher_t *m = new kws_matcher_t;
    kws_matcher_init(m);
    kws_matcher_add(m, word("stopp_zira_r0").get());
    kws_matcher_calibrate(m, KWS_DEFAULT_MARGIN, KWS_DEFAULT_FLOOR_THRESHOLD);
    EXPECT_FLOAT_EQ(m->threshold, KWS_DEFAULT_FLOOR_THRESHOLD);  // one template: the floor
    EXPECT_LT(kws_matcher_score(m, word("stopp_zira_r0").get()), m->threshold);
    EXPECT_GE(kws_matcher_score(m, word("stopp_hedda_r0").get()), m->threshold);
    delete m;
}

TEST(KwsMatcher, CalibrationScalesWithTheSpeakersVariation)
{
    auto m = enrolled_matcher();
    // two templates: each one's nearest neighbour is the other one
    float spread = kws_dtw_distance(&m->templates[0], &m->templates[1]);
    EXPECT_NEAR(m->threshold, std::max(KWS_DEFAULT_MARGIN * spread, KWS_DEFAULT_FLOOR_THRESHOLD),
                1.0e-3f);
}

TEST(KwsMatcher, MoreEnrolmentsDoNotInflateTheThreshold)
{
    // Enrolling the same word again (different reading) must not raise the
    // acceptance threshold: a repetition is scored against its best template.
    auto two = enrolled_matcher();
    kws_matcher_t *three = new kws_matcher_t(*two);
    kws_matcher_add(three, word("stopp_hedda_r-2").get());
    kws_matcher_calibrate(three, KWS_DEFAULT_MARGIN, KWS_DEFAULT_FLOOR_THRESHOLD);
    EXPECT_LE(three->threshold, two->threshold + 1.0e-3f);
    // ...while the word "Stock" stays out
    EXPECT_GE(kws_matcher_score(three, word("neg_stock").get()), three->threshold);
    delete three;
}

TEST(KwsMatcher, FullAndEmptyMatchers)
{
    kws_matcher_t *m = new kws_matcher_t;
    kws_matcher_init(m);
    EXPECT_GE(kws_matcher_score(m, word("stopp_hedda_r0").get()), KWS_DTW_INFINITE);
    auto p = word("stopp_hedda_r0");
    for (int i = 0; i < KWS_MAX_TEMPLATES; i++) {
        EXPECT_EQ(kws_matcher_add(m, p.get()), i);
    }
    EXPECT_EQ(kws_matcher_add(m, p.get()), -1);
    delete m;
}

TEST(KwsRobustness, LevelDoesNotMatter)
{
    auto m = enrolled_matcher();
    Pcm w = load_wav("stopp_hedda_r-2");
    for (double gain : {0.05, 0.3, 1.0, 1.5}) {
        Pcm x = scaled(w, gain);
        Pcm clip = concat({silence(4000), x, silence(4000)});
        auto p = pattern_of(clip);
        float score = kws_matcher_score(m.get(), p.get());
        EXPECT_LT(score, m->threshold) << "gain " << gain << " score " << score;
    }
}

// Templates recorded in the same room see the same background noise as the
// later utterances, which is the situation on the device (enrolment happens
// on the frame itself).
TEST(KwsRobustness, MatchesInTheSameNoisyRoomItWasEnrolledIn)
{
    auto with_room_noise = [](const Pcm &w, unsigned seed) {
        double rms = 0;
        for (auto s : w) {
            rms += (double) s * s;
        }
        rms = std::sqrt(rms / (double) w.size());
        Pcm clip = concat({silence(6000), w, silence(6000)});
        return mixed(clip, noise(clip.size(), rms * 0.03, seed));  // about 30 dB below the speech
    };
    auto m = std::make_unique<kws_matcher_t>();
    kws_matcher_init(m.get());
    kws_matcher_add(m.get(), pattern_of(with_room_noise(load_wav("stopp_hedda_r0"), 11)).get());
    kws_matcher_add(m.get(), pattern_of(with_room_noise(load_wav("stopp_hedda_r2"), 12)).get());
    kws_matcher_calibrate(m.get(), KWS_DEFAULT_MARGIN, KWS_DEFAULT_FLOOR_THRESHOLD);

    for (const char *name : {"stopp_hedda_r-2", "stopp_hedda_r3"}) {
        float score =
            kws_matcher_score(m.get(), pattern_of(with_room_noise(load_wav(name), 7)).get());
        EXPECT_LT(score, m->threshold) << name << " score " << score << " thr " << m->threshold;
    }
    for (const char *name : OTHER_WORDS) {
        float score =
            kws_matcher_score(m.get(), pattern_of(with_room_noise(load_wav(name), 9)).get());
        EXPECT_GE(score, m->threshold) << name << " score " << score << " thr " << m->threshold;
    }
}

// Noise the templates never saw is the hard case: it may cost recall (known
// limitation - the keyword is enrolled on the frame itself, in its own room),
// but it must never create false accepts.
TEST(KwsRobustness, NoisyOtherWordsAreNeverAccepted)
{
    auto m = enrolled_matcher();
    for (const char *name : OTHER_WORDS) {
        Pcm w = load_wav(name);
        double rms = 0;
        for (auto s : w) {
            rms += (double) s * s;
        }
        rms = std::sqrt(rms / (double) w.size());
        for (double ratio : {0.03, 0.1, 0.3}) {
            Pcm clip = concat({silence(6000), w, silence(6000)});
            Pcm noisy = mixed(clip, noise(clip.size(), rms * ratio, 5));
            kws_pattern_t p;
            if (kws_extract(noisy.data(), noisy.size(), &p, nullptr, nullptr) == KWS_OK) {
                EXPECT_GE(kws_matcher_score(m.get(), &p), m->threshold)
                    << name << " noise ratio " << ratio;
            }
        }
    }
}

TEST(KwsRobustness, NoiseAndSilenceNeverMatch)
{
    auto m = enrolled_matcher();
    for (double rms : {10.0, 100.0, 1000.0, 5000.0}) {
        Pcm n = noise(20000, rms, 3);
        kws_pattern_t p;
        if (kws_extract(n.data(), n.size(), &p, nullptr, nullptr) == KWS_OK) {
            EXPECT_GE(kws_matcher_score(m.get(), &p), m->threshold) << "rms " << rms;
        }
    }
}

TEST(KwsStream, DetectsTheKeywordOnceInsideOtherSpeech)
{
    auto m = enrolled_matcher();
    Pcm audio =
        concat({silence(8000), load_wav("neg_danke"), silence(8000), load_wav("stopp_hedda_r-2"),
                silence(12000), load_wav("neg_stock"), silence(12000)});
    std::vector<int16_t> ring(32000);
    auto scratch = std::make_unique<kws_pattern_t>();
    kws_stream_t s;
    kws_stream_init(&s, ring.data(), ring.size(), scratch.get());

    int detections = 0;
    size_t detected_at = 0;
    // feed like the device does: 16 ms blocks
    for (size_t pos = 0; pos < audio.size(); pos += 256) {
        size_t n = std::min<size_t>(256, audio.size() - pos);
        float score = 0;
        if (kws_stream_push(&s, m.get(), audio.data() + pos, n, &score)) {
            detections++;
            detected_at = pos + n;
            EXPECT_LT(score, m->threshold);
        }
    }
    EXPECT_EQ(detections, 1);
    // during/after the keyword (which starts at ~8000 + danke + 8000)
    size_t keyword_start = 8000 + load_wav("neg_danke").size() + 8000;
    EXPECT_GT(detected_at, keyword_start);
    EXPECT_LT(detected_at, keyword_start + load_wav("stopp_hedda_r-2").size() + 12000);
    EXPECT_GE(s.utterances, 3u);  // the other two words were evaluated and rejected
}

TEST(KwsStream, OnlyOtherWordsNeverTrigger)
{
    auto m = enrolled_matcher();
    std::vector<int16_t> ring(32000);
    auto scratch = std::make_unique<kws_pattern_t>();
    kws_stream_t s;
    kws_stream_init(&s, ring.data(), ring.size(), scratch.get());
    int detections = 0;
    for (const char *name : OTHER_WORDS) {
        Pcm audio = concat({silence(9000), load_wav(name), silence(9000)});
        for (size_t pos = 0; pos < audio.size(); pos += 256) {
            size_t n = std::min<size_t>(256, audio.size() - pos);
            float score = 0;
            if (kws_stream_push(&s, m.get(), audio.data() + pos, n, &score)) {
                detections++;
            }
        }
    }
    EXPECT_EQ(detections, 0);
}

TEST(KwsStream, HandlesTinyAndHugeBlocks)
{
    auto m = enrolled_matcher();
    std::vector<int16_t> ring(32000);
    auto scratch = std::make_unique<kws_pattern_t>();
    kws_stream_t s;
    kws_stream_init(&s, ring.data(), ring.size(), scratch.get());
    float score = 0;
    int16_t one = 5;
    EXPECT_FALSE(kws_stream_push(&s, m.get(), &one, 1, &score));
    EXPECT_FALSE(kws_stream_push(&s, m.get(), &one, 0, &score));
    Pcm huge = silence(100000);  // more than the ring holds
    EXPECT_FALSE(kws_stream_push(&s, m.get(), huge.data(), huge.size(), &score));
    EXPECT_FALSE(kws_stream_push(&s, m.get(), nullptr, 0, &score));
}

// The alarm rings loudly while the stop word is spoken: the notes are muted for
// the detector (as alarm_manager.c does with alarm_pattern_tone_sounding()), so
// only the pauses are listened to.
TEST(KwsAlarm, StopWordInAPauseBetweenAlarmNotesIsHeard)
{
    auto m = enrolled_matcher();
    const int sr = KWS_SAMPLE_RATE;
    const size_t total = (size_t) sr * 14;
    Pcm audio(total, 0);

    // alarm tones (loud) at the start of every cycle
    alarm_note_t notes[64];
    int n = alarm_pattern_build(notes, 64, 14000, ALARM_TUNE_DEFAULT);
    size_t pos = 0;
    double phase = 0;
    for (int i = 0; i < n; i++) {
        size_t len = (size_t) notes[i].duration_ms * sr / 1000;
        for (size_t k = 0; k < len && pos + k < total; k++) {
            if (notes[i].freq_hz > 0) {
                phase += 2.0 * 3.14159265358979 * notes[i].freq_hz / sr;
                audio[pos + k] = (int16_t) (9000.0 * std::sin(phase));
            }
        }
        pos += len;
    }
    // the stop word spoken 2.3 s into the first pause (t = 1.2 s + 2.3 s), at speaking level
    Pcm word_pcm = load_wav("stopp_hedda_r-2");
    size_t at = (size_t) (3.5 * sr);
    for (size_t k = 0; k < word_pcm.size(); k++) {
        audio[at + k] =
            (int16_t) std::max(-32768, std::min(32767, (int) audio[at + k] + word_pcm[k]));
    }

    std::vector<int16_t> ring(32000);
    auto scratch = std::make_unique<kws_pattern_t>();
    kws_stream_t s;
    kws_stream_init(&s, ring.data(), ring.size(), scratch.get());
    int detections = 0;
    size_t detected_at = 0;
    for (size_t p = 0; p < total; p += 256) {
        size_t len = std::min<size_t>(256, total - p);
        Pcm block(audio.begin() + (long) p, audio.begin() + (long) (p + len));
        uint32_t t_ms = (uint32_t) ((p + len / 2) * 1000 / sr);
        if (alarm_pattern_tone_sounding(t_ms)) {
            std::fill(block.begin(), block.end(), 0);  // deaf while the notes sound
        }
        float score = 0;
        if (kws_stream_push(&s, m.get(), block.data(), len, &score)) {
            detections++;
            detected_at = p + len;
        }
    }
    EXPECT_EQ(detections, 1);
    EXPECT_GT(detected_at, at + word_pcm.size());
    EXPECT_LT(detected_at, (size_t) (6.2 * sr));  // before the next cycle's notes
}

TEST(KwsAlarm, AlarmNotesAloneNeverTrigger)
{
    auto m = enrolled_matcher();
    const int sr = KWS_SAMPLE_RATE;
    std::vector<int16_t> ring(32000);
    auto scratch = std::make_unique<kws_pattern_t>();
    kws_stream_t s;
    kws_stream_init(&s, ring.data(), ring.size(), scratch.get());
    alarm_note_t notes[64];
    int n = alarm_pattern_build(notes, 64, 20000, ALARM_TUNE_DEFAULT);
    int detections = 0;
    double phase = 0;
    size_t t = 0;
    for (int i = 0; i < n; i++) {
        size_t len = (size_t) notes[i].duration_ms * sr / 1000;
        for (size_t k = 0; k < len; k += 256) {
            size_t blen = std::min<size_t>(256, len - k);
            Pcm block(blen, 0);
            if (notes[i].freq_hz > 0) {
                for (size_t j = 0; j < blen; j++) {
                    phase += 2.0 * 3.14159265358979 * notes[i].freq_hz / sr;
                    block[j] = (int16_t) (9000.0 * std::sin(phase));
                }
            }
            // deliberately NOT muted: even unmuted, the notes are no keyword
            float score = 0;
            if (kws_stream_push(&s, m.get(), block.data(), blen, &score)) {
                detections++;
            }
            t += blen;
        }
    }
    (void) t;
    EXPECT_EQ(detections, 0);
}
