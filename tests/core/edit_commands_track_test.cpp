// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for the track, project-settings, transition and validation commands,
// split out of edit_commands_test.cpp. Same target, so the test names are unchanged.
//
// _Requirements: 2.2, 2.3, 2.4, 2.5, 2.6, 2.7, 3.3, 3.8, 3.10_

#include "core/EditCommands.hpp"

#include <gtest/gtest.h>

#include "edit_commands_test_support.hpp"

namespace palmier {
namespace {

using namespace palmier::testsupport::editcommands;  // NOLINT(google-build-using-namespace)

// --- Track-command fixtures ------------------------------------------------

Track makeTrack(TrackKind kind) {
    Track track;
    track.id = Uuid::generateV4();
    track.kind = kind;
    return track;
}

// An empty project with no tracks at all.
Project makeEmptyProject() {
    Project project;
    project.id = Uuid::generateV4();
    project.name = "tracks";
    return project;
}

std::vector<Uuid> trackIds(const Project& project) {
    std::vector<Uuid> ids;
    ids.reserve(project.tracks.size());
    for (const Track& track : project.tracks) {
        ids.push_back(track.id);
    }
    return ids;
}

// --- AddTrackCommand -------------------------------------------------------

TEST(AddTrackCommand, AppendsAfterTheLastTrackOfItsKind) {
    Project project = makeEmptyProject();
    project.tracks = {makeTrack(TrackKind::Video), makeTrack(TrackKind::Video),
                      makeTrack(TrackKind::Audio)};
    const std::vector<Uuid> before = trackIds(project);

    AddTrackCommand cmd(TrackKind::Video);
    ASSERT_TRUE(cmd.apply(project).isOk());

    // The new video track lands directly after the last existing video track,
    // i.e. at index 2, ahead of the audio track.
    ASSERT_EQ(project.tracks.size(), 4u);
    EXPECT_EQ(cmd.insertedIndex().value(), 2u);
    EXPECT_EQ(project.tracks[2].id, cmd.trackId());
    EXPECT_EQ(project.tracks[2].kind, TrackKind::Video);
    EXPECT_TRUE(project.tracks[2].clips.empty());

    // Every pre-existing track keeps its identity and relative order.
    EXPECT_EQ(project.tracks[0].id, before[0]);
    EXPECT_EQ(project.tracks[1].id, before[1]);
    EXPECT_EQ(project.tracks[3].id, before[2]);
}

TEST(AddTrackCommand, AppendsAtTheEndWhenNoTrackOfThatKindExists) {
    Project project = makeEmptyProject();
    project.tracks = {makeTrack(TrackKind::Video)};

    AddTrackCommand cmd(TrackKind::Audio);
    ASSERT_TRUE(cmd.apply(project).isOk());

    ASSERT_EQ(project.tracks.size(), 2u);
    EXPECT_EQ(project.tracks[1].id, cmd.trackId());
    EXPECT_EQ(project.tracks[1].kind, TrackKind::Audio);
}

TEST(AddTrackCommand, LeavesExistingClipsUntouched) {
    Uuid trackId;
    Project project = makeProjectWithOneTrack(trackId);
    const ClipId a = Uuid::generateV4();
    project.tracks[0].clips = {makeClip(a, ms(0), ms(0), ms(1000))};
    const Clip original = project.tracks[0].clips[0];

    AddTrackCommand cmd(TrackKind::Video);
    ASSERT_TRUE(cmd.apply(project).isOk());

    ASSERT_EQ(project.tracks[0].clips.size(), 1u);
    EXPECT_EQ(project.tracks[0].clips[0].id, original.id);
    EXPECT_EQ(project.tracks[0].clips[0].timelineStart, original.timelineStart);
    EXPECT_EQ(project.tracks[0].clips[0].sourceIn, original.sourceIn);
    EXPECT_EQ(project.tracks[0].clips[0].sourceOut, original.sourceOut);
}

TEST(AddTrackCommand, ReturnsAnIdentifierUniqueWithinTheProject) {
    Project project = makeEmptyProject();
    AddTrackCommand first(TrackKind::Video);
    AddTrackCommand second(TrackKind::Video);
    ASSERT_TRUE(first.apply(project).isOk());
    ASSERT_TRUE(second.apply(project).isOk());
    EXPECT_NE(first.trackId(), second.trackId());

    // A caller-supplied id that already exists is refused, project unchanged.
    AddTrackCommand duplicate(TrackKind::Video, first.trackId());
    const auto result = duplicate.apply(project);
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::FailedPrecondition);
    EXPECT_EQ(project.tracks.size(), 2u);
}

TEST(AddTrackCommand, RejectsThe65thTrackOfOneKindAndNamesTheKind) {
    Project project = makeEmptyProject();
    for (std::size_t i = 0; i < AddTrackCommand::kMaxTracksPerKind; ++i) {
        AddTrackCommand cmd(TrackKind::Audio);
        ASSERT_TRUE(cmd.apply(project).isOk()) << "at track " << i;
    }
    ASSERT_EQ(project.tracks.size(), AddTrackCommand::kMaxTracksPerKind);

    AddTrackCommand overflow(TrackKind::Audio);
    const auto result = overflow.apply(project);
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::OutOfRange);
    EXPECT_NE(result.error().message().find("audio"), std::string::npos);
    EXPECT_EQ(project.tracks.size(), AddTrackCommand::kMaxTracksPerKind);

