// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/core/keyframes_test.cpp — animating a clip's properties.
//
// Before this, every per-clip property was constant for the clip's whole length, so animation had to
// be faked by cutting a clip into pieces and giving each a different constant.
//
// The behaviour worth pinning is not that interpolation works — it is the two properties that make
// the feature additive and safe:
//
//   * An empty track answers "not animated" rather than "zero", so a caller can tell the difference
//     and an unanimated clip renders exactly as it did before keyframes existed.
//   * Outside the keyframed span the nearest value holds, so a property never falls to an implied
//     zero beyond the last keyframe.

#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include "core/Clip.hpp"
#include "core/Duration.hpp"
#include "core/EditCommands.hpp"
#include "core/Error.hpp"
#include "core/Keyframes.hpp"
#include "core/Project.hpp"
#include "core/Result.hpp"
#include "core/Uuid.hpp"

namespace palmier {
namespace {

Keyframe kf(double seconds, double value, Interpolation interpolation = Interpolation::Linear) {
    return Keyframe{Duration::fromSeconds(seconds), value, interpolation};
}

KeyframeTrack trackOf(AnimatableProperty property, std::vector<Keyframe> frames) {
    KeyframeTrack track;
    track.property = property;
    track.keyframes = std::move(frames);
    return track;
}

Project projectWithAnimatedClip(ClipId& idOut) {
    Clip clip;
    clip.id = Uuid::generateV4();
    clip.assetRef = MediaAssetRef(Uuid::generateV4(), "mem://asset");
    clip.timelineStart = Duration::zero();
    clip.sourceIn = Duration::zero();
    clip.sourceOut = Duration::fromSeconds(4.0);
    idOut = clip.id;

    Track track;
    track.id = Uuid::generateV4();
    track.kind = TrackKind::Video;
    track.clips.push_back(std::move(clip));

    Project project;
    project.id = Uuid::generateV4();
    project.name = "keyframes";
    project.timelineFps = FrameRate::fps30();
    project.canvas = Resolution::hd1080();
    project.assets.push_back(track.clips.front().assetRef);
    project.tracks.push_back(std::move(track));
    return project;
}

// --- the vocabulary --------------------------------------------------------

TEST(AnimatablePropertyNames, EveryPropertyRoundTripsThroughItsName) {
    for (const AnimatableProperty property : allAnimatableProperties()) {
        const std::optional<AnimatableProperty> parsed =
            parseAnimatableProperty(animatablePropertyName(property));
        ASSERT_TRUE(parsed.has_value()) << animatablePropertyName(property);
        EXPECT_EQ(*parsed, property);
    }
}

TEST(AnimatablePropertyNames, AnUnknownNameIsRefusedRatherThanDefaulted) {
    // A silently-defaulted property name is how an animation would end up on the wrong property, or
    // on none at all, with no error to show for it.
    EXPECT_FALSE(parseAnimatableProperty("temperature").has_value());
    EXPECT_FALSE(parseAnimatableProperty("").has_value());
    EXPECT_FALSE(parseInterpolation("bezier").has_value());
    EXPECT_TRUE(parseInterpolation("smooth").has_value());
}

// --- evaluation ------------------------------------------------------------

TEST(EvaluateKeyframeTrack, AnEmptyTrackIsNotAnimatedRatherThanZero) {
    const KeyframeTrack empty = trackOf(AnimatableProperty::Opacity, {});
    EXPECT_FALSE(evaluateKeyframeTrack(empty, Duration::fromSeconds(1.0)).has_value());
    // So a reader falls back to the clip's static value.
    EXPECT_DOUBLE_EQ(
        evaluateProperty({empty}, AnimatableProperty::Opacity, Duration::fromSeconds(1.0), 0.42),
        0.42);
}

TEST(EvaluateKeyframeTrack, InterpolatesLinearlyBetweenSurroundingKeyframes) {
    const KeyframeTrack track = trackOf(AnimatableProperty::Opacity, {kf(1.0, 0.0), kf(3.0, 1.0)});
    const auto at = [&](double s) {
        return evaluateKeyframeTrack(track, Duration::fromSeconds(s)).value_or(-1.0);
    };
    EXPECT_NEAR(at(1.0), 0.0, 1e-9);
    EXPECT_NEAR(at(2.0), 0.5, 1e-9);
    EXPECT_NEAR(at(2.5), 0.75, 1e-9);
    EXPECT_NEAR(at(3.0), 1.0, 1e-9);
}

TEST(EvaluateKeyframeTrack, OutsideTheSpanTheNearestValueHolds) {
    // Not zero: a clip keyframed only over its first second should stay where the last keyframe left
    // it for the rest of its length, rather than snapping to an implied zero.
    const KeyframeTrack track = trackOf(AnimatableProperty::Scale, {kf(1.0, 0.4), kf(2.0, 0.9)});
    EXPECT_NEAR(evaluateKeyframeTrack(track, Duration::zero()).value_or(-1.0), 0.4, 1e-9);
    EXPECT_NEAR(evaluateKeyframeTrack(track, Duration::fromSeconds(99.0)).value_or(-1.0), 0.9,
                1e-9);
}

TEST(EvaluateKeyframeTrack, ASingleKeyframeIsAConstant) {
    const KeyframeTrack track = trackOf(AnimatableProperty::Gain, {kf(5.0, 0.3)});
    EXPECT_NEAR(evaluateKeyframeTrack(track, Duration::zero()).value_or(-1.0), 0.3, 1e-9);
    EXPECT_NEAR(evaluateKeyframeTrack(track, Duration::fromSeconds(99.0)).value_or(-1.0), 0.3,
                1e-9);
}

TEST(EvaluateKeyframeTrack, SmoothEasesBothEndsAndMeetsLinearAtTheMidpoint) {
    const KeyframeTrack track =
        trackOf(AnimatableProperty::Scale, {kf(0.0, 0.0, Interpolation::Smooth), kf(2.0, 1.0)});
    const auto at = [&](double s) {
        return evaluateKeyframeTrack(track, Duration::fromSeconds(s)).value_or(-1.0);
    };
    EXPECT_NEAR(at(1.0), 0.5, 1e-9);
    // smoothstep(0.25) = 0.15625, behind the linear 0.25; and symmetrically ahead past halfway.
    EXPECT_NEAR(at(0.5), 0.15625, 1e-9);
    EXPECT_NEAR(at(1.5), 0.84375, 1e-9);
}

TEST(EvaluateKeyframeTrack, TheSegmentsShapeComesFromItsEarlierKeyframe) {
    // Two segments, only the first eased: the second must be linear.
    const KeyframeTrack track = trackOf(
        AnimatableProperty::Scale,
        {kf(0.0, 0.0, Interpolation::Smooth), kf(2.0, 1.0, Interpolation::Linear), kf(4.0, 2.0)});
    EXPECT_NEAR(evaluateKeyframeTrack(track, Duration::fromSeconds(0.5)).value_or(-1.0), 0.15625,
                1e-9);
    EXPECT_NEAR(evaluateKeyframeTrack(track, Duration::fromSeconds(3.0)).value_or(-1.0), 1.5, 1e-9);
}

TEST(EvaluateProperty, AnUnkeyframedPropertyKeepsItsFallback) {
    const std::vector<KeyframeTrack> tracks{
        trackOf(AnimatableProperty::Opacity, {kf(0.0, 0.2), kf(1.0, 0.8)})};
    EXPECT_NEAR(
        evaluateProperty(tracks, AnimatableProperty::Scale, Duration::fromSeconds(0.5), 0.77), 0.77,
        1e-9);
}

TEST(HasAnyKeyframes, IsFalseUntilSomeTrackHoldsAKeyframe) {
    std::vector<KeyframeTrack> tracks{trackOf(AnimatableProperty::Opacity, {}),
                                      trackOf(AnimatableProperty::Gain, {})};
    EXPECT_FALSE(hasAnyKeyframes(tracks));
    tracks[1].keyframes.push_back(kf(0.0, 1.0));
    EXPECT_TRUE(hasAnyKeyframes(tracks));
}

// --- ordering --------------------------------------------------------------

TEST(SortKeyframes, OrdersByTimeAndTheLaterOfASharedInstantWins) {
    // A property cannot hold two values at one moment, and refusing the edit would be a worse answer
    // than honouring the most recently given value.
    std::vector<Keyframe> frames = {kf(3.0, 0.3), kf(1.0, 0.1), kf(2.0, 0.2), kf(2.0, 0.9)};
    sortKeyframes(frames);

    ASSERT_EQ(frames.size(), 3u);
    EXPECT_EQ(frames[0].time.ticks(), Duration::fromSeconds(1.0).ticks());
    EXPECT_EQ(frames[1].time.ticks(), Duration::fromSeconds(2.0).ticks());
    EXPECT_NEAR(frames[1].value, 0.9, 1e-9);
    EXPECT_EQ(frames[2].time.ticks(), Duration::fromSeconds(3.0).ticks());
}

// --- the command -----------------------------------------------------------

TEST(SetKeyframesCommand, InstallsATrackAndRevertRestoresTheClipExactly) {
    ClipId id{};
    Project project = projectWithAnimatedClip(id);

    SetKeyframesCommand command(id, AnimatableProperty::Opacity, {kf(0.0, 0.0), kf(4.0, 1.0)});
    ASSERT_TRUE(command.apply(project).isOk());

    const Clip& clip = project.tracks.front().clips.front();
    ASSERT_EQ(clip.keyframes.size(), 1u);
    EXPECT_EQ(clip.keyframes.front().property, AnimatableProperty::Opacity);
    EXPECT_EQ(clip.keyframes.front().keyframes.size(), 2u);

    ASSERT_TRUE(command.revert(project).isOk());
    EXPECT_TRUE(project.tracks.front().clips.front().keyframes.empty());
}

TEST(SetKeyframesCommand, ReplacesOnlyItsOwnPropertysTrack) {
    ClipId id{};
    Project project = projectWithAnimatedClip(id);

    ASSERT_TRUE(
        SetKeyframesCommand(id, AnimatableProperty::Opacity, {kf(0.0, 0.1)}).apply(project).isOk());
    ASSERT_TRUE(
        SetKeyframesCommand(id, AnimatableProperty::Gain, {kf(0.0, 0.2)}).apply(project).isOk());
    EXPECT_EQ(project.tracks.front().clips.front().keyframes.size(), 2u);

    // Re-keying opacity replaces that track and leaves gain alone.
    ASSERT_TRUE(
        SetKeyframesCommand(id, AnimatableProperty::Opacity, {kf(1.0, 0.9)}).apply(project).isOk());
    const Clip& clip = project.tracks.front().clips.front();
    ASSERT_EQ(clip.keyframes.size(), 2u);
    EXPECT_NEAR(
        evaluateProperty(clip.keyframes, AnimatableProperty::Opacity, Duration::zero(), -1.0), 0.9,
        1e-9);
    EXPECT_NEAR(evaluateProperty(clip.keyframes, AnimatableProperty::Gain, Duration::zero(), -1.0),
                0.2, 1e-9);
}

TEST(SetKeyframesCommand, AnEmptyListClearsThePropertysAnimation) {
    ClipId id{};
    Project project = projectWithAnimatedClip(id);

    ASSERT_TRUE(
        SetKeyframesCommand(id, AnimatableProperty::Opacity, {kf(0.0, 0.1)}).apply(project).isOk());
    ASSERT_FALSE(project.tracks.front().clips.front().keyframes.empty());

    // The only way back to the static value, so it needs a spelling.
    ASSERT_TRUE(SetKeyframesCommand(id, AnimatableProperty::Opacity, {}).apply(project).isOk());
    EXPECT_TRUE(project.tracks.front().clips.front().keyframes.empty());
}

TEST(SetKeyframesCommand, SortsOnTheWayInSoEvaluationCanAssumeOrder) {
    ClipId id{};
    Project project = projectWithAnimatedClip(id);

    SetKeyframesCommand command(id, AnimatableProperty::Scale,
                                {kf(2.0, 1.0), kf(0.0, 0.0)});  // given out of order
    ASSERT_EQ(command.keyframes().size(), 2u);
    EXPECT_EQ(command.keyframes()[0].time.ticks(), Duration::zero().ticks());
    ASSERT_TRUE(command.apply(project).isOk());
    EXPECT_NEAR(evaluateProperty(project.tracks.front().clips.front().keyframes,
                                 AnimatableProperty::Scale, Duration::fromSeconds(1.0), -1.0),
                0.5, 1e-9);
}

TEST(SetKeyframesCommand, AnUnknownClipIsReportedAsNotFound) {
    ClipId id{};
    Project project = projectWithAnimatedClip(id);
    SetKeyframesCommand command(Uuid::generateV4(), AnimatableProperty::Opacity, {kf(0.0, 0.5)});

    const Result<void> applied = command.apply(project);
    ASSERT_TRUE(applied.isError());
    EXPECT_EQ(applied.error().code(), ErrorCode::NotFound);
    EXPECT_TRUE(project.tracks.front().clips.front().keyframes.empty());
}

TEST(SetKeyframesCommand, RevertBeforeASuccessfulApplyIsRefused) {
    ClipId id{};
    Project project = projectWithAnimatedClip(id);
    SetKeyframesCommand command(id, AnimatableProperty::Opacity, {kf(0.0, 0.5)});

    const Result<void> reverted = command.revert(project);
    ASSERT_TRUE(reverted.isError());
    EXPECT_EQ(reverted.error().code(), ErrorCode::FailedPrecondition);
}

}  // namespace
}  // namespace palmier
