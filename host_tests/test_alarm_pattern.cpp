#include <gtest/gtest.h>

#include <cmath>
#include <vector>

extern "C" {
#include "alarm_pattern.h"
#include "alarm_ramp.h"
}

TEST(AlarmPattern, CycleIsFourNotesAndAFiveSecondPause)
{
    // board_hal_play_alarm() plays 4 x 300 ms notes then a 5 s pause; this pins
    // the mirrored constants (audio_chime.c has its own copy).
    EXPECT_EQ(ALARM_PATTERN_CYCLE_MS, 6200);
    alarm_note_t notes[5];
    ASSERT_EQ(alarm_pattern_build(notes, 5, ALARM_PATTERN_CYCLE_MS, ALARM_TUNE_DEFAULT), 5);
    EXPECT_FLOAT_EQ(notes[0].freq_hz, 392.0f);
    EXPECT_FLOAT_EQ(notes[1].freq_hz, 523.0f);
    EXPECT_FLOAT_EQ(notes[2].freq_hz, 659.0f);
    EXPECT_FLOAT_EQ(notes[3].freq_hz, 523.0f);
    for (int i = 0; i < 4; i++) {
        EXPECT_EQ(notes[i].duration_ms, 300);
    }
    EXPECT_FLOAT_EQ(notes[4].freq_hz, 0.0f);
    EXPECT_EQ(notes[4].duration_ms, 5000);
}

TEST(AlarmPattern, BuildCoversTheWholeRingDurationAndCountMatches)
{
    for (uint32_t ms : {1u, 6200u, 6201u, 30000u, 60000u, 300000u}) {
        int count = alarm_pattern_count(ms);
        std::vector<alarm_note_t> notes((size_t) count + 8);
        int n = alarm_pattern_build(notes.data(), count + 8, ms, ALARM_TUNE_DEFAULT);
        EXPECT_EQ(n, count) << ms << " ms";
        uint64_t total = 0;
        for (int i = 0; i < n; i++) {
            total += (uint64_t) notes[i].duration_ms;
        }
        EXPECT_GE(total, ms);
        EXPECT_LT(total, (uint64_t) ms + ALARM_PATTERN_CYCLE_MS);
    }
}

TEST(AlarmPattern, BuildNeverWritesPastMax)
{
    alarm_note_t notes[7];
    EXPECT_EQ(alarm_pattern_build(notes, 3, 60000, ALARM_TUNE_DEFAULT), 3);
    EXPECT_EQ(alarm_pattern_build(notes, 7, 60000, ALARM_TUNE_DEFAULT), 7);
    EXPECT_EQ(alarm_pattern_build(notes, 0, 60000, ALARM_TUNE_DEFAULT), 0);
}

TEST(AlarmPattern, ToneSoundingOnlyDuringNotesAndTheirEcho)
{
    EXPECT_TRUE(alarm_pattern_tone_sounding(0));
    EXPECT_TRUE(alarm_pattern_tone_sounding(1100));
    EXPECT_TRUE(alarm_pattern_tone_sounding(1200 + ALARM_PATTERN_TAIL_MS - 1));
    EXPECT_FALSE(alarm_pattern_tone_sounding(1200 + ALARM_PATTERN_TAIL_MS));
    EXPECT_FALSE(alarm_pattern_tone_sounding(3000));
    EXPECT_FALSE(alarm_pattern_tone_sounding(6199));
    EXPECT_TRUE(alarm_pattern_tone_sounding(6200));  // next cycle
    EXPECT_TRUE(alarm_pattern_tone_sounding(6200 + 1000));
    EXPECT_FALSE(alarm_pattern_tone_sounding(6200 * 7 + 3000));
}

TEST(AlarmPattern, ThePauseLeavesRoomForAWord)
{
    // A stop word needs about 1.5 s of quiet (word + trailing silence for the
    // end-of-utterance decision): the pause minus the echo tail is plenty.
    EXPECT_GE(ALARM_PATTERN_PAUSE_MS - ALARM_PATTERN_TAIL_MS, 3000);
}