    // The cap is per kind: a video track is still accepted.
    AddTrackCommand video(TrackKind::Video);
    ASSERT_TRUE(video.apply(project).isOk());
    EXPECT_EQ(project.tracks.size(), AddTrackCommand::kMaxTracksPerKind + 1);
}

TEST(AddTrackCommand, UndoRemovesAndRedoRestoresTheSameTrack) {
    Project project = makeEmptyProject();
    project.tracks = {makeTrack(TrackKind::Video), makeTrack(TrackKind::Audio)};
    const std::vector<Uuid> before = trackIds(project);
    TimelineEngine engine(project);

    auto cmd = std::make_unique<AddTrackCommand>(TrackKind::Video);
    const Uuid added = cmd->trackId();
    ASSERT_TRUE(engine.apply(std::move(cmd)).changed());
    ASSERT_EQ(engine.snapshot().tracks.size(), 3u);
    EXPECT_EQ(engine.snapshot().tracks[1].id, added);

    ASSERT_TRUE(engine.undo().changed());
    EXPECT_EQ(trackIds(engine.snapshot()), before);

    ASSERT_TRUE(engine.redo().changed());
    ASSERT_EQ(engine.snapshot().tracks.size(), 3u);
    EXPECT_EQ(engine.snapshot().tracks[1].id, added);
}

TEST(AddTrackCommand, RevertBeforeApplyFails) {
    Project project = makeEmptyProject();
    AddTrackCommand cmd(TrackKind::Video);
    const auto result = cmd.revert(project);
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::FailedPrecondition);
}

// --- RemoveTrackCommand ----------------------------------------------------

TEST(RemoveTrackCommand, RemovesTrackWithItsClipsPreservingRemainingOrder) {
    Project project = makeEmptyProject();
    project.tracks = {makeTrack(TrackKind::Video), makeTrack(TrackKind::Video),
                      makeTrack(TrackKind::Audio)};
    project.tracks[1].clips = {makeClip(Uuid::generateV4(), ms(0), ms(0), ms(500)),
                               makeClip(Uuid::generateV4(), ms(500), ms(0), ms(500))};
    const Uuid removedId = project.tracks[1].id;
    const Uuid firstId = project.tracks[0].id;
    const Uuid lastId = project.tracks[2].id;

    RemoveTrackCommand cmd(removedId);
    ASSERT_TRUE(cmd.apply(project).isOk());

    ASSERT_EQ(project.tracks.size(), 2u);
    EXPECT_EQ(project.tracks[0].id, firstId);
    EXPECT_EQ(project.tracks[1].id, lastId);
    EXPECT_EQ(cmd.removedClipCount().value(), 2u);
}

