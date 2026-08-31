// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/media_audio_levels_test.cpp — unit tests for media::measureLevels
// (monitoring-and-grading Requirement 1.1, 1.2, 1.9).
//
// Every case here is hand-computable on purpose. A meter is only worth having if
// its numbers are right, and "right" for peak and RMS is arithmetic anyone can
// check: a constant full-scale buffer must read 1.0 on both figures, a +/-1
// alternating buffer must also read 1.0 on both (RMS does not care about sign), a
// constant 0.5 buffer must read 0.5 on both, and a sine must read its amplitude
// on peak but amplitude/sqrt(2) on RMS. Getting the last pair the wrong way round
// is the classic metering bug, so it is asserted explicitly rather than implied.
//
// These run with no FFmpeg, no GPU, no audio device and no Qt, which is
// Requirement 1.9's whole point.

#include <cmath>
#include <vector>

#include <gtest/gtest.h>

#include "media/AudioGraph.hpp"

namespace palmier::media {
namespace {

constexpr int   kRate = 48'000;
constexpr float kEpsilon = 1.0e-5f;

/// An interleaved buffer whose every sample is `value`.
[[nodiscard]] AudioBuffer constantBuffer(int channels, std::size_t frames, float value) {
    std::vector<float> samples(frames * static_cast<std::size_t>(channels), value);
    return AudioBuffer::interleaved(kRate, channels, std::move(samples));
}

TEST(MeasureLevels, SilenceReadsZeroOnBothFiguresButStillReportsEveryChannel) {
    const AudioLevels levels = measureLevels(constantBuffer(2, 128, 0.0f));

    // Sized to the channel count rather than empty: a meter must still draw two
    // channels at zero, not forget how many channels there are.
    ASSERT_EQ(levels.peak.size(), 2u);
    ASSERT_EQ(levels.rms.size(), 2u);
    EXPECT_FLOAT_EQ(levels.peak[0], 0.0f);
    EXPECT_FLOAT_EQ(levels.peak[1], 0.0f);
    EXPECT_FLOAT_EQ(levels.rms[0], 0.0f);
    EXPECT_FLOAT_EQ(levels.rms[1], 0.0f);
    EXPECT_FALSE(levels.clippedOn(0));
    EXPECT_FLOAT_EQ(levels.peakAcrossChannels(), 0.0f);
}

TEST(MeasureLevels, ConstantFullScaleReadsOneOnBothFigures) {
    const AudioLevels levels = measureLevels(constantBuffer(1, 64, 1.0f));

    ASSERT_EQ(levels.peak.size(), 1u);
    EXPECT_NEAR(levels.peak[0], 1.0f, kEpsilon);
    EXPECT_NEAR(levels.rms[0], 1.0f, kEpsilon);
    EXPECT_TRUE(levels.clippedOn(0));
}

TEST(MeasureLevels, PeakIsAMagnitudeSoNegativeFullScaleAlsoReadsOne) {
    const AudioLevels levels = measureLevels(constantBuffer(1, 64, -1.0f));

    EXPECT_NEAR(levels.peak[0], 1.0f, kEpsilon);
    EXPECT_NEAR(levels.rms[0], 1.0f, kEpsilon);
    EXPECT_TRUE(levels.clippedOn(0));
}

TEST(MeasureLevels, ConstantHalfScaleReadsHalfOnBothFigures) {
    const AudioLevels levels = measureLevels(constantBuffer(1, 100, 0.5f));

    EXPECT_NEAR(levels.peak[0], 0.5f, kEpsilon);
    EXPECT_NEAR(levels.rms[0], 0.5f, kEpsilon);
    EXPECT_FALSE(levels.clippedOn(0));
}

TEST(MeasureLevels, AlternatingFullScaleReadsOneOnBothFiguresBecauseRmsIgnoresSign) {
    std::vector<float> samples(256);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i] = (i % 2 == 0) ? 1.0f : -1.0f;
    }
    const AudioLevels levels =
        measureLevels(AudioBuffer::interleaved(kRate, 1, std::move(samples)));

    EXPECT_NEAR(levels.peak[0], 1.0f, kEpsilon);
    EXPECT_NEAR(levels.rms[0], 1.0f, kEpsilon);
}

