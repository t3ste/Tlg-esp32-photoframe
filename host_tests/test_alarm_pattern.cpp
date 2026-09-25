#include <gtest/gtest.h>

#include <vector>

extern "C" {
#include "alarm_pattern.h"
}

TEST(AlarmPattern, CycleIsFourNotesAndAFiveSecondPause)
{
    // board_hal_play_alarm() plays 4 x 300 ms notes then a 5 s pause; this pins
    // the mirrored constants (audio_chime.c has its own copy).
    EXPECT_EQ(ALARM_PATTERN_CYCLE_MS, 6200);
    alarm_note_t notes[5];
    ASSERT_EQ(alarm_pattern_build(notes, 5, ALARM_PATTERN_CYCLE_MS), 5);
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
        int n = alarm_pattern_build(notes.data(), count + 8, ms);
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
    EXPECT_EQ(alarm_pattern_build(notes, 3, 60000), 3);
    EXPECT_EQ(alarm_pattern_build(notes, 7, 60000), 7);
    EXPECT_EQ(alarm_pattern_build(notes, 0, 60000), 0);
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