TEST(RemoveTrackCommand, RevertRestoresTheTrackAndItsClipsAtItsFormerIndex) {
    Project project = makeEmptyProject();
    project.tracks = {makeTrack(TrackKind::Video), makeTrack(TrackKind::Audio),
                      makeTrack(TrackKind::Audio)};
    const ClipId clipId = Uuid::generateV4();
    project.tracks[1].clips = {makeClip(clipId, ms(250), ms(100), ms(900))};
    project.tracks[1].muted = true;
    const Uuid removedId = project.tracks[1].id;
    const std::vector<Uuid> before = trackIds(project);

    RemoveTrackCommand cmd(removedId);
    ASSERT_TRUE(cmd.apply(project).isOk());
    ASSERT_TRUE(cmd.revert(project).isOk());

    EXPECT_EQ(trackIds(project), before);
    ASSERT_EQ(project.tracks[1].clips.size(), 1u);
    EXPECT_EQ(project.tracks[1].clips[0].id, clipId);
    EXPECT_EQ(project.tracks[1].clips[0].timelineStart, ms(250));
    EXPECT_EQ(project.tracks[1].clips[0].sourceIn, ms(100));
    EXPECT_EQ(project.tracks[1].clips[0].sourceOut, ms(900));
    EXPECT_TRUE(project.tracks[1].muted);
}

TEST(RemoveTrackCommand, UnknownTrackIsRejectedAndChangesNothing) {
    Project project = makeEmptyProject();
    project.tracks = {makeTrack(TrackKind::Video)};
    const std::vector<Uuid> before = trackIds(project);

    RemoveTrackCommand cmd(Uuid::generateV4());
    const auto result = cmd.apply(project);
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::NotFound);
    EXPECT_EQ(trackIds(project), before);
    EXPECT_FALSE(cmd.removedClipCount().has_value());
}

TEST(RemoveTrackCommand, IsUndoableThroughTheEngine) {
    Project project = makeEmptyProject();
    project.tracks = {makeTrack(TrackKind::Video), makeTrack(TrackKind::Audio)};
    const ClipId clipId = Uuid::generateV4();
    project.tracks[0].clips = {makeClip(clipId, ms(0), ms(0), ms(1000))};
    const std::vector<Uuid> before = trackIds(project);
    const Uuid target = project.tracks[0].id;
    TimelineEngine engine(project);

    ASSERT_TRUE(engine.apply(std::make_unique<RemoveTrackCommand>(target)).changed());
    EXPECT_EQ(engine.snapshot().tracks.size(), 1u);
    EXPECT_FALSE(engine.clip(clipId).has_value());

    ASSERT_TRUE(engine.undo().changed());
    EXPECT_EQ(trackIds(engine.snapshot()), before);
    EXPECT_TRUE(engine.clip(clipId).has_value());

    ASSERT_TRUE(engine.redo().changed());
    EXPECT_EQ(engine.snapshot().tracks.size(), 1u);
    EXPECT_FALSE(engine.clip(clipId).has_value());
}

// --- SetTrackMutedCommand (task 10.1) --------------------------------------
//
// The command behind `timeline.set_track_muted`, which the offline interpreter's
// "mute track N" / "unmute track N" phrases resolve to.

TEST(SetTrackMutedCommand, SetsTheFlagAndRevertRestoresThePriorValue) {
    Project project = makeEmptyProject();
    project.tracks = {makeTrack(TrackKind::Video), makeTrack(TrackKind::Audio)};
    const Uuid target = project.tracks[1].id;
    ASSERT_FALSE(project.tracks[1].muted);

    SetTrackMutedCommand cmd(target, true);
    ASSERT_TRUE(cmd.apply(project).isOk());
    EXPECT_TRUE(project.tracks[1].muted);
    EXPECT_FALSE(project.tracks[0].muted) << "no other track is touched";
    ASSERT_TRUE(cmd.priorMuted().has_value());
    EXPECT_FALSE(*cmd.priorMuted());

    ASSERT_TRUE(cmd.revert(project).isOk());
    EXPECT_FALSE(project.tracks[1].muted);
}