TEST(MeasureLevels, ASineReadsItsAmplitudeOnPeakAndAmplitudeOverRootTwoOnRms) {
    // The case that catches peak and RMS being swapped or scaled alike.
    //
    // Pi is spelled out rather than taken from M_PI: this project configures with
    // CMAKE_CXX_EXTENSIONS OFF, so the compiler runs in strict ISO mode where the
    // POSIX M_* macros are not guaranteed to be defined by <cmath>.
    constexpr double      kPi = 3.14159265358979323846;
    constexpr float       kAmplitude = 0.8f;
    constexpr std::size_t kPeriods = 50;
    constexpr std::size_t kFramesPerPeriod = 64;
    std::vector<float>    samples(kPeriods * kFramesPerPeriod);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const double phase = 2.0 * kPi * static_cast<double>(i) /
                             static_cast<double>(kFramesPerPeriod);
        samples[i] = kAmplitude * static_cast<float>(std::sin(phase));
    }
    const AudioLevels levels =
        measureLevels(AudioBuffer::interleaved(kRate, 1, std::move(samples)));

    EXPECT_NEAR(levels.peak[0], kAmplitude, 1.0e-3f);
    EXPECT_NEAR(levels.rms[0], kAmplitude / std::sqrt(2.0f), 1.0e-3f);
    // And the ordering itself, stated so a future change cannot quietly invert it.
    EXPECT_GT(levels.peak[0], levels.rms[0]);
}

TEST(MeasureLevels, ChannelsAreMeasuredIndependentlyAndNotAveragedTogether) {
    // Left at full scale, right at a quarter. A meter that averages the two, or
    // that measures only the interleaved stream as a whole, fails here.
    std::vector<float> samples;
    samples.reserve(200);
    for (std::size_t frame = 0; frame < 100; ++frame) {
        samples.push_back(1.0f);
        samples.push_back(0.25f);
    }
    const AudioLevels levels =
        measureLevels(AudioBuffer::interleaved(kRate, 2, std::move(samples)));

    ASSERT_EQ(levels.peak.size(), 2u);
    EXPECT_NEAR(levels.peak[0], 1.0f, kEpsilon);
    EXPECT_NEAR(levels.peak[1], 0.25f, kEpsilon);
    EXPECT_NEAR(levels.rms[0], 1.0f, kEpsilon);
    EXPECT_NEAR(levels.rms[1], 0.25f, kEpsilon);

    // Only the left channel is at or above full scale.
    EXPECT_TRUE(levels.clippedOn(0));
    EXPECT_FALSE(levels.clippedOn(1));
    EXPECT_NEAR(levels.peakAcrossChannels(), 1.0f, kEpsilon);
}

TEST(MeasureLevels, AnEmptyOrChannellessBufferMeasuresEmptyRatherThanFailing) {
    // Requirement 1.3's mechanism: the engine's suppressed quantum is zero-filled
    // silence and a degenerate buffer is empty; neither is an error, because the
    // `suppressed` flag on the report — not the levels — is what carries "no
    // output device".
    EXPECT_TRUE(measureLevels(AudioBuffer{}).peak.empty());
    EXPECT_TRUE(measureLevels(AudioBuffer{}).rms.empty());

    const AudioLevels noFrames = measureLevels(AudioBuffer(kRate, 2, 0));
    EXPECT_TRUE(noFrames.peak.empty());
    EXPECT_TRUE(noFrames.rms.empty());

    // Out-of-range channel queries answer false rather than reading past the end.
    EXPECT_FALSE(noFrames.clippedOn(0));
    EXPECT_FALSE(noFrames.clippedOn(-1));
    EXPECT_FALSE(noFrames.clippedOn(99));
}

TEST(MeasureLevels, AZeroFilledSuppressedStyleQuantumMeasuresZeroWithoutASpecialCase) {
    // Exactly the buffer AudioEngine::pump() submits when no output device is
    // available: a zero-filled quantum at the output format. Requirement 1.3
    // wants both figures at zero, which falls out of the same code path.
    const AudioLevels levels = measureLevels(AudioBuffer(kRate, 2, 512));

    ASSERT_EQ(levels.peak.size(), 2u);
    EXPECT_FLOAT_EQ(levels.peak[0], 0.0f);
    EXPECT_FLOAT_EQ(levels.rms[0], 0.0f);
    EXPECT_FLOAT_EQ(levels.peak[1], 0.0f);
    EXPECT_FLOAT_EQ(levels.rms[1], 0.0f);
}

TEST(MeasureLevels, MeasuringDoesNotModifyTheBuffer) {
    // Requirement 1.8: the measurement is read-only. Asserted rather than assumed
    // because a meter that normalises or clamps in place would corrupt the audio
    // on its way to the sink.
    const AudioBuffer        before = constantBuffer(2, 64, 0.75f);
    AudioBuffer              buffer = before;
    const AudioLevels        levels = measureLevels(buffer);

    EXPECT_EQ(buffer.samples(), before.samples());
    EXPECT_EQ(buffer.frameCount(), before.frameCount());
    EXPECT_EQ(buffer.channels(), before.channels());
    EXPECT_NEAR(levels.peak[0], 0.75f, kEpsilon);
}

} // namespace
} // namespace palmier::media
