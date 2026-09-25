#include <gtest/gtest.h>

#include <cmath>
#include <vector>

extern "C" {
#include "mic_level.h"
}

namespace
{

constexpr double kPi = 3.14159265358979323846;

std::vector<int16_t> stereo_sine(int amplitude, int frames, bool left_only = false)
{
    std::vector<int16_t> v((size_t) frames * 2, 0);
    for (int i = 0; i < frames; i++) {
        int16_t s = (int16_t) std::lround(amplitude * std::sin(2.0 * kPi * i / 32.0));
        v[(size_t) i * 2] = s;
        v[(size_t) i * 2 + 1] = left_only ? 0 : s;
    }
    return v;
}

mic_level_t level_of(const std::vector<int16_t> &v)
{
    mic_level_acc_t acc;
    mic_level_acc_reset(&acc);
    mic_level_acc_add(&acc, v.data(), v.size() / 2);
    return mic_level_acc_result(&acc);
}

}  // namespace

TEST(MicLevel, EmptyIsFloor)
{
    mic_level_acc_t acc;
    mic_level_acc_reset(&acc);
    mic_level_t l = mic_level_acc_result(&acc);
    EXPECT_FLOAT_EQ(l.rms_dbfs, MIC_LEVEL_FLOOR_DBFS);
    EXPECT_FLOAT_EQ(l.peak_dbfs, MIC_LEVEL_FLOOR_DBFS);
}

TEST(MicLevel, DigitalSilenceIsFloor)
{
    mic_level_t l = level_of(std::vector<int16_t>(512, 0));
    EXPECT_FLOAT_EQ(l.rms_dbfs, MIC_LEVEL_FLOOR_DBFS);
    EXPECT_FLOAT_EQ(l.peak_dbfs, MIC_LEVEL_FLOOR_DBFS);
}

TEST(MicLevel, ConstantOffsetIsRemoved)
{
    // A steady DC offset is not sound: it must not show up as a level.
    mic_level_t l = level_of(std::vector<int16_t>(512, 12000));
    EXPECT_FLOAT_EQ(l.rms_dbfs, MIC_LEVEL_FLOOR_DBFS);
    EXPECT_FLOAT_EQ(l.peak_dbfs, MIC_LEVEL_FLOOR_DBFS);
}

TEST(MicLevel, FullScaleSquareWaveIsZeroDb)
{
    std::vector<int16_t> v(1024);
    for (size_t i = 0; i < v.size() / 2; i++) {
        int16_t s = (i % 2) ? 32767 : -32767;
        v[i * 2] = s;
        v[i * 2 + 1] = s;
    }
    mic_level_t l = level_of(v);
    EXPECT_NEAR(l.rms_dbfs, 0.0f, 0.01f);
    EXPECT_NEAR(l.peak_dbfs, 0.0f, 0.01f);
}

TEST(MicLevel, HalfScaleSineRmsIsAboutMinus9Db)
{
    // amplitude 16384 = -6.02 dBFS peak, sine RMS is 3.01 dB lower.
    mic_level_t l = level_of(stereo_sine(16384, 3200));
    EXPECT_NEAR(l.peak_dbfs, -6.02f, 0.1f);
    EXPECT_NEAR(l.rms_dbfs, -9.03f, 0.1f);
}

TEST(MicLevel, ReportsTheLouderChannel)
{
    mic_level_t both = level_of(stereo_sine(8000, 3200));
    mic_level_t left_only = level_of(stereo_sine(8000, 3200, true));
    EXPECT_NEAR(both.rms_dbfs, left_only.rms_dbfs, 0.01f);
}

TEST(MicLevel, AccumulatesAcrossBlocks)
{
    std::vector<int16_t> quiet = stereo_sine(1000, 1600);
    std::vector<int16_t> loud = stereo_sine(16000, 1600);
    mic_level_acc_t acc;
    mic_level_acc_reset(&acc);
    mic_level_acc_add(&acc, quiet.data(), 1600);
    mic_level_acc_add(&acc, loud.data(), 1600);
    EXPECT_EQ(acc.frames, 3200u);
    mic_level_t l = mic_level_acc_result(&acc);
    EXPECT_NEAR(l.peak_dbfs, 20.0f * std::log10(16000.0f / 32768.0f), 0.1f);
    mic_level_t only_loud = level_of(loud);
    EXPECT_LT(l.rms_dbfs, only_loud.rms_dbfs);
}

TEST(MicLevel, BarMapsFloorToFull)
{
    char bar[16];
    mic_level_bar(-60.0f, -60.0f, bar, 10);
    EXPECT_STREQ(bar, "[----------]");
    mic_level_bar(0.0f, -60.0f, bar, 10);
    EXPECT_STREQ(bar, "[##########]");
    mic_level_bar(-30.0f, -60.0f, bar, 10);
    EXPECT_STREQ(bar, "[#####-----]");
    mic_level_bar(-96.0f, -60.0f, bar, 10);  // clamped below the floor
    EXPECT_STREQ(bar, "[----------]");
    mic_level_bar(5.0f, -60.0f, bar, 10);  // clamped above full scale
    EXPECT_STREQ(bar, "[##########]");
}