TEST(SetTrackMutedCommand, SettingTheValueItAlreadyHoldsIsAppliedAndRevertsToItself) {
    Project project = makeEmptyProject();
    project.tracks = {makeTrack(TrackKind::Audio)};
    project.tracks[0].muted = true;
    const Uuid target = project.tracks[0].id;

    SetTrackMutedCommand cmd(target, true);
    ASSERT_TRUE(cmd.apply(project).isOk());
    EXPECT_TRUE(project.tracks[0].muted);
    ASSERT_TRUE(cmd.revert(project).isOk());
    EXPECT_TRUE(project.tracks[0].muted);
}

TEST(SetTrackMutedCommand, UnknownTrackIsRejectedAndChangesNothing) {
    Project project = makeEmptyProject();
    project.tracks = {makeTrack(TrackKind::Video)};

    SetTrackMutedCommand cmd(Uuid::generateV4(), true);
    const auto result = cmd.apply(project);
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::NotFound);
    EXPECT_FALSE(project.tracks[0].muted);
    EXPECT_FALSE(cmd.priorMuted().has_value());
}

TEST(SetTrackMutedCommand, RevertBeforeApplyFails) {
    Project project = makeEmptyProject();
    project.tracks = {makeTrack(TrackKind::Video)};

    SetTrackMutedCommand cmd(project.tracks[0].id, true);
    const auto result = cmd.revert(project);
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::FailedPrecondition);
}

TEST(SetTrackMutedCommand, IsUndoableThroughTheEngine) {
    Project project = makeEmptyProject();
    project.tracks = {makeTrack(TrackKind::Video), makeTrack(TrackKind::Audio)};
    const Uuid target = project.tracks[1].id;
    TimelineEngine engine(project);

    ASSERT_TRUE(engine.apply(std::make_unique<SetTrackMutedCommand>(target, true)).changed());
    EXPECT_TRUE(engine.snapshot().tracks[1].muted);
    EXPECT_EQ(engine.undoDepth(), 1u);

    ASSERT_TRUE(engine.undo().changed());
    EXPECT_FALSE(engine.snapshot().tracks[1].muted);

    ASSERT_TRUE(engine.redo().changed());
    EXPECT_TRUE(engine.snapshot().tracks[1].muted);
}

// --- SetTransitionCommand --------------------------------------------------

TEST(SetTransitionCommand, SetsTheIncomingTransitionAndRevertRestoresAbsence) {
    Uuid trackId;
    Project project = makeProjectWithOneTrack(trackId);
    const ClipId a = Uuid::generateV4();
    project.tracks[0].clips = {makeClip(a, ms(0), ms(0), ms(1000))};
    ASSERT_FALSE(project.tracks[0].clips[0].transitionIn.has_value());

    const Transition transition(Uuid::generateV4(), TransitionKind::Wipe, ms(400));
    SetTransitionCommand cmd(a, transition);
    ASSERT_TRUE(cmd.apply(project).isOk());

    ASSERT_TRUE(project.tracks[0].clips[0].transitionIn.has_value());
    EXPECT_EQ(project.tracks[0].clips[0].transitionIn->id, transition.id);
    EXPECT_EQ(project.tracks[0].clips[0].transitionIn->kind, TransitionKind::Wipe);
    EXPECT_EQ(project.tracks[0].clips[0].transitionIn->duration, ms(400));
    EXPECT_EQ(cmd.transitionId(), transition.id);

    ASSERT_TRUE(cmd.revert(project).isOk());
    EXPECT_FALSE(project.tracks[0].clips[0].transitionIn.has_value());
}

