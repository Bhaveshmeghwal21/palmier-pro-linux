// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/core/clip_properties_command_test.cpp — changing a clip's properties after creation.
//
// `opacity` and `gain` could previously only be passed to `timeline.add_clip`, which made them
// write-once across the whole 42-tool surface: adjusting a composite's blend or a music bed's
// level meant deleting the clip and adding it back, losing its identifier and everything
// attached to it. Upstream's `set_clip_properties` changes them at any time.
//
// The behaviour worth pinning is not that a field can be assigned — it is that an ABSENT field
// is left alone (so one property can be changed without knowing the other), and that revert
// restores the clip exactly, including a field the command never touched.

#include <optional>

#include <gtest/gtest.h>

#include "core/Clip.hpp"
#include "core/ClipGeometry.hpp"
#include "core/Duration.hpp"
#include "core/EditCommands.hpp"
#include "core/Error.hpp"
#include "core/Project.hpp"
#include "core/Result.hpp"
#include "core/Uuid.hpp"

namespace palmier {
namespace {

using Properties = SetClipPropertiesCommand::Properties;

/// A project holding one video track with one clip at the given opacity and gain.
Project projectWithClip(ClipId clipId, double opacity, double gain) {
    Clip clip;
    clip.id = clipId;
    clip.assetRef = MediaAssetRef(Uuid::generateV4(), "mem://asset");
    clip.timelineStart = Duration::zero();
    clip.sourceIn = Duration::zero();
    clip.sourceOut = Duration::fromSeconds(2.0);
    clip.opacity = opacity;
    clip.gain = gain;

    Track track;
    track.id = Uuid::generateV4();
    track.kind = TrackKind::Video;
    track.clips.push_back(std::move(clip));

    Project project;
    project.id = Uuid::generateV4();
    project.name = "clip properties";
    project.tracks.push_back(std::move(track));
    return project;
}

const Clip& onlyClip(const Project& project) { return project.tracks.at(0).clips.at(0); }

TEST(SetClipProperties, SettingOpacityLeavesGainAlone) {
    const ClipId id = Uuid::generateV4();
    Project project = projectWithClip(id, 1.0, 0.8);

    Properties props;
    props.opacity = 0.25;
    SetClipPropertiesCommand command(id, props);

    ASSERT_TRUE(command.apply(project).isOk());
    EXPECT_DOUBLE_EQ(onlyClip(project).opacity, 0.25);
    EXPECT_DOUBLE_EQ(onlyClip(project).gain, 0.8);
}

TEST(SetClipProperties, SettingGainLeavesOpacityAlone) {
    const ClipId id = Uuid::generateV4();
    Project project = projectWithClip(id, 0.4, 1.0);

    Properties props;
    props.gain = 0.5;
    SetClipPropertiesCommand command(id, props);

    ASSERT_TRUE(command.apply(project).isOk());
    EXPECT_DOUBLE_EQ(onlyClip(project).gain, 0.5);
    EXPECT_DOUBLE_EQ(onlyClip(project).opacity, 0.4);
}

TEST(SetClipProperties, BothPropertiesCanChangeInOneEdit) {
    const ClipId id = Uuid::generateV4();
    Project project = projectWithClip(id, 1.0, 1.0);

    Properties props;
    props.opacity = 0.6;
    props.gain = 2.0;
    SetClipPropertiesCommand command(id, props);

    ASSERT_TRUE(command.apply(project).isOk());
    EXPECT_DOUBLE_EQ(onlyClip(project).opacity, 0.6);
    EXPECT_DOUBLE_EQ(onlyClip(project).gain, 2.0);
}

TEST(SetClipProperties, RevertRestoresBothValuesIncludingTheUntouchedOne) {
    const ClipId id = Uuid::generateV4();
    Project project = projectWithClip(id, 0.3, 0.7);

    Properties props;
    props.opacity = 1.0;  // only opacity is changed
    SetClipPropertiesCommand command(id, props);

    ASSERT_TRUE(command.apply(project).isOk());
    ASSERT_DOUBLE_EQ(onlyClip(project).opacity, 1.0);

    ASSERT_TRUE(command.revert(project).isOk());
    EXPECT_DOUBLE_EQ(onlyClip(project).opacity, 0.3);
    EXPECT_DOUBLE_EQ(onlyClip(project).gain, 0.7);
}

TEST(SetClipProperties, ACallNamingNoPropertyIsAnAppliedNoOp) {
    // Refusing this would put the advertised schema and the handler in permanent disagreement,
    // because "at least one of these optional arguments" is a relation between arguments the
    // schema cannot state. SetTrackMutedCommand settled the same question the same way.
    const ClipId id = Uuid::generateV4();
    Project project = projectWithClip(id, 0.9, 1.1);

    SetClipPropertiesCommand command(id, Properties{});
    ASSERT_TRUE(command.apply(project).isOk());
    EXPECT_DOUBLE_EQ(onlyClip(project).opacity, 0.9);
    EXPECT_DOUBLE_EQ(onlyClip(project).gain, 1.1);

    ASSERT_TRUE(command.revert(project).isOk());
    EXPECT_DOUBLE_EQ(onlyClip(project).opacity, 0.9);
    EXPECT_DOUBLE_EQ(onlyClip(project).gain, 1.1);
}

TEST(SetClipProperties, AnUnknownClipIsReportedAsNotFoundAndChangesNothing) {
    const ClipId present = Uuid::generateV4();
    Project project = projectWithClip(present, 0.5, 0.5);

    Properties props;
    props.opacity = 0.1;
    SetClipPropertiesCommand command(Uuid::generateV4(), props);

    const Result<void> applied = command.apply(project);
    ASSERT_TRUE(applied.isError());
    EXPECT_EQ(applied.error().code(), ErrorCode::NotFound);
    EXPECT_DOUBLE_EQ(onlyClip(project).opacity, 0.5);
}

TEST(SetClipProperties, RevertBeforeASuccessfulApplyIsRefused) {
    const ClipId id = Uuid::generateV4();
    Project project = projectWithClip(id, 0.5, 0.5);

    Properties props;
    props.opacity = 0.2;
    SetClipPropertiesCommand command(id, props);

    const Result<void> reverted = command.revert(project);
    ASSERT_TRUE(reverted.isError());
    EXPECT_EQ(reverted.error().code(), ErrorCode::FailedPrecondition);
}

TEST(SetClipProperties, TheCommandReportsWhatItWasAskedToInstall) {
    const ClipId id = Uuid::generateV4();
    Properties props;
    props.gain = 1.5;
    const SetClipPropertiesCommand command(id, props);

    EXPECT_EQ(command.clipId(), id);
    EXPECT_EQ(command.name(), "SetClipProperties");
    ASSERT_TRUE(command.properties().gain.has_value());
    EXPECT_DOUBLE_EQ(*command.properties().gain, 1.5);
    EXPECT_FALSE(command.properties().opacity.has_value());
    EXPECT_TRUE(command.properties().any());
}

// --- fadeCoverage -----------------------------------------------------------
//
// The fade ramp lives in the core rather than in the compositor because the audio mix needs the
// identical curve: a cut that fades its picture but not its sound is a defect, and computing both
// from one function is the only way to guarantee they agree.

TEST(FadeCoverage, WithoutFadesACliIsFullyCoveredInsideAndAbsentOutside) {
    const Duration start = Duration::fromSeconds(1.0);
    const Duration end = Duration::fromSeconds(3.0);
    const Duration none = Duration::zero();

    EXPECT_DOUBLE_EQ(fadeCoverage(start, end, none, none, Duration::fromSeconds(2.0)), 1.0);
    EXPECT_DOUBLE_EQ(fadeCoverage(start, end, none, none, Duration::fromSeconds(1.0)), 1.0);
    EXPECT_DOUBLE_EQ(fadeCoverage(start, end, none, none, Duration::fromSeconds(0.5)), 0.0);
    EXPECT_DOUBLE_EQ(fadeCoverage(start, end, none, none, Duration::fromSeconds(3.0)), 0.0);
}

TEST(FadeCoverage, AFadeInRisesLinearlyFromTheLeadingEdge) {
    const Duration start = Duration::zero();
    const Duration end = Duration::fromSeconds(4.0);
    const Duration in = Duration::fromSeconds(2.0);

    EXPECT_NEAR(fadeCoverage(start, end, in, Duration::zero(), Duration::zero()), 0.0, 1e-9);
    EXPECT_NEAR(fadeCoverage(start, end, in, Duration::zero(), Duration::fromSeconds(0.5)), 0.25,
                1e-9);
    EXPECT_NEAR(fadeCoverage(start, end, in, Duration::zero(), Duration::fromSeconds(1.0)), 0.5,
                1e-9);
    EXPECT_NEAR(fadeCoverage(start, end, in, Duration::zero(), Duration::fromSeconds(3.0)), 1.0,
                1e-9);
}

TEST(FadeCoverage, AFadeOutFallsTowardsTheTrailingEdge) {
    const Duration start = Duration::zero();
    const Duration end = Duration::fromSeconds(4.0);
    const Duration out = Duration::fromSeconds(2.0);

    EXPECT_NEAR(fadeCoverage(start, end, Duration::zero(), out, Duration::fromSeconds(1.0)), 1.0,
                1e-9);
    EXPECT_NEAR(fadeCoverage(start, end, Duration::zero(), out, Duration::fromSeconds(3.0)), 0.5,
                1e-9);
    EXPECT_NEAR(fadeCoverage(start, end, Duration::zero(), out, Duration::fromSeconds(3.5)), 0.25,
                1e-9);
}

TEST(FadeCoverage, AFadeLongerThanTheClipIsClampedToHalfOfIt) {
    // Otherwise two overlapping ramps would multiply into a clip that never reaches full
    // coverage, which reads as a mistake rather than as a fade.
    const Duration start = Duration::zero();
    const Duration end = Duration::fromSeconds(2.0);
    const Duration huge = Duration::fromSeconds(10.0);

    // Each ramp is clamped to 1.0s, so the midpoint is where they meet: fully in, starting out.
    EXPECT_NEAR(fadeCoverage(start, end, huge, huge, Duration::fromSeconds(1.0)), 1.0, 1e-9);
    EXPECT_NEAR(fadeCoverage(start, end, huge, huge, Duration::fromSeconds(0.5)), 0.5, 1e-9);
    EXPECT_NEAR(fadeCoverage(start, end, huge, huge, Duration::fromSeconds(1.5)), 0.5, 1e-9);
}

TEST(FadeCoverage, AnEmptyClipCoversNothing) {
    const Duration at = Duration::fromSeconds(1.0);
    EXPECT_DOUBLE_EQ(fadeCoverage(at, at, Duration::zero(), Duration::zero(), at), 0.0);
}

// --- layoutTransforms -------------------------------------------------------
//
// A named arrangement is pure arithmetic over a count, so it is pinned here rather than inferred
// from a rendered frame.

TEST(LayoutTransforms, NoClipsYieldNoPlacements) {
    EXPECT_TRUE(layoutTransforms(LayoutKind::SideBySide, 0).empty());
    EXPECT_TRUE(layoutTransforms(LayoutKind::Grid, 0).empty());
}

TEST(LayoutTransforms, SideBySideSplitsTheWidthEvenlyAndCentresEachColumn) {
    const std::vector<Transform> two = layoutTransforms(LayoutKind::SideBySide, 2);
    ASSERT_EQ(two.size(), 2u);
    EXPECT_DOUBLE_EQ(two[0].scale, 0.5);
    EXPECT_DOUBLE_EQ(two[0].centerX, 0.25);
    EXPECT_DOUBLE_EQ(two[1].centerX, 0.75);
    EXPECT_DOUBLE_EQ(two[0].centerY, 0.5);

    const std::vector<Transform> three = layoutTransforms(LayoutKind::SideBySide, 3);
    ASSERT_EQ(three.size(), 3u);
    EXPECT_NEAR(three[1].scale, 1.0 / 3.0, 1e-9);
    EXPECT_NEAR(three[1].centerX, 0.5, 1e-9);
}

TEST(LayoutTransforms, SideBySideLeavesASingleClipFillingTheFrame) {
    // "Side by side" with one participant means nothing, and shrinking it to a full-width column
    // would be a strange reading of the request.
    const std::vector<Transform> one = layoutTransforms(LayoutKind::SideBySide, 1);
    ASSERT_EQ(one.size(), 1u);
    EXPECT_TRUE(one[0].isIdentity());
}

TEST(LayoutTransforms, PictureInPictureKeepsTheFirstClipFullFrame) {
    const std::vector<Transform> three = layoutTransforms(LayoutKind::PictureInPicture, 3);
    ASSERT_EQ(three.size(), 3u);
    EXPECT_TRUE(three[0].isIdentity());
    // The insets are small, and the first one sits in the top right.
    EXPECT_DOUBLE_EQ(three[1].scale, 0.25);
    EXPECT_GT(three[1].centerX, 0.5);
    EXPECT_LT(three[1].centerY, 0.5);
    // The second inset is in a different corner.
    EXPECT_NE(three[1].centerY, three[2].centerY);
}

TEST(LayoutTransforms, PictureInPictureReusesCornersRatherThanShrinkingIndefinitely) {
    // A fifth clip returns to the first corner: an unreadably small inset is worse than an
    // overlapping one, and the caller can place extras itself.
    const std::vector<Transform> six = layoutTransforms(LayoutKind::PictureInPicture, 6);
    ASSERT_EQ(six.size(), 6u);
    EXPECT_DOUBLE_EQ(six[1].centerX, six[5].centerX);
    EXPECT_DOUBLE_EQ(six[1].centerY, six[5].centerY);
    EXPECT_DOUBLE_EQ(six[5].scale, 0.25);
}

TEST(LayoutTransforms, AGridIsTheSquarestArrangementThatHoldsThemAll) {
    // Four clips make a 2x2: two distinct columns and two distinct rows, every cell the same size.
    const std::vector<Transform> four = layoutTransforms(LayoutKind::Grid, 4);
    ASSERT_EQ(four.size(), 4u);
    EXPECT_DOUBLE_EQ(four[0].scale, 0.5);
    EXPECT_DOUBLE_EQ(four[0].centerX, 0.25);
    EXPECT_DOUBLE_EQ(four[0].centerY, 0.25);
    EXPECT_DOUBLE_EQ(four[3].centerX, 0.75);
    EXPECT_DOUBLE_EQ(four[3].centerY, 0.75);

    // Five clips need three columns, so two rows of three with the last row half empty.
    const std::vector<Transform> five = layoutTransforms(LayoutKind::Grid, 5);
    ASSERT_EQ(five.size(), 5u);
    EXPECT_NEAR(five[0].centerX, 1.0 / 6.0, 1e-9);
    EXPECT_NEAR(five[4].centerY, 0.75, 1e-9);
}

TEST(LayoutTransforms, EveryGridCellSharesOneScaleSoNoClipIsLargerThanAnother) {
    const std::vector<Transform> five = layoutTransforms(LayoutKind::Grid, 5);
    for (const Transform& t : five) {
        EXPECT_DOUBLE_EQ(t.scale, five[0].scale);
    }
}

}  // namespace
}  // namespace palmier
