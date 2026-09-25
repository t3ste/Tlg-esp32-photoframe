#include <gtest/gtest.h>

#include <vector>

extern "C" {
#include "mic_detect.h"
}

namespace
{

mic_detect_t run(const std::vector<float> &windows)
{
    mic_detect_t d;
    mic_detect_init(&d);
    for (float w : windows) {
        mic_detect_add_window(&d, w);
    }
    return d;
}

std::vector<float> quiet(int n, float db = -60.0f)
{
    return std::vector<float>((size_t) n, db);
}

void append(std::vector<float> &v, const std::vector<float> &more)
{
    v.insert(v.end(), more.begin(), more.end());
}

}  // namespace

TEST(MicDetect, RoomNoiseWobbleIsNotABurst)
{
    // Measured on a real frame: windows between -68 and -47 dBFS around a -65 floor.
    std::vector<float> w = quiet(5, -65.0f);
    for (float db : {-58.0f, -52.8f, -64.0f, -47.0f, -57.0f, -51.9f, -67.0f, -55.0f}) {
        w.push_back(db);
    }
    mic_detect_t d = run(w);
    EXPECT_EQ(d.bursts, 0u);
}

TEST(MicDetect, SteadyNoiseIsNotABurst)
{
    mic_detect_t d = run(quiet(30));
    EXPECT_TRUE(d.baseline_ready);
    EXPECT_NEAR(d.baseline_dbfs, -60.0f, 0.01f);
    EXPECT_EQ(d.bursts, 0u);
}

TEST(MicDetect, CountsToneBurstsSeparatedByPauses)
{
    std::vector<float> w = quiet(5);  // baseline
    append(w, quiet(3));
    append(w, quiet(2, -30.0f));  // burst 1 (400 ms)
    append(w, quiet(3));
    append(w, quiet(2, -30.0f));  // burst 2
    append(w, quiet(3));
    mic_detect_t d = run(w);
    EXPECT_EQ(d.bursts, 2u);
    EXPECT_NEAR(d.peak_dbfs, -30.0f, 0.01f);
}

TEST(MicDetect, ALoudClickInsideTheBaselineRaisesTheFloor)
{
    // One -20 dBFS window among the baseline windows dominates the power average
    // (about -27 dBFS), so a later -20 dBFS is only a 7 dB rise: not a burst.
    std::vector<float> w = {-60.0f, -60.0f, -20.0f, -60.0f, -60.0f};
    append(w, quiet(6, -20.0f));
    mic_detect_t d = run(w);
    EXPECT_TRUE(d.baseline_ready);
    EXPECT_GT(d.baseline_dbfs, -30.0f);
    EXPECT_EQ(d.bursts, 0u);
}

TEST(MicDetect, HysteresisKeepsOneBurstTogether)
{
    std::vector<float> w = quiet(5);
    w.push_back(-30.0f);  // above the -40 dBFS threshold (baseline -60 + 20) -> burst starts
    w.push_back(-41.5f);  // dips just below threshold but within the release margin
    w.push_back(-30.0f);
    append(w, quiet(3));
    mic_detect_t d = run(w);
    EXPECT_EQ(d.bursts, 1u);
}

TEST(MicDetect, SilentRoomNeedsAnAbsoluteMinimum)
{
    std::vector<float> w = quiet(5, -96.0f);
    w.push_back(-60.0f);  // room noise: below the -45 dBFS floor -> ignored
    w.push_back(-96.0f);
    w.push_back(-30.0f);  // real sound
    w.push_back(-96.0f);
    mic_detect_t d = run(w);
    EXPECT_EQ(d.bursts, 1u);
    EXPECT_NEAR(mic_detect_threshold_dbfs(&d), -45.0f, 0.01f);
}

TEST(MicDetect, NothingCountedWhileStillInBaseline)
{
    mic_detect_t d = run(quiet(4, -30.0f));
    EXPECT_FALSE(d.baseline_ready);
    EXPECT_EQ(d.bursts, 0u);
}

TEST(MicDetect, ManualThresholdCountsFromTheFirstWindow)
{
    mic_detect_t d;
    mic_detect_init(&d);
    mic_detect_set_manual(&d, -50.0f);
    for (float db : {-30.0f, -70.0f, -70.0f, -70.0f, -70.0f, -30.0f, -70.0f}) {
        mic_detect_add_window(&d, db);
    }
    EXPECT_EQ(d.bursts, 2u);  // the very first window already counts
    EXPECT_FLOAT_EQ(mic_detect_threshold_dbfs(&d), -50.0f);
}

TEST(MicDetect, ManualThresholdIgnoresQuieterSound)
{
    mic_detect_t d;
    mic_detect_init(&d);
    mic_detect_set_manual(&d, -20.0f);
    for (int i = 0; i < 12; i++) {
        mic_detect_add_window(&d, i % 2 ? -25.0f : -60.0f);
    }
    EXPECT_EQ(d.bursts, 0u);
    EXPECT_TRUE(d.baseline_ready);  // still measured for reporting
}

TEST(MicDetect, AutoThresholdFormula)
{
    EXPECT_FLOAT_EQ(mic_detect_auto_threshold_dbfs(-70.0f),
                    -45.0f);  // -70 + 20 is below the -45 minimum
    EXPECT_FLOAT_EQ(mic_detect_auto_threshold_dbfs(-30.0f), -10.0f);
}

TEST(MicFloor, FollowsSteadyNoiseAndIgnoresEvents)
{
    mic_floor_t f;
    mic_floor_init(&f);
    for (int i = 0; i < 40; i++) {
        mic_floor_update(&f, -60.0f);
    }
    EXPECT_NEAR(f.floor_dbfs, -60.0f, 0.5f);
    for (int i = 0; i < 4; i++) {
        mic_floor_update(&f, -20.0f);  // a tone burst
    }
    EXPECT_NEAR(f.floor_dbfs, -60.0f, 0.5f);
    for (int i = 0; i < 40; i++) {
        mic_floor_update(&f, -58.0f);  // slowly a bit louder room
    }
    EXPECT_NEAR(f.floor_dbfs, -58.0f, 0.5f);
}

TEST(MicFloor, RelocksWhenTheRoomStaysLouder)
{
    mic_floor_t f;
    mic_floor_init(&f);
    for (int i = 0; i < 20; i++) {
        mic_floor_update(&f, -65.0f);
    }
    for (unsigned i = 0; i < MIC_FLOOR_RELOCK_WINDOWS + 5; i++) {
        mic_floor_update(&f, -35.0f);  // e.g. a fan switched on
    }
    EXPECT_NEAR(f.floor_dbfs, -35.0f, 1.0f);
}