TEST(SetTransitionCommand, RevertRestoresAPriorTransition) {
    Uuid trackId;
    Project project = makeProjectWithOneTrack(trackId);
    const ClipId a = Uuid::generateV4();
    Clip clip = makeClip(a, ms(0), ms(0), ms(1000));
    const Transition prior(Uuid::generateV4(), TransitionKind::Crossfade, ms(200));
    clip.transitionIn = prior;
    project.tracks[0].clips = {clip};

    SetTransitionCommand cmd(a, Transition(Uuid::generateV4(), TransitionKind::Slide, ms(600)));
    ASSERT_TRUE(cmd.apply(project).isOk());
    EXPECT_EQ(project.tracks[0].clips[0].transitionIn->kind, TransitionKind::Slide);

    ASSERT_TRUE(cmd.revert(project).isOk());
    ASSERT_TRUE(project.tracks[0].clips[0].transitionIn.has_value());
    EXPECT_EQ(project.tracks[0].clips[0].transitionIn->id, prior.id);
    EXPECT_EQ(project.tracks[0].clips[0].transitionIn->duration, ms(200));
}

TEST(SetTransitionCommand, MissingClipFailsAndRevertBeforeApplyFails) {
    Uuid trackId;
    Project project = makeProjectWithOneTrack(trackId);

    SetTransitionCommand cmd(Uuid::generateV4(),
                             Transition(Uuid::generateV4(), TransitionKind::Fade, ms(100)));
    const auto applied = cmd.apply(project);
    EXPECT_TRUE(applied.isError());
    EXPECT_EQ(applied.error().code(), ErrorCode::NotFound);

    const auto reverted = cmd.revert(project);
    EXPECT_TRUE(reverted.isError());
    EXPECT_EQ(reverted.error().code(), ErrorCode::FailedPrecondition);
}

TEST(SetTransitionCommand, IsUndoableThroughTheEngine) {
    Uuid trackId;
    Project project = makeProjectWithOneTrack(trackId);
    const ClipId a = Uuid::generateV4();
    project.tracks[0].clips = {makeClip(a, ms(0), ms(0), ms(1000))};
    TimelineEngine engine(project);

    const Transition transition(Uuid::generateV4(), TransitionKind::Crossfade, ms(300));
    ASSERT_TRUE(engine.apply(std::make_unique<SetTransitionCommand>(a, transition)).changed());
    ASSERT_TRUE(engine.clip(a)->transitionIn.has_value());
    EXPECT_EQ(engine.clip(a)->transitionIn->id, transition.id);

    ASSERT_TRUE(engine.undo().changed());
    EXPECT_FALSE(engine.clip(a)->transitionIn.has_value());

    ASSERT_TRUE(engine.redo().changed());
    EXPECT_EQ(engine.clip(a)->transitionIn->id, transition.id);
}

// ===========================================================================
// Mutable project settings (usable-editor task 10; Requirement 7)
// ===========================================================================

TEST(SetProjectSettingsCommand, ChangesAllThreeSettingsAndUndoesExactly) {
    Uuid trackId;
    Project project = makeProjectWithOneTrack(trackId);
    project.timelineFps = FrameRate::fps30();
    project.canvas = Resolution::hd1080();
    project.colorSpace = ColorSpace::Rec709;
    TimelineEngine engine(std::move(project));

    ASSERT_TRUE(engine
                    .apply(std::make_unique<SetProjectSettingsCommand>(
                        FrameRate::fps24(), Resolution::uhd4k(), ColorSpace::Rec2020))
                    .changed());

    const Project after = engine.snapshot();
    EXPECT_EQ(after.timelineFps, FrameRate::fps24());
    EXPECT_EQ(after.canvas, Resolution::uhd4k());
    EXPECT_EQ(after.colorSpace, ColorSpace::Rec2020);

    // Requirement 7.4: one Undo restores every field exactly.
    ASSERT_TRUE(engine.undo().changed());
    const Project restored = engine.snapshot();
    EXPECT_EQ(restored.timelineFps, FrameRate::fps30());
    EXPECT_EQ(restored.canvas, Resolution::hd1080());
    EXPECT_EQ(restored.colorSpace, ColorSpace::Rec709);
    EXPECT_FALSE(engine.canUndo());
}

