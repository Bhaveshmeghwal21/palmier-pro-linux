// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/ui/audio_meter_viewmodel_test.cpp — unit tests for the Qt-free meter
// logic (monitoring-and-grading Requirement 1.5, 1.6, 1.7).
//
// Both of the meter's rules are stated in real time, and both are the kind of
// rule that is normally either untested or tested by sleeping:
//
//   * Requirement 1.5 — a full-scale peak stays indicated for AT LEAST 1 second,
//     so a single over-scale sample cannot slip between two repaints;
//   * Requirement 1.6 — the peak-hold decays NO FASTER than 20 dB per second.
//
// Because AudioMeterViewModel::update() is told the instant its levels belong to
// rather than reading a clock, every case below drives simulated time and runs in
// microseconds. That is the entire reason the logic lives outside the widget.

#include <chrono>
#include <cmath>
#include <vector>

#include <gtest/gtest.h>

#include "media/AudioGraph.hpp"
#include "ui/AudioMeterViewModel.hpp"

namespace palmier::ui {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

/// Levels with an explicit per-channel peak; RMS is set to half the peak so the
/// two are always distinguishable in an assertion.
[[nodiscard]] media::AudioLevels levelsWithPeaks(std::vector<float> peaks) {
    media::AudioLevels levels;
    levels.peak = std::move(peaks);
    levels.rms.reserve(levels.peak.size());
    for (const float peak : levels.peak) {
        levels.rms.push_back(peak * 0.5f);
    }
    return levels;
}

/// A fixed origin for simulated time, so every case reads in absolute offsets.
[[nodiscard]] Clock::time_point origin() {
    static const Clock::time_point t0 = Clock::now();
    return t0;
}

TEST(AudioMeterViewModel, TheFirstUpdateDecaysNothingAndHoldsTheIncomingPeak) {
    AudioMeterViewModel model;
    model.update(levelsWithPeaks({0.6f}), /*playing=*/true, origin());

    ASSERT_EQ(model.channelCount(), 1u);
    EXPECT_NEAR(model.channels()[0].peak, 0.6f, 1.0e-5f);
    EXPECT_NEAR(model.channels()[0].rms, 0.3f, 1.0e-5f);
    // Nothing to decay from, so the hold is exactly the peak rather than a
    // fraction of it.
    EXPECT_NEAR(model.channels()[0].hold, 0.6f, 1.0e-5f);
    EXPECT_FALSE(model.channels()[0].clipped);
}

TEST(AudioMeterViewModel, TheHoldDecaysTwentyDecibelsPerSecondWhichIsAFactorOfTenASecond) {
    // Requirement 1.6, asserted against the arithmetic rather than against a
    // transcribed constant: 20 dB is a factor of 10, so one second of decay from
    // 1.0 must leave 0.1, and two seconds must leave 0.01.
    AudioMeterViewModel model;
    model.update(levelsWithPeaks({1.0f}), true, origin());
    ASSERT_NEAR(model.channels()[0].hold, 1.0f, 1.0e-5f);

    model.update(levelsWithPeaks({0.0f}), true, origin() + 1s);
    EXPECT_NEAR(model.channels()[0].hold, 0.1f, 1.0e-4f);

    model.update(levelsWithPeaks({0.0f}), true, origin() + 2s);
    EXPECT_NEAR(model.channels()[0].hold, 0.01f, 1.0e-4f);
}

TEST(AudioMeterViewModel, RepaintingMoreOftenDoesNotDecayTheHoldAnyFaster) {
    // The decay depends on elapsed time, not on update count. A widget that
    // repaints at 20 Hz must not decay twice as fast as one at 10 Hz, or the
    // 20 dB/s bound would be a function of the frame rate.
    AudioMeterViewModel coarse;
    AudioMeterViewModel fine;

    coarse.update(levelsWithPeaks({1.0f}), true, origin());
    fine.update(levelsWithPeaks({1.0f}), true, origin());

    // One 1000 ms step versus ten 100 ms steps, over the same second.
    coarse.update(levelsWithPeaks({0.0f}), true, origin() + 1000ms);
    for (int step = 1; step <= 10; ++step) {
        fine.update(levelsWithPeaks({0.0f}), true, origin() + std::chrono::milliseconds{100 * step});
    }

    EXPECT_NEAR(coarse.channels()[0].hold, fine.channels()[0].hold, 1.0e-4f);
}

TEST(AudioMeterViewModel, TwoUpdatesAtTheSameInstantDecayNothing) {
    AudioMeterViewModel model;
    model.update(levelsWithPeaks({0.8f}), true, origin());
    const float first = model.channels()[0].hold;

    model.update(levelsWithPeaks({0.0f}), true, origin());
    EXPECT_NEAR(model.channels()[0].hold, first, 1.0e-5f);
}

TEST(AudioMeterViewModel, TheHoldIsRaisedByALouderPeakButNeverLoweredByAQuieterOne) {
    AudioMeterViewModel model;
    model.update(levelsWithPeaks({0.9f}), true, origin());
    EXPECT_NEAR(model.channels()[0].hold, 0.9f, 1.0e-5f);

    // A much quieter peak an instant later leaves the hold where it was (bar the
    // negligible decay over 1 ms), which is the whole point of a peak hold.
    model.update(levelsWithPeaks({0.1f}), true, origin() + 1ms);
    EXPECT_GT(model.channels()[0].hold, 0.85f);
    EXPECT_NEAR(model.channels()[0].peak, 0.1f, 1.0e-5f);

    // A louder one raises it immediately.
    model.update(levelsWithPeaks({0.95f}), true, origin() + 2ms);
    EXPECT_NEAR(model.channels()[0].hold, 0.95f, 1.0e-5f);
}

TEST(AudioMeterViewModel, AFullScalePeakLatchesTheClipIndicationForAtLeastOneSecond) {
    // Requirement 1.5. The indication must survive a long gap between repaints,
    // so it is checked just before and just after the one-second boundary.
    AudioMeterViewModel model;
    model.update(levelsWithPeaks({1.0f}), true, origin());
    EXPECT_TRUE(model.channels()[0].clipped);
    EXPECT_TRUE(model.anyClipped());

    model.update(levelsWithPeaks({0.0f}), true, origin() + 999ms);
    EXPECT_TRUE(model.channels()[0].clipped) << "the latch must survive to 1 second";

    model.update(levelsWithPeaks({0.0f}), true, origin() + 1000ms);
    EXPECT_FALSE(model.channels()[0].clipped) << "and must clear once it has elapsed";
    EXPECT_FALSE(model.anyClipped());
}

TEST(AudioMeterViewModel, ALaterFullScalePeakExtendsTheLatchRatherThanRestartingAShorterOne) {
    AudioMeterViewModel model;
    model.update(levelsWithPeaks({1.0f}), true, origin());
    model.update(levelsWithPeaks({1.0f}), true, origin() + 500ms);

    // The second peak's own second has not elapsed at 1400 ms, even though the
    // first peak's has.
    model.update(levelsWithPeaks({0.0f}), true, origin() + 1400ms);
    EXPECT_TRUE(model.channels()[0].clipped);

    model.update(levelsWithPeaks({0.0f}), true, origin() + 1500ms);
    EXPECT_FALSE(model.channels()[0].clipped);
}

TEST(AudioMeterViewModel, APeakAboveFullScaleAlsoLatches) {
    // Requirement 1.5 says "at or above", and a mixer can hand over a sample past
    // full scale before any clamp.
    AudioMeterViewModel model;
    model.update(levelsWithPeaks({1.4f}), true, origin());
    EXPECT_TRUE(model.channels()[0].clipped);
}

TEST(AudioMeterViewModel, AStoppedTransportFallsToZeroRatherThanFreezingAtItsLastReading) {
    // Requirement 1.7. The levels the engine last measured are deliberately still
    // loud here: a stopped transport must read silence regardless, or a paused
    // editor would look like a playing one.
    AudioMeterViewModel model;
    model.update(levelsWithPeaks({0.9f}), /*playing=*/true, origin());
    ASSERT_NEAR(model.channels()[0].peak, 0.9f, 1.0e-5f);

    model.update(levelsWithPeaks({0.9f}), /*playing=*/false, origin() + 100ms);
    EXPECT_FLOAT_EQ(model.channels()[0].peak, 0.0f);
    EXPECT_FLOAT_EQ(model.channels()[0].rms, 0.0f);

    // The hold still decays from where it was rather than snapping to zero, so
    // the fall is visible instead of instantaneous.
    EXPECT_GT(model.channels()[0].hold, 0.0f);
    EXPECT_LT(model.channels()[0].hold, 0.9f);
}

TEST(AudioMeterViewModel, TheHoldEventuallyReachesExactlyZeroRatherThanApproachingItForever) {
    AudioMeterViewModel model;
    model.update(levelsWithPeaks({1.0f}), true, origin());
    model.update(levelsWithPeaks({0.0f}), true, origin() + 10s);

    EXPECT_FLOAT_EQ(model.channels()[0].hold, 0.0f);
}

TEST(AudioMeterViewModel, AChannelCountChangeResizesWithoutCarryingAStaleLatchOrHold) {
    // A project switch can change the channel count; channel 1 must not inherit
    // the old channel 1's latch.
    AudioMeterViewModel model;
    model.update(levelsWithPeaks({1.0f, 1.0f}), true, origin());
    ASSERT_EQ(model.channelCount(), 2u);
    ASSERT_TRUE(model.anyClipped());

    model.update(levelsWithPeaks({0.2f}), true, origin() + 10ms);
    ASSERT_EQ(model.channelCount(), 1u);
    EXPECT_FALSE(model.channels()[0].clipped);
    EXPECT_NEAR(model.channels()[0].hold, 0.2f, 1.0e-5f);
}

TEST(AudioMeterViewModel, AnEmptyMeasurementLeavesNoChannels) {
    AudioMeterViewModel model;
    model.update(media::AudioLevels{}, true, origin());

    EXPECT_EQ(model.channelCount(), 0u);
    EXPECT_FALSE(model.anyClipped());
}

TEST(AudioMeterViewModel, ResetClearsEveryLevelAndEveryLatch) {
    AudioMeterViewModel model;
    model.update(levelsWithPeaks({1.0f, 1.0f}), true, origin());
    ASSERT_TRUE(model.anyClipped());

    model.reset();
    EXPECT_EQ(model.channelCount(), 0u);
    EXPECT_FALSE(model.anyClipped());

    // And the next update behaves like a first update: no decay applied.
    model.update(levelsWithPeaks({0.5f}), true, origin() + 5s);
    EXPECT_NEAR(model.channels()[0].hold, 0.5f, 1.0e-5f);
}

TEST(AudioMeterViewModel, MeasuredLevelsFlowThroughFromTheRealMeasurementFunction) {
    // The seam the widget actually uses: media::measureLevels() feeding the model,
    // so the two halves are exercised together at least once rather than only in
    // isolation.
    std::vector<float> samples;
    for (std::size_t frame = 0; frame < 64; ++frame) {
        samples.push_back(1.0f);   // left at full scale
        samples.push_back(0.25f);  // right well below it
    }
    const media::AudioLevels measured =
        media::measureLevels(media::AudioBuffer::interleaved(48'000, 2, std::move(samples)));

    AudioMeterViewModel model;
    model.update(measured, true, origin());

    ASSERT_EQ(model.channelCount(), 2u);
    EXPECT_TRUE(model.channels()[0].clipped);
    EXPECT_FALSE(model.channels()[1].clipped);
    EXPECT_NEAR(model.channels()[1].peak, 0.25f, 1.0e-5f);
}

} // namespace
} // namespace palmier::ui