TEST(AlarmTunes, EveryTuneIsFourAudibleNotesAndTheDefaultIsG4C5E5C5)
{
    const float *def = alarm_pattern_tune(ALARM_TUNE_DEFAULT);
    EXPECT_FLOAT_EQ(def[0], 392.0f);
    EXPECT_FLOAT_EQ(def[1], 523.0f);
    EXPECT_FLOAT_EQ(def[2], 659.0f);
    EXPECT_FLOAT_EQ(def[3], 523.0f);
    for (int tune = 0; tune < ALARM_TUNE_COUNT; tune++) {
        const float *n = alarm_pattern_tune(tune);
        for (int i = 0; i < ALARM_PATTERN_NOTE_COUNT; i++) {
            EXPECT_GE(n[i], 300.0f) << tune;  // inside the speaker's useful range
            EXPECT_LE(n[i], 1000.0f) << tune;
        }
        // tunes differ from each other
        for (int other = tune + 1; other < ALARM_TUNE_COUNT; other++) {
            bool same = true;
            for (int i = 0; i < ALARM_PATTERN_NOTE_COUNT; i++) {
                same = same && n[i] == alarm_pattern_tune(other)[i];
            }
            EXPECT_FALSE(same) << tune << " vs " << other;
        }
    }
}

TEST(AlarmTunes, TheRequestedMelodies)
{
    auto near = [](const float *n, float a, float b, float c, float d) {
        return std::abs(n[0] - a) < 2 && std::abs(n[1] - b) < 2 && std::abs(n[2] - c) < 2 &&
               std::abs(n[3] - d) < 2;
    };
    EXPECT_TRUE(near(alarm_pattern_tune(1), 523.25f, 659.26f, 783.99f, 659.26f));  // C5 E5 G5 E5
    EXPECT_TRUE(near(alarm_pattern_tune(2), 440.0f, 523.25f, 659.26f, 523.25f));   // A4 C5 E5 C5
    EXPECT_TRUE(near(alarm_pattern_tune(3), 392.0f, 587.33f, 493.88f, 587.33f));   // G4 D5 B4 D5
    EXPECT_TRUE(near(alarm_pattern_tune(4), 349.23f, 440.0f, 523.25f, 440.0f));    // F4 A4 C5 A4
    EXPECT_TRUE(near(alarm_pattern_tune(5), 523.25f, 392.0f, 659.26f, 523.25f));   // C5 G4 E5 C5
}

TEST(AlarmTunes, UnknownTuneFallsBackToTheDefaultAndBuildUsesTheTune)
{
    EXPECT_EQ(alarm_pattern_tune(-1), alarm_pattern_tune(ALARM_TUNE_DEFAULT));
    EXPECT_EQ(alarm_pattern_tune(ALARM_TUNE_COUNT), alarm_pattern_tune(ALARM_TUNE_DEFAULT));
    alarm_note_t notes[5];
    ASSERT_EQ(alarm_pattern_build(notes, 5, ALARM_PATTERN_CYCLE_MS, 3), 5);
    EXPECT_FLOAT_EQ(notes[1].freq_hz, 587.0f);
    EXPECT_FLOAT_EQ(notes[4].freq_hz, 0.0f);  // still the same pause
}

TEST(AlarmRamp, NoRampIsFullVolumeAtOnce)
{
    EXPECT_FLOAT_EQ(alarm_ramp_gain(0, 0), 1.0f);
    EXPECT_FLOAT_EQ(alarm_ramp_gain(123456, 0), 1.0f);
}

TEST(AlarmRamp, RisesLinearlyFromAQuietAudibleStartToFullAtTheEnd)
{
    const uint32_t ramp = 60000;
    EXPECT_FLOAT_EQ(alarm_ramp_gain(0, ramp), ALARM_RAMP_START_GAIN);
    EXPECT_NEAR(alarm_ramp_gain(ramp / 2, ramp), (1.0f + ALARM_RAMP_START_GAIN) / 2, 1e-4);
    EXPECT_FLOAT_EQ(alarm_ramp_gain(ramp, ramp), 1.0f);
    EXPECT_FLOAT_EQ(alarm_ramp_gain(ramp * 3, ramp), 1.0f);
    float prev = 0.0f;
    for (uint32_t t = 0; t <= ramp; t += 1000) {
        float g = alarm_ramp_gain(t, ramp);
        EXPECT_GE(g, prev);
        EXPECT_LE(g, 1.0f);
        prev = g;
    }
    EXPECT_GT(ALARM_RAMP_START_GAIN, 0.02f);  // not inaudible at the start
}