TEST(SetProjectSettingsCommand, LeavesAnOmittedSettingUntouched) {
    Uuid trackId;
    Project project = makeProjectWithOneTrack(trackId);
    project.timelineFps = FrameRate::fps30();
    project.canvas = Resolution::hd1080();
    project.colorSpace = ColorSpace::Rec709;
    TimelineEngine engine(std::move(project));

    // Only the colour space is supplied; fps and canvas are std::nullopt.
    ASSERT_TRUE(engine
                    .apply(std::make_unique<SetProjectSettingsCommand>(std::nullopt, std::nullopt,
                                                                       ColorSpace::DisplayP3))
                    .changed());

    const Project after = engine.snapshot();
    EXPECT_EQ(after.timelineFps, FrameRate::fps30());  // untouched
    EXPECT_EQ(after.canvas, Resolution::hd1080());     // untouched
    EXPECT_EQ(after.colorSpace, ColorSpace::DisplayP3);
}

// Requirement 7.3: every clip's timeline position and source range survives a
// frame-rate change as a Duration — nothing is migrated, because Duration is an
// absolute nanosecond count with no embedded frame rate.
TEST(SetProjectSettingsCommand, FrameRateChangeLeavesEveryClipsDurationsExactlyAsTheyWere) {
    Uuid trackId;
    Project project = makeProjectWithOneTrack(trackId);
    project.timelineFps = FrameRate::fps30();
    const ClipId clipId = Uuid::generateV4();
    project.tracks[0].clips.push_back(makeClip(clipId, ms(1500), ms(200), ms(1700)));
    const Clip before = project.tracks[0].clips[0];
    TimelineEngine engine(std::move(project));

    ASSERT_TRUE(engine
                    .apply(std::make_unique<SetProjectSettingsCommand>(FrameRate::fps24(),
                                                                       std::nullopt, std::nullopt))
                    .changed());

    const Project after = engine.snapshot();
    ASSERT_EQ(after.timelineFps, FrameRate::fps24());
    ASSERT_EQ(after.tracks[0].clips.size(), 1u);
    const Clip& clip = after.tracks[0].clips[0];
    EXPECT_EQ(clip.timelineStart, before.timelineStart);
    EXPECT_EQ(clip.sourceIn, before.sourceIn);
    EXPECT_EQ(clip.sourceOut, before.sourceOut);
    EXPECT_EQ(clip.duration(), before.duration());
}

TEST(SetProjectSettingsCommand, AnInvalidFrameRateIsRefusedAndLeavesTheProjectUnchanged) {
    Uuid trackId;
    Project project = makeProjectWithOneTrack(trackId);
    project.timelineFps = FrameRate::fps30();
    TimelineEngine engine(std::move(project));

    const CommandResult result = engine.apply(std::make_unique<SetProjectSettingsCommand>(
        FrameRate(), std::nullopt, std::nullopt));  // default-constructed: 0/0, invalid
    EXPECT_FALSE(result.changed());
    EXPECT_EQ(engine.snapshot().timelineFps, FrameRate::fps30());
    EXPECT_FALSE(engine.canUndo());
}

// Project.assets — asset resolution — validation's own well-formedness rules.
TEST(ProjectValidation, ATextClipIsExemptFromAssetResolutionAndPassesWithNoAssetsAtAll) {
    Uuid trackId;
    Project project = makeProjectWithOneTextTrack(trackId);
    project.timelineFps = FrameRate::fps30();
    project.canvas = Resolution::hd1080();
    const ClipId clipId = Uuid::generateV4();
    project.tracks[0].clips.push_back(makeTextClip(clipId, ms(0), ms(1000), "Title"));

    ASSERT_TRUE(project.assets.empty());
    EXPECT_TRUE(validateProject(project).isOk());
}

TEST(ProjectValidation, RejectsATextStyledClipOnAVideoTrack) {
    Uuid trackId;
    Project project = makeProjectWithOneTrack(trackId);  // TrackKind::Video
    project.timelineFps = FrameRate::fps30();
    project.canvas = Resolution::hd1080();
    const ClipId clipId = Uuid::generateV4();
    project.tracks[0].clips.push_back(makeTextClip(clipId, ms(0), ms(1000), "Title"));

    const Result<void> result = validateProject(project);
    EXPECT_FALSE(result.isOk());
}

