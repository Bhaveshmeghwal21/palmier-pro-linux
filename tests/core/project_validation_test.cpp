// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for the project data-model validation rules (task 2.2).
//
// Exercises every rule declared in core/ProjectValidation.hpp with both valid
// and invalid inputs, and checks boundary values (design.md Data Models
// "Validation rules"):
//   Clip    — sourceOut > sourceIn; opacity in [0, 1]; gain >= 0.
//   Project — timelineFps > 0; positive canvas; supported schema version;
//             every Clip.assetRef resolves to an entry in Project.assets.
//
// Validates: Requirements 2.1, 3.5

#include <gtest/gtest.h>

#include "core/Types.hpp"

namespace palmier {
namespace {

// Builds a minimal, fully valid project: one asset and one video track holding a
// single 5-second clip that references that asset. Individual tests copy this and
// mutate exactly one field to isolate each rule.
Project makeValidProject() {
    const Uuid assetId = Uuid::generateV4();

    Clip clip;
    clip.id = Uuid::generateV4();
    clip.assetRef = MediaAssetRef{assetId, "/media/input.mp4"};
    clip.timelineStart = Duration::zero();
    clip.sourceIn = Duration::zero();
    clip.sourceOut = Duration::fromSeconds(5.0);
    clip.gain = 1.0;
    clip.opacity = 1.0;

    Track track;
    track.id = Uuid::generateV4();
    track.kind = TrackKind::Video;
    track.clips.push_back(clip);

    Project project;
    project.id = Uuid::generateV4();
    project.name = "Test Project";
    project.timelineFps = FrameRate::fps30();
    project.canvas = Resolution::hd1080();
    project.colorSpace = ColorSpace::Rec709;
    project.version = SchemaVersion::current();
    project.assets.push_back(MediaAssetRef{assetId, "/media/input.mp4"});
    project.tracks.push_back(track);

    return project;
}

// --- Whole-project happy path ---------------------------------------------

TEST(ProjectValidation, ValidProjectPasses) {
    EXPECT_TRUE(validateProject(makeValidProject()).isOk());
}

TEST(ProjectValidation, EmptyProjectWithNoTracksOrAssetsIsValid) {
    Project p = makeValidProject();
    p.tracks.clear();
    p.assets.clear();
    EXPECT_TRUE(validateProject(p).isOk());
}

// --- timelineFps > 0 -------------------------------------------------------

TEST(ProjectValidation, RejectsInvalidTimelineFps) {
    Project p = makeValidProject();
    p.timelineFps = FrameRate{}; // default-constructed rate is invalid (0/0)
    const auto r = validateProject(p);
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
}

TEST(ProjectValidation, RejectsZeroNumeratorFrameRate) {
    Project p = makeValidProject();
    p.timelineFps = FrameRate{0, 1};
    EXPECT_TRUE(validateProject(p).isError());
}

// --- positive canvas -------------------------------------------------------

TEST(ProjectValidation, RejectsZeroWidthCanvas) {
    Project p = makeValidProject();
    p.canvas = Resolution{0, 1080};
    const auto r = validateProject(p);
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
}

TEST(ProjectValidation, RejectsZeroHeightCanvas) {
    Project p = makeValidProject();
    p.canvas = Resolution{1920, 0};
    EXPECT_TRUE(validateProject(p).isError());
}

// --- known schema version --------------------------------------------------

TEST(ProjectValidation, RejectsUnsupportedSchemaVersion) {
    Project p = makeValidProject();
    p.version = SchemaVersion{SchemaVersion::current().major + 1, 0};
    const auto r = validateProject(p);
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(r.error().code(), ErrorCode::Unsupported);
}

// --- sourceOut > sourceIn --------------------------------------------------

TEST(ProjectValidation, RejectsSourceOutEqualToSourceIn) {
    Project p = makeValidProject();
    p.tracks[0].clips[0].sourceIn = Duration::fromSeconds(2.0);
    p.tracks[0].clips[0].sourceOut = Duration::fromSeconds(2.0);
    const auto r = validateProject(p);
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(r.error().code(), ErrorCode::OutOfRange);
}

TEST(ProjectValidation, RejectsSourceOutBeforeSourceIn) {
    Project p = makeValidProject();
    p.tracks[0].clips[0].sourceIn = Duration::fromSeconds(3.0);
    p.tracks[0].clips[0].sourceOut = Duration::fromSeconds(1.0);
    EXPECT_TRUE(validateProject(p).isError());
}

// --- opacity in [0, 1] -----------------------------------------------------

TEST(ProjectValidation, RejectsOpacityAboveOne) {
    Project p = makeValidProject();
    p.tracks[0].clips[0].opacity = 1.5;
    const auto r = validateProject(p);
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(r.error().code(), ErrorCode::OutOfRange);
}

TEST(ProjectValidation, RejectsNegativeOpacity) {
    Project p = makeValidProject();
    p.tracks[0].clips[0].opacity = -0.01;
    EXPECT_TRUE(validateProject(p).isError());
}

TEST(ProjectValidation, AcceptsOpacityBoundaryValues) {
    Project lo = makeValidProject();
    lo.tracks[0].clips[0].opacity = 0.0;
    EXPECT_TRUE(validateProject(lo).isOk());

    Project hi = makeValidProject();
    hi.tracks[0].clips[0].opacity = 1.0;
    EXPECT_TRUE(validateProject(hi).isOk());
}

// --- gain >= 0 -------------------------------------------------------------

TEST(ProjectValidation, RejectsNegativeGain) {
    Project p = makeValidProject();
    p.tracks[0].clips[0].gain = -1.0;
    const auto r = validateProject(p);
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(r.error().code(), ErrorCode::OutOfRange);
}

TEST(ProjectValidation, AcceptsZeroGain) {
    Project p = makeValidProject();
    p.tracks[0].clips[0].gain = 0.0;
    EXPECT_TRUE(validateProject(p).isOk());
}

// --- every Clip.assetRef resolves -----------------------------------------

TEST(ProjectValidation, RejectsUnresolvedAssetRef) {
    Project p = makeValidProject();
    p.tracks[0].clips[0].assetRef = MediaAssetRef{Uuid::generateV4(), "/media/ghost.mp4"};
    const auto r = validateProject(p);
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(r.error().code(), ErrorCode::NotFound);
}

TEST(ProjectValidation, RejectsClipWhenAssetTableIsEmpty) {
    Project p = makeValidProject();
    p.assets.clear();
    EXPECT_TRUE(validateProject(p).isError());
}

TEST(ProjectValidation, ResolvesAssetRefByIdentityIgnoringPath) {
    Project p = makeValidProject();
    // Same identity as the declared asset but a different informational path
    // still resolves, since resolution is by assetId.
    const Uuid id = p.assets[0].assetId;
    p.tracks[0].clips[0].assetRef = MediaAssetRef{id, "/some/other/path.mp4"};
    EXPECT_TRUE(validateProject(p).isOk());
}

// --- transitionIn ----------------------------------------------------------

TEST(ProjectValidation, AcceptsNonNegativeTransition) {
    Project p = makeValidProject();
    p.tracks[0].clips[0].transitionIn =
        Transition{Uuid::generateV4(), TransitionKind::Crossfade, Duration::fromSeconds(1.0)};
    EXPECT_TRUE(validateProject(p).isOk());
}

TEST(ProjectValidation, RejectsNegativeTransitionDuration) {
    Project p = makeValidProject();
    p.tracks[0].clips[0].transitionIn =
        Transition{Uuid::generateV4(), TransitionKind::Crossfade, Duration::fromSeconds(-1.0)};
    EXPECT_TRUE(validateProject(p).isError());
}

// --- validateClip / validateTrack standalone -------------------------------

TEST(ProjectValidation, ValidateClipStandaloneHappyPath) {
    Clip clip;
    clip.id = Uuid::generateV4();
    clip.sourceIn = Duration::zero();
    clip.sourceOut = Duration::fromSeconds(1.0);
    clip.gain = 1.0;
    clip.opacity = 0.5;
    EXPECT_TRUE(validateClip(clip).isOk());
}

TEST(ProjectValidation, ValidateTrackReportsInvalidClip) {
    Track track;
    track.id = Uuid::generateV4();
    Clip bad;
    bad.id = Uuid::generateV4();
    bad.sourceIn = Duration::fromSeconds(2.0);
    bad.sourceOut = Duration::fromSeconds(1.0); // sourceOut <= sourceIn
    track.clips.push_back(bad);
    EXPECT_TRUE(validateTrack(track).isError());
}

TEST(ProjectValidation, ClipDurationEqualsSourceRange) {
    Clip clip;
    clip.sourceIn = Duration::fromSeconds(1.0);
    clip.sourceOut = Duration::fromSeconds(4.0);
    EXPECT_EQ(clip.duration(), Duration::fromSeconds(3.0));
}

}  // namespace
}  // namespace palmier