TEST(ProjectValidation, RejectsAMediaClipOnATextTrack) {
    Uuid trackId;
    Project project = makeProjectWithOneTextTrack(trackId);
    project.timelineFps = FrameRate::fps30();
    project.canvas = Resolution::hd1080();
    const ClipId clipId = Uuid::generateV4();
    MediaAssetRef asset(Uuid::generateV4(), "mem://asset");
    project.assets.push_back(asset);
    Clip clip = makeClip(clipId, ms(0), ms(0), ms(1000));
    clip.assetRef = asset;
    project.tracks[0].clips.push_back(std::move(clip));

    const Result<void> result = validateProject(project);
    EXPECT_FALSE(result.isOk());
}

TEST(ProjectValidation, RejectsATextClipWhoseStyleIsNotInternallyWellFormed) {
    Uuid trackId;
    Project project = makeProjectWithOneTextTrack(trackId);
    project.timelineFps = FrameRate::fps30();
    project.canvas = Resolution::hd1080();
    const ClipId clipId = Uuid::generateV4();
    Clip clip = makeTextClip(clipId, ms(0), ms(1000), "Title");
    clip.textStyle->colorR = 2.5;  // outside [0,1]
    project.tracks[0].clips.push_back(std::move(clip));

    const Result<void> result = validateProject(project);
    EXPECT_FALSE(result.isOk());
}

TEST(ProjectValidation, ACaptionCueIsExemptFromAssetResolutionAndPassesWithNoAssetsAtAll) {
    Uuid trackId;
    Project project = makeProjectWithOneCaptionTrack(trackId);
    project.timelineFps = FrameRate::fps30();
    project.canvas = Resolution::hd1080();
    const ClipId clipId = Uuid::generateV4();
    project.tracks[0].clips.push_back(makeCaptionCue(clipId, ms(0), ms(1000), "Hello"));

    ASSERT_TRUE(project.assets.empty());
    EXPECT_TRUE(validateProject(project).isOk());
}

TEST(ProjectValidation, RejectsACaptionCueOnAVideoTrack) {
    Uuid trackId;
    Project project = makeProjectWithOneTrack(trackId);  // TrackKind::Video
    project.timelineFps = FrameRate::fps30();
    project.canvas = Resolution::hd1080();
    const ClipId clipId = Uuid::generateV4();
    project.tracks[0].clips.push_back(makeCaptionCue(clipId, ms(0), ms(1000), "Hello"));

    const Result<void> result = validateProject(project);
    EXPECT_FALSE(result.isOk());
}

TEST(ProjectValidation, RejectsAMediaClipOnACaptionTrack) {
    Uuid trackId;
    Project project = makeProjectWithOneCaptionTrack(trackId);
    project.timelineFps = FrameRate::fps30();
    project.canvas = Resolution::hd1080();
    const ClipId clipId = Uuid::generateV4();
    MediaAssetRef asset(Uuid::generateV4(), "mem://asset");
    project.assets.push_back(asset);
    Clip clip = makeClip(clipId, ms(0), ms(0), ms(1000));
    clip.assetRef = asset;
    project.tracks[0].clips.push_back(std::move(clip));

    const Result<void> result = validateProject(project);
    EXPECT_FALSE(result.isOk());
}

TEST(ProjectValidation, RejectsACaptionCueWithEmptyText) {
    Uuid trackId;
    Project project = makeProjectWithOneCaptionTrack(trackId);
    project.timelineFps = FrameRate::fps30();
    project.canvas = Resolution::hd1080();
    const ClipId clipId = Uuid::generateV4();
    Clip clip = makeCaptionCue(clipId, ms(0), ms(1000), "Hello");
    clip.captionText = "";  // present but empty — must be rejected
    project.tracks[0].clips.push_back(std::move(clip));

    const Result<void> result = validateProject(project);
    EXPECT_FALSE(result.isOk());
}
}  // namespace
}  // namespace palmier
