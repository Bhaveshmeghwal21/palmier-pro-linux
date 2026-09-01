// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for the concrete editing commands (task 3.3) — AddClipCommand,
// DeleteClipCommand, MoveClipCommand, TrimClipCommand, SplitClipCommand,
// ReorderClipsCommand, AddEffectCommand — plus the track and transition commands
// added by task 1.2: AddTrackCommand, RemoveTrackCommand and SetTransitionCommand
// (the last promoted out of services/ToolRegistry.cpp into the core).
//
// These exercise both the command semantics themselves and their apply()/revert()
// round-trip. Where an operation must respect a timeline invariant (overlap
// rejection on move/add) the command is driven through a TimelineEngine so the
// engine's invariant enforcement and atomic rollback are also covered; where the
// focus is the exact-inverse revert(), commands are applied directly to a Project.
//
// _Requirements: 2.2, 2.3, 2.4, 2.5, 2.6, 2.7, 3.3, 3.8, 3.10_

#include "core/EditCommands.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/Clip.hpp"
#include "core/ColorSpace.hpp"
#include "core/FrameRate.hpp"
#include "core/Project.hpp"
#include "core/ProjectValidation.hpp"
#include "core/Resolution.hpp"
#include "core/TextStyle.hpp"
#include "core/TimelineEngine.hpp"
#include "core/ToneCurve.hpp"
#include "core/Track.hpp"
#include "core/Transition.hpp"
#include "core/Uuid.hpp"

namespace palmier {
namespace {

// --- Fixtures --------------------------------------------------------------

Clip makeClip(ClipId id, Duration timelineStart, Duration sourceIn, Duration sourceOut) {
    Clip clip;
    clip.id = id;
    clip.assetRef = MediaAssetRef(Uuid::generateV4(), "mem://asset");
    clip.timelineStart = timelineStart;
    clip.sourceIn = sourceIn;
    clip.sourceOut = sourceOut;
    return clip;
}

// A project with a single empty video track, returning the track id.
Project makeProjectWithOneTrack(Uuid& trackIdOut) {
    Project project;
    project.id = Uuid::generateV4();
    project.name = "test";
    Track track;
    track.id = Uuid::generateV4();
    track.kind = TrackKind::Video;
    trackIdOut = track.id;
    project.tracks.push_back(std::move(track));
    return project;
}

// A project with a single empty TEXT track, returning the track id (usable-editor
// task 12; Requirement 9).
Project makeProjectWithOneTextTrack(Uuid& trackIdOut) {
    Project project;
    project.id = Uuid::generateV4();
    project.name = "test";
    Track track;
    track.id = Uuid::generateV4();
    track.kind = TrackKind::Text;
    trackIdOut = track.id;
    project.tracks.push_back(std::move(track));
    return project;
}

// A text clip: no assetRef (default-constructed, nil/"invalid" — exactly what
// AddClipCommand's own asset-registration step is written to skip), timing like
// any other clip, and a populated TextStyle.
Clip makeTextClip(ClipId id, Duration timelineStart, Duration duration,
                  std::string content) {
    Clip clip;
    clip.id = id;
    clip.timelineStart = timelineStart;
    clip.sourceIn = Duration::zero();
    clip.sourceOut = duration;
    TextStyle style;
    style.content = std::move(content);
    clip.textStyle = std::move(style);
    return clip;
}

// A project with a single, otherwise-empty CAPTION track (usable-editor task
// 13; Requirement 10), mirroring makeProjectWithOneTextTrack exactly.
Project makeProjectWithOneCaptionTrack(Uuid& trackIdOut) {
    Project project;
    project.id = Uuid::generateV4();
    project.name = "test";
    Track track;
    track.id = Uuid::generateV4();
    track.kind = TrackKind::Caption;
    trackIdOut = track.id;
    project.tracks.push_back(std::move(track));
    return project;
}

// A caption cue: no assetRef, timing like any other clip, and a plain
// captionText — no styling, mirroring makeTextClip exactly minus the style.
Clip makeCaptionCue(ClipId id, Duration timelineStart, Duration duration, std::string text) {
    Clip clip;
    clip.id = id;
    clip.timelineStart = timelineStart;
    clip.sourceIn = Duration::zero();
    clip.sourceOut = duration;
    clip.captionText = std::move(text);
    return clip;
}

constexpr Duration ms(std::int64_t v) { return Duration::fromMilliseconds(v); }

// --- AddClipCommand --------------------------------------------------------

TEST(AddClipCommand, InsertsInTimelineStartOrder) {
    Uuid trackId;
    TimelineEngine engine(makeProjectWithOneTrack(trackId));

    const ClipId late = Uuid::generateV4();
    const ClipId early = Uuid::generateV4();
    ASSERT_TRUE(engine.apply(std::make_unique<AddClipCommand>(
                                 trackId, makeClip(late, ms(2000), ms(0), ms(500))))
                    .changed());
    // Inserting an earlier clip must land before the later one.
    ASSERT_TRUE(engine.apply(std::make_unique<AddClipCommand>(
                                 trackId, makeClip(early, ms(0), ms(0), ms(500))))
                    .changed());

    const Project snap = engine.snapshot();
    ASSERT_EQ(snap.tracks[0].clips.size(), 2u);
    EXPECT_EQ(snap.tracks[0].clips[0].id, early);
    EXPECT_EQ(snap.tracks[0].clips[1].id, late);
}

TEST(AddClipCommand, OverlappingAddIsRejectedByEngine) {
    Uuid trackId;
    TimelineEngine engine(makeProjectWithOneTrack(trackId));
    ASSERT_TRUE(engine.apply(std::make_unique<AddClipCommand>(
                                 trackId, makeClip(Uuid::generateV4(), ms(0), ms(0), ms(1000))))
                    .changed());

    // Second clip overlaps [0,1000) with no transition -> rejected + rolled back.
    const auto result = engine.apply(std::make_unique<AddClipCommand>(
        trackId, makeClip(Uuid::generateV4(), ms(500), ms(0), ms(1000))));
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(engine.snapshot().tracks[0].clips.size(), 1u);
}

TEST(AddClipCommand, UndoRemovesAndRedoRestores) {
    Uuid trackId;
    TimelineEngine engine(makeProjectWithOneTrack(trackId));
    const ClipId id = Uuid::generateV4();
    ASSERT_TRUE(engine.apply(std::make_unique<AddClipCommand>(
                                 trackId, makeClip(id, ms(0), ms(0), ms(1000))))
                    .changed());

    ASSERT_TRUE(engine.undo().changed());
    EXPECT_FALSE(engine.clip(id).has_value());
    ASSERT_TRUE(engine.redo().changed());
    EXPECT_TRUE(engine.clip(id).has_value());
}

// The asset table is what a saved document carries and what validateProject
// resolves a clip's assetRef against, so placing a clip must register its asset
// there — otherwise a project saved after an import cannot be re-opened. Undo
// removes the entry the add created; redo puts it back.
TEST(AddClipCommand, RegistersTheClipsAssetAndUndoRemovesIt) {
    Uuid trackId;
    Project project = makeProjectWithOneTrack(trackId);
    project.timelineFps = FrameRate::fps30();
    project.canvas = Resolution(1920, 1080);
    TimelineEngine engine(std::move(project));

    const ClipId id = Uuid::generateV4();
    const Clip   clip = makeClip(id, ms(0), ms(0), ms(1000));
    ASSERT_TRUE(engine.apply(std::make_unique<AddClipCommand>(trackId, clip)).changed());

    Project snap = engine.snapshot();
    ASSERT_EQ(snap.assets.size(), 1u);
    EXPECT_EQ(snap.assets[0].assetId, clip.assetRef.assetId);
    EXPECT_EQ(snap.assets[0].sourcePath, clip.assetRef.sourcePath);
    // The clip's assetRef now resolves, which is what makes the project loadable.
    EXPECT_TRUE(validateProject(snap).isOk()) << validateProject(snap).error().toString();

    ASSERT_TRUE(engine.undo().changed());
    EXPECT_TRUE(engine.snapshot().assets.empty());

    ASSERT_TRUE(engine.redo().changed());
    snap = engine.snapshot();
    ASSERT_EQ(snap.assets.size(), 1u);
    EXPECT_EQ(snap.assets[0].assetId, clip.assetRef.assetId);
}

// Resolution is by assetId, so an asset the table already carries — whether from a
// previous add or from the loaded document — gets no second entry, and undoing the
// later add must not remove the entry it did not create.
TEST(AddClipCommand, DoesNotDuplicateAnAlreadyRegisteredAsset) {
    Uuid trackId;
    Project project = makeProjectWithOneTrack(trackId);
    const MediaAssetRef shared(Uuid::generateV4(), "mem://shared");
    TimelineEngine engine(std::move(project));

    Clip first = makeClip(Uuid::generateV4(), ms(0), ms(0), ms(500));
    first.assetRef = shared;
    Clip second = makeClip(Uuid::generateV4(), ms(1000), ms(0), ms(500));
    second.assetRef = shared;

    ASSERT_TRUE(engine.apply(std::make_unique<AddClipCommand>(trackId, first)).changed());
    ASSERT_TRUE(engine.apply(std::make_unique<AddClipCommand>(trackId, second)).changed());
    ASSERT_EQ(engine.snapshot().assets.size(), 1u);

    // Undoing the second add leaves the entry the FIRST add created in place.
    ASSERT_TRUE(engine.undo().changed());
    ASSERT_EQ(engine.snapshot().assets.size(), 1u);
    EXPECT_EQ(engine.snapshot().assets[0].assetId, shared.assetId);

    // Undoing the first removes it.
    ASSERT_TRUE(engine.undo().changed());
    EXPECT_TRUE(engine.snapshot().assets.empty());
}

// A rejected add is atomic down to the asset table: the engine's rollback must
// leave no orphaned asset entry behind.
TEST(AddClipCommand, RejectedAddLeavesTheAssetTableUnchanged) {
    Uuid trackId;
    TimelineEngine engine(makeProjectWithOneTrack(trackId));
    ASSERT_TRUE(engine.apply(std::make_unique<AddClipCommand>(
                                 trackId, makeClip(Uuid::generateV4(), ms(0), ms(0), ms(1000))))
                    .changed());
    ASSERT_EQ(engine.snapshot().assets.size(), 1u);

    // Overlaps [0,1000) with no transition -> rejected and rolled back.
    const auto overlapping = engine.apply(std::make_unique<AddClipCommand>(
        trackId, makeClip(Uuid::generateV4(), ms(500), ms(0), ms(1000))));
    ASSERT_TRUE(overlapping.isError());
    EXPECT_EQ(engine.snapshot().assets.size(), 1u);

    // An add naming a track that does not exist likewise registers nothing.
    const auto noTrack = engine.apply(std::make_unique<AddClipCommand>(
        Uuid::generateV4(), makeClip(Uuid::generateV4(), ms(5000), ms(0), ms(500))));
    ASSERT_TRUE(noTrack.isError());
    EXPECT_EQ(engine.snapshot().assets.size(), 1u);
}

// The nil identity cannot be catalogued when a document's media library is
// rebuilt, so it is never added to the table; the clip is still placed.
TEST(AddClipCommand, NilAssetRefIsNotRegistered) {
    Uuid trackId;
    TimelineEngine engine(makeProjectWithOneTrack(trackId));

    Clip clip = makeClip(Uuid::generateV4(), ms(0), ms(0), ms(500));
    clip.assetRef = MediaAssetRef{};
    ASSERT_TRUE(engine.apply(std::make_unique<AddClipCommand>(trackId, clip)).changed());

    EXPECT_TRUE(engine.snapshot().assets.empty());
    EXPECT_EQ(engine.snapshot().tracks[0].clips.size(), 1u);
}

// --- DeleteClipCommand -----------------------------------------------------

TEST(DeleteClipCommand, RemovesAndRevertReinsertsAtSameIndex) {
    Uuid trackId;
    Project project = makeProjectWithOneTrack(trackId);
    const ClipId a = Uuid::generateV4();
    const ClipId b = Uuid::generateV4();
    const ClipId c = Uuid::generateV4();
    project.tracks[0].clips = {makeClip(a, ms(0), ms(0), ms(500)),
                               makeClip(b, ms(500), ms(0), ms(500)),
                               makeClip(c, ms(1000), ms(0), ms(500))};

    DeleteClipCommand cmd(b);
    ASSERT_TRUE(cmd.apply(project).isOk());
    ASSERT_EQ(project.tracks[0].clips.size(), 2u);
    EXPECT_EQ(project.tracks[0].clips[0].id, a);
    EXPECT_EQ(project.tracks[0].clips[1].id, c);

    ASSERT_TRUE(cmd.revert(project).isOk());
    ASSERT_EQ(project.tracks[0].clips.size(), 3u);
    EXPECT_EQ(project.tracks[0].clips[1].id, b);  // restored at its former index
}

TEST(DeleteClipCommand, MissingClipFails) {
    Uuid trackId;
    Project project = makeProjectWithOneTrack(trackId);
    DeleteClipCommand cmd(Uuid::generateV4());
    const auto result = cmd.apply(project);
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::NotFound);
}

// --- MoveClipCommand -------------------------------------------------------

TEST(MoveClipCommand, MovesToValidPositionAndReorders) {
    Uuid trackId;
    TimelineEngine engine(makeProjectWithOneTrack(trackId));
    const ClipId a = Uuid::generateV4();
    const ClipId b = Uuid::generateV4();
    ASSERT_TRUE(engine.apply(std::make_unique<AddClipCommand>(
                                 trackId, makeClip(a, ms(0), ms(0), ms(500)))).changed());
    ASSERT_TRUE(engine.apply(std::make_unique<AddClipCommand>(
                                 trackId, makeClip(b, ms(500), ms(0), ms(500)))).changed());

    // Move a to 2000ms: it now sorts after b.
    ASSERT_TRUE(engine.apply(std::make_unique<MoveClipCommand>(a, ms(2000))).changed());
    const Project snap = engine.snapshot();
    EXPECT_EQ(snap.tracks[0].clips[0].id, b);
    EXPECT_EQ(snap.tracks[0].clips[1].id, a);
    EXPECT_EQ(snap.tracks[0].clips[1].timelineStart, ms(2000));
}

TEST(MoveClipCommand, OverlappingDropIsRejectedAndPositionRetained) {
    Uuid trackId;
    TimelineEngine engine(makeProjectWithOneTrack(trackId));
    const ClipId a = Uuid::generateV4();
    const ClipId b = Uuid::generateV4();
    ASSERT_TRUE(engine.apply(std::make_unique<AddClipCommand>(
                                 trackId, makeClip(a, ms(0), ms(0), ms(1000)))).changed());
    ASSERT_TRUE(engine.apply(std::make_unique<AddClipCommand>(
                                 trackId, makeClip(b, ms(2000), ms(0), ms(500)))).changed());

    // Drop b at 500ms overlaps a's [0,1000) -> rejected, b retained at 2000ms.
    const auto result = engine.apply(std::make_unique<MoveClipCommand>(b, ms(500)));
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::FailedPrecondition);
    const auto moved = engine.clip(b);
    ASSERT_TRUE(moved.has_value());
    EXPECT_EQ(moved->timelineStart, ms(2000));
}

TEST(MoveClipCommand, NegativeDestinationRejected) {
    Uuid trackId;
    Project project = makeProjectWithOneTrack(trackId);
    const ClipId a = Uuid::generateV4();
    project.tracks[0].clips = {makeClip(a, ms(1000), ms(0), ms(500))};

    MoveClipCommand cmd(a, Duration::fromMilliseconds(-100));
    const auto result = cmd.apply(project);
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(project.tracks[0].clips[0].timelineStart, ms(1000));  // unchanged
}

TEST(MoveClipCommand, RevertRestoresPriorOrderAndPositions) {
    Uuid trackId;
    Project project = makeProjectWithOneTrack(trackId);
    const ClipId a = Uuid::generateV4();
    const ClipId b = Uuid::generateV4();
    project.tracks[0].clips = {makeClip(a, ms(0), ms(0), ms(500)),
                               makeClip(b, ms(500), ms(0), ms(500))};

    MoveClipCommand cmd(a, ms(2000));
    ASSERT_TRUE(cmd.apply(project).isOk());
    ASSERT_TRUE(cmd.revert(project).isOk());
    EXPECT_EQ(project.tracks[0].clips[0].id, a);
    EXPECT_EQ(project.tracks[0].clips[0].timelineStart, ms(0));
    EXPECT_EQ(project.tracks[0].clips[1].id, b);
}

// --- TrimClipCommand -------------------------------------------------------

TEST(TrimClipCommand, TrimEndSetsOutPoint) {
    Uuid trackId;
    Project project = makeProjectWithOneTrack(trackId);
    const ClipId a = Uuid::generateV4();
    project.tracks[0].clips = {makeClip(a, ms(0), ms(0), ms(1000))};

    TrimClipCommand cmd(a, TrimClipCommand::Edge::End, ms(600),
                        FrameRate::fps24(), ms(2000));
    ASSERT_TRUE(cmd.apply(project).isOk());
    const Clip& clip = project.tracks[0].clips[0];
    EXPECT_EQ(clip.sourceOut, ms(600));
    EXPECT_EQ(clip.duration(), ms(600));
    EXPECT_EQ(clip.timelineStart, ms(0));  // start edge unchanged
}

TEST(TrimClipCommand, TrimEndClampedToSourceDuration) {
    Uuid trackId;
    Project project = makeProjectWithOneTrack(trackId);
    const ClipId a = Uuid::generateV4();
    project.tracks[0].clips = {makeClip(a, ms(0), ms(0), ms(1000))};

    // Request an out-point beyond the source: clamp to sourceDuration (2000ms).
    TrimClipCommand cmd(a, TrimClipCommand::Edge::End, ms(9000),
                        FrameRate::fps24(), ms(2000));
    ASSERT_TRUE(cmd.apply(project).isOk());
    EXPECT_EQ(project.tracks[0].clips[0].sourceOut, ms(2000));
}

TEST(TrimClipCommand, TrimEndClampedToOneFrameMinimum) {
    Uuid trackId;
    Project project = makeProjectWithOneTrack(trackId);
    const ClipId a = Uuid::generateV4();
    project.tracks[0].clips = {makeClip(a, ms(0), ms(0), ms(1000))};

    // Request an out-point below sourceIn: clamp so duration is exactly one frame.
    TrimClipCommand cmd(a, TrimClipCommand::Edge::End, Duration::zero(),
                        FrameRate::fps24(), ms(2000));
    ASSERT_TRUE(cmd.apply(project).isOk());
    EXPECT_EQ(project.tracks[0].clips[0].duration(), FrameRate::fps24().frameDuration());
}

TEST(TrimClipCommand, TrimStartShiftsInPointAndTimeline) {
    Uuid trackId;
    Project project = makeProjectWithOneTrack(trackId);
    const ClipId a = Uuid::generateV4();
    project.tracks[0].clips = {makeClip(a, ms(1000), ms(0), ms(1000))};

    // New in-point at 300ms shifts timelineStart by +300ms.
    TrimClipCommand cmd(a, TrimClipCommand::Edge::Start, ms(300),
                        FrameRate::fps24(), ms(2000));
    ASSERT_TRUE(cmd.apply(project).isOk());
    const Clip& clip = project.tracks[0].clips[0];
    EXPECT_EQ(clip.sourceIn, ms(300));
    EXPECT_EQ(clip.timelineStart, ms(1300));
    EXPECT_EQ(clip.duration(), ms(700));

    ASSERT_TRUE(cmd.revert(project).isOk());
    const Clip& restored = project.tracks[0].clips[0];
    EXPECT_EQ(restored.sourceIn, ms(0));
    EXPECT_EQ(restored.timelineStart, ms(1000));
    EXPECT_EQ(restored.sourceOut, ms(1000));
}

// --- SplitClipCommand ------------------------------------------------------

TEST(SplitClipCommand, InteriorSplitProducesTwoContiguousClips) {
    Uuid trackId;
    Project project = makeProjectWithOneTrack(trackId);
    const ClipId a = Uuid::generateV4();
    project.tracks[0].clips = {makeClip(a, ms(0), ms(0), ms(1000))};

    SplitClipCommand cmd(a, ms(400));
    ASSERT_TRUE(cmd.apply(project).isOk());

    const auto& clips = project.tracks[0].clips;
    ASSERT_EQ(clips.size(), 2u);
    const Clip& left = clips[0];
    const Clip& right = clips[1];

    EXPECT_EQ(left.id, a);
    EXPECT_EQ(left.timelineStart, ms(0));
    EXPECT_EQ(left.duration(), ms(400));
    // Right half begins exactly where the left ends (contiguous).
    EXPECT_EQ(right.timelineStart, left.timelineEnd());
    EXPECT_EQ(right.timelineStart, ms(400));
    // Combined duration equals the original.
    EXPECT_EQ(left.duration() + right.duration(), ms(1000));
    // Combined source range equals the original.
    EXPECT_EQ(left.sourceIn, ms(0));
    EXPECT_EQ(right.sourceOut, ms(1000));
    EXPECT_EQ(left.sourceOut, right.sourceIn);

    ASSERT_TRUE(cmd.revert(project).isOk());
    ASSERT_EQ(project.tracks[0].clips.size(), 1u);
    EXPECT_EQ(project.tracks[0].clips[0].id, a);
    EXPECT_EQ(project.tracks[0].clips[0].duration(), ms(1000));
}

TEST(SplitClipCommand, PlayheadOutsideClipFailsWithoutMutation) {
    Uuid trackId;
    Project project = makeProjectWithOneTrack(trackId);
    const ClipId a = Uuid::generateV4();
    project.tracks[0].clips = {makeClip(a, ms(1000), ms(0), ms(1000))};  // [1000,2000)

    // At the boundary and outside are both no-splits.
    SplitClipCommand atStart(a, ms(1000));
    EXPECT_TRUE(atStart.apply(project).isError());
    SplitClipCommand before(a, ms(200));
    EXPECT_TRUE(before.apply(project).isError());
    SplitClipCommand after(a, ms(5000));
    EXPECT_TRUE(after.apply(project).isError());

    // The clip is untouched.
    ASSERT_EQ(project.tracks[0].clips.size(), 1u);
    EXPECT_EQ(project.tracks[0].clips[0].id, a);
    EXPECT_EQ(project.tracks[0].clips[0].duration(), ms(1000));
}

// A track may legally carry an overlap: an incoming clip may start before its
// predecessor ends, by up to its own transition region. Splitting the
// predecessor at a playhead that lies PAST the incoming clip's start would leave
// the track unordered by timelineStart, because the right half is inserted
// immediately after the left one. The engine rejects that split and rolls back,
// so the track never becomes unordered; the same split before the incoming
// clip's start is accepted.
TEST(SplitClipCommand, SplitPastAnOverlappingSuccessorIsRejectedAndRolledBack) {
    Uuid trackId;
    Project project = makeProjectWithOneTrack(trackId);
    const ClipId a = Uuid::generateV4();
    const ClipId b = Uuid::generateV4();
    Clip incoming = makeClip(b, ms(400), ms(0), ms(1000));  // starts inside a
    Transition transition;
    transition.id = Uuid::generateV4();
    transition.kind = TransitionKind::Crossfade;
    transition.duration = ms(600);  // exactly the 600 ms overlap it introduces
    incoming.transitionIn = transition;
    project.tracks[0].clips = {makeClip(a, ms(0), ms(0), ms(1000)), std::move(incoming)};

    TimelineEngine engine(std::move(project));

    // Playhead at 500 ms is inside clip a but past the successor's start.
    const auto rejected = engine.apply(std::make_unique<SplitClipCommand>(a, ms(500)));
    EXPECT_TRUE(rejected.isError());
    EXPECT_FALSE(rejected.changed());
    {
        const Project snap = engine.snapshot();
        ASSERT_EQ(snap.tracks[0].clips.size(), 2u);
        EXPECT_EQ(snap.tracks[0].clips[0].id, a);
        EXPECT_EQ(snap.tracks[0].clips[0].duration(), ms(1000));
        EXPECT_EQ(snap.tracks[0].clips[1].id, b);
    }
    EXPECT_FALSE(engine.canUndo());

    // The same clip splits cleanly before the successor's start.
    const auto accepted = engine.apply(std::make_unique<SplitClipCommand>(a, ms(300)));
    EXPECT_TRUE(accepted.changed());
    const Project snap = engine.snapshot();
    ASSERT_EQ(snap.tracks[0].clips.size(), 3u);
    EXPECT_EQ(snap.tracks[0].clips[0].duration(), ms(300));
    EXPECT_EQ(snap.tracks[0].clips[1].timelineStart, ms(300));
    EXPECT_EQ(snap.tracks[0].clips[2].id, b);
}

TEST(SplitClipCommand, RedoReproducesSameRightId) {
    Uuid trackId;
    TimelineEngine engine(makeProjectWithOneTrack(trackId));
    const ClipId a = Uuid::generateV4();
    ASSERT_TRUE(engine.apply(std::make_unique<AddClipCommand>(
                                 trackId, makeClip(a, ms(0), ms(0), ms(1000)))).changed());

    auto split = std::make_unique<SplitClipCommand>(a, ms(400));
    SplitClipCommand* raw = split.get();
    ASSERT_TRUE(engine.apply(std::move(split)).changed());
    const auto rightId = raw->rightClipId();
    ASSERT_TRUE(rightId.has_value());

    ASSERT_TRUE(engine.undo().changed());
    EXPECT_FALSE(engine.clip(*rightId).has_value());
    ASSERT_TRUE(engine.redo().changed());
    // The same right-half id reappears after redo (deterministic re-apply).
    EXPECT_TRUE(engine.clip(*rightId).has_value());
}

// --- ReorderClipsCommand ---------------------------------------------------

TEST(ReorderClipsCommand, ReordersAndPreservesCount) {
    Uuid trackId;
    Project project = makeProjectWithOneTrack(trackId);
    const ClipId a = Uuid::generateV4();
    const ClipId b = Uuid::generateV4();
    const ClipId c = Uuid::generateV4();
    project.tracks[0].clips = {makeClip(a, ms(0), ms(0), ms(500)),
                               makeClip(b, ms(500), ms(0), ms(300)),
                               makeClip(c, ms(800), ms(0), ms(200))};

    ReorderClipsCommand cmd(trackId, {c, a, b});
    ASSERT_TRUE(cmd.apply(project).isOk());

    const auto& clips = project.tracks[0].clips;
    ASSERT_EQ(clips.size(), 3u);  // count preserved (Requirement 2.7)
    EXPECT_EQ(clips[0].id, c);
    EXPECT_EQ(clips[1].id, a);
    EXPECT_EQ(clips[2].id, b);
    // Repacked contiguously from the earliest start (0): c(200) a(500) b(300).
    EXPECT_EQ(clips[0].timelineStart, ms(0));
    EXPECT_EQ(clips[1].timelineStart, ms(200));
    EXPECT_EQ(clips[2].timelineStart, ms(700));

    ASSERT_TRUE(cmd.revert(project).isOk());
    const auto& restored = project.tracks[0].clips;
    EXPECT_EQ(restored[0].id, a);
    EXPECT_EQ(restored[1].id, b);
    EXPECT_EQ(restored[2].id, c);
    EXPECT_EQ(restored[1].timelineStart, ms(500));
}

TEST(ReorderClipsCommand, NonPermutationRejected) {
    Uuid trackId;
    Project project = makeProjectWithOneTrack(trackId);
    const ClipId a = Uuid::generateV4();
    const ClipId b = Uuid::generateV4();
    project.tracks[0].clips = {makeClip(a, ms(0), ms(0), ms(500)),
                               makeClip(b, ms(500), ms(0), ms(500))};

    // Contains an unknown id -> not a permutation.
    ReorderClipsCommand cmd(trackId, {a, Uuid::generateV4()});
    const auto result = cmd.apply(project);
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    // Track unchanged.
    EXPECT_EQ(project.tracks[0].clips[0].id, a);
    EXPECT_EQ(project.tracks[0].clips[1].id, b);
}

// --- EditCurvePointCommand -------------------------------------------------
//
// Requirement 5.7: add, move and remove are each ONE undoable edit. The interesting
// claim is not that each mutates correctly but that each has an exact inverse, which
// for removal means restoring the NUMBERING and not merely the coordinates.

namespace {

/// A project holding one clip with one tone-curve effect, and the ids to address it.
Project makeProjectWithCurveEffect(ClipId& clipOut, Uuid& effectOut) {
    Uuid trackId;
    Project project = makeProjectWithOneTrack(trackId);
    clipOut = Uuid::generateV4();
    Effect effect;
    effect.id = Uuid::generateV4();
    effect.type = EffectType::ToneCurve;
    effectOut = effect.id;
    Clip clip = makeClip(clipOut, ms(0), ms(0), ms(1000));
    clip.effects = {effect};
    project.tracks[0].clips = {clip};
    return project;
}

const std::map<std::string, double>& curveParams(const Project& project) {
    return project.tracks[0].clips[0].effects[0].parameters;
}

/// Add points p0..pN-1 to one channel, each through its own command, so the fixture is
/// built by the code under test rather than by hand-written parameter names.
void addPoints(Project& project, ClipId clipId, const Uuid& effectId, CurveChannel channel,
               const std::vector<CurvePoint>& points) {
    for (const CurvePoint& p : points) {
        EditCurvePointCommand cmd(clipId, effectId, channel,
                                  EditCurvePointCommand::Operation::Add, 0, p.x, p.y);
        ASSERT_TRUE(cmd.apply(project).isOk());
    }
}

}  // namespace

TEST(EditCurvePointCommand, AddingAPointIsOneEditAndRevertRemovesBothCoordinates) {
    ClipId clipId;
    Uuid   effectId;
    Project project = makeProjectWithCurveEffect(clipId, effectId);

    EditCurvePointCommand cmd(clipId, effectId, CurveChannel::Master,
                              EditCurvePointCommand::Operation::Add, 0, 0.25, 0.75);
    ASSERT_TRUE(cmd.apply(project).isOk());

    const std::vector<CurvePoint> after = curvePoints(curveParams(project), CurveChannel::Master);
    ASSERT_EQ(after.size(), 1u);
    EXPECT_DOUBLE_EQ(after[0].x, 0.25);
    EXPECT_DOUBLE_EQ(after[0].y, 0.75);

    // The whole point of a dedicated command: one revert removes BOTH coordinates. Two
    // set_effect_parameter edits would leave an X with no Y behind, which curvePoints
    // refuses to read as a point -- so the curve would change shape after a single undo.
    ASSERT_TRUE(cmd.revert(project).isOk());
    EXPECT_TRUE(curvePoints(curveParams(project), CurveChannel::Master).empty());
    EXPECT_TRUE(curveParams(project).empty()) << "revert must leave no orphaned coordinate";
}

TEST(EditCurvePointCommand, MovingAPointReplacesBothCoordinatesAndRevertRestoresThem) {
    ClipId clipId;
    Uuid   effectId;
    Project project = makeProjectWithCurveEffect(clipId, effectId);
    addPoints(project, clipId, effectId, CurveChannel::Red, {{0.2, 0.3}, {0.8, 0.9}});

    EditCurvePointCommand cmd(clipId, effectId, CurveChannel::Red,
                              EditCurvePointCommand::Operation::Move, 0, 0.1, 0.15);
    ASSERT_TRUE(cmd.apply(project).isOk());

    std::vector<CurvePoint> after = curvePoints(curveParams(project), CurveChannel::Red);
    ASSERT_EQ(after.size(), 2u);
    EXPECT_DOUBLE_EQ(after[0].x, 0.1);
    EXPECT_DOUBLE_EQ(after[0].y, 0.15);
    EXPECT_DOUBLE_EQ(after[1].x, 0.8) << "the other point must not move";

    ASSERT_TRUE(cmd.revert(project).isOk());
    after = curvePoints(curveParams(project), CurveChannel::Red);
    ASSERT_EQ(after.size(), 2u);
    EXPECT_DOUBLE_EQ(after[0].x, 0.2);
    EXPECT_DOUBLE_EQ(after[0].y, 0.3);
}

// The case the whole-channel capture exists for. Removing a middle point renumbers
// every point after it, so the inverse of "remove p1" is not "add a point at p1" --
// after the removal there IS no p1 with the old p2's coordinates to put back.
TEST(EditCurvePointCommand, RemovingAMiddlePointRenumbersAndRevertRestoresTheNumbering) {
    ClipId clipId;
    Uuid   effectId;
    Project project = makeProjectWithCurveEffect(clipId, effectId);
    addPoints(project, clipId, effectId, CurveChannel::Master,
              {{0.0, 0.0}, {0.5, 0.9}, {1.0, 1.0}});

    EditCurvePointCommand cmd(clipId, effectId, CurveChannel::Master,
                              EditCurvePointCommand::Operation::Remove, 1, 0.0, 0.0);
    ASSERT_TRUE(cmd.apply(project).isOk());

    std::vector<CurvePoint> after = curvePoints(curveParams(project), CurveChannel::Master);
    ASSERT_EQ(after.size(), 2u) << "a gap would truncate the run and lose the last point";
    EXPECT_DOUBLE_EQ(after[1].x, 1.0) << "p2 must become p1, contiguously";
    // The old p2's parameter names must be GONE, not left behind as a duplicate tail.
    EXPECT_EQ(curveParams(project).count(
                  curvePointParameterName(CurveChannel::Master, 2, /*isY=*/false)),
              0u);

    ASSERT_TRUE(cmd.revert(project).isOk());
    after = curvePoints(curveParams(project), CurveChannel::Master);
    ASSERT_EQ(after.size(), 3u);
    EXPECT_DOUBLE_EQ(after[1].x, 0.5);
    EXPECT_DOUBLE_EQ(after[1].y, 0.9);
    EXPECT_DOUBLE_EQ(after[2].x, 1.0);
}

TEST(EditCurvePointCommand, AnEditToOneChannelLeavesTheOthersAlone) {
    ClipId clipId;
    Uuid   effectId;
    Project project = makeProjectWithCurveEffect(clipId, effectId);
    addPoints(project, clipId, effectId, CurveChannel::Master, {{0.5, 0.5}});
    addPoints(project, clipId, effectId, CurveChannel::Blue, {{0.25, 0.4}, {0.75, 0.6}});

    EditCurvePointCommand cmd(clipId, effectId, CurveChannel::Master,
                              EditCurvePointCommand::Operation::Remove, 0, 0.0, 0.0);
    ASSERT_TRUE(cmd.apply(project).isOk());
    EXPECT_TRUE(curvePoints(curveParams(project), CurveChannel::Master).empty());
    EXPECT_EQ(curvePoints(curveParams(project), CurveChannel::Blue).size(), 2u);

    // And the inverse is equally narrow: restoring master must not resurrect or disturb
    // blue. A capture keyed on the wrong prefix would pass the line above and fail here.
    ASSERT_TRUE(cmd.revert(project).isOk());
    EXPECT_EQ(curvePoints(curveParams(project), CurveChannel::Master).size(), 1u);
    const std::vector<CurvePoint> blue = curvePoints(curveParams(project), CurveChannel::Blue);
    ASSERT_EQ(blue.size(), 2u);
    EXPECT_DOUBLE_EQ(blue[1].y, 0.6);
}

TEST(EditCurvePointCommand, AnEditLeavesOtherParametersOnTheSameEffectUntouched) {
    ClipId clipId;
    Uuid   effectId;
    Project project = makeProjectWithCurveEffect(clipId, effectId);
    project.tracks[0].clips[0].effects[0].parameters["amount"] = 0.42;
    addPoints(project, clipId, effectId, CurveChannel::Green, {{0.3, 0.7}});

    EditCurvePointCommand cmd(clipId, effectId, CurveChannel::Green,
                              EditCurvePointCommand::Operation::Add, 0, 0.6, 0.2);
    ASSERT_TRUE(cmd.apply(project).isOk());
    EXPECT_DOUBLE_EQ(curveParams(project).at("amount"), 0.42);
    ASSERT_TRUE(cmd.revert(project).isOk());
    EXPECT_DOUBLE_EQ(curveParams(project).at("amount"), 0.42)
        << "the wholesale restore must be scoped to the channel, not to the effect";
}

TEST(EditCurvePointCommand, MovingOrRemovingAPointThatDoesNotExistIsRefused) {
    ClipId clipId;
    Uuid   effectId;
    Project project = makeProjectWithCurveEffect(clipId, effectId);
    addPoints(project, clipId, effectId, CurveChannel::Master, {{0.5, 0.5}});

    EditCurvePointCommand move(clipId, effectId, CurveChannel::Master,
                               EditCurvePointCommand::Operation::Move, 3, 0.1, 0.1);
    const auto moved = move.apply(project);
    ASSERT_TRUE(moved.isError());
    EXPECT_EQ(moved.error().code(), ErrorCode::NotFound);

    EditCurvePointCommand remove(clipId, effectId, CurveChannel::Master,
                                 EditCurvePointCommand::Operation::Remove, 1, 0.0, 0.0);
    EXPECT_TRUE(remove.apply(project).isError());

    // A refused edit changes nothing, so the one real point is still there.
    EXPECT_EQ(curvePoints(curveParams(project), CurveChannel::Master).size(), 1u);
}

TEST(EditCurvePointCommand, AddingToAnEmptyCurveNeedsNoIndexAndRevertBeforeApplyFails) {
    ClipId clipId;
    Uuid   effectId;
    Project project = makeProjectWithCurveEffect(clipId, effectId);

    // index 9 with operation Add: ignored rather than refused, because add appends.
    EditCurvePointCommand cmd(clipId, effectId, CurveChannel::Master,
                              EditCurvePointCommand::Operation::Add, 9, 0.4, 0.4);
    EditCurvePointCommand never(clipId, effectId, CurveChannel::Master,
                                EditCurvePointCommand::Operation::Add, 0, 0.4, 0.4);
    const auto premature = never.revert(project);
    ASSERT_TRUE(premature.isError());
    EXPECT_EQ(premature.error().code(), ErrorCode::FailedPrecondition);

    ASSERT_TRUE(cmd.apply(project).isOk());
    const std::vector<CurvePoint> after = curvePoints(curveParams(project), CurveChannel::Master);
    ASSERT_EQ(after.size(), 1u) << "add appends at the next free index, whatever index says";
    EXPECT_DOUBLE_EQ(after[0].x, 0.4);
}

TEST(EditCurvePointCommand, AMissingClipOrEffectIsRefusedAndNamed) {
    ClipId clipId;
    Uuid   effectId;
    Project project = makeProjectWithCurveEffect(clipId, effectId);

    EditCurvePointCommand noClip(Uuid::generateV4(), effectId, CurveChannel::Master,
                                 EditCurvePointCommand::Operation::Add, 0, 0.5, 0.5);
    auto result = noClip.apply(project);
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::NotFound);

    EditCurvePointCommand noEffect(clipId, Uuid::generateV4(), CurveChannel::Master,
                                   EditCurvePointCommand::Operation::Add, 0, 0.5, 0.5);
    result = noEffect.apply(project);
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::NotFound);
    EXPECT_NE(result.error().message().find("effect"), std::string::npos);
}

// --- SetEffectResourceCommand ----------------------------------------------
//
// Requirement 7.9: applying a LUT by path is ONE undoable edit. The interesting case is the
// EMPTY prior, because it is the common one -- an effect just added has no resource -- and
// treating "no prior" as "nothing to restore" would make the first LUT applied to an effect
// un-undoable.

TEST(SetEffectResourceCommand, SetsThePathAndRevertRestoresAnEmptyPrior) {
    ClipId clipId;
    Uuid   effectId;
    Project project = makeProjectWithCurveEffect(clipId, effectId);
    ASSERT_TRUE(project.tracks[0].clips[0].effects[0].resourcePath.empty());

    SetEffectResourceCommand cmd(clipId, effectId, "/luts/look.cube");
    ASSERT_TRUE(cmd.apply(project).isOk());
    EXPECT_EQ(project.tracks[0].clips[0].effects[0].resourcePath, "/luts/look.cube");

    ASSERT_TRUE(cmd.revert(project).isOk());
    EXPECT_TRUE(project.tracks[0].clips[0].effects[0].resourcePath.empty())
        << "an empty prior must be restored, not skipped";
}

TEST(SetEffectResourceCommand, ReplacingAPathRestoresThePreviousOneNotAnEmptyString) {
    ClipId clipId;
    Uuid   effectId;
    Project project = makeProjectWithCurveEffect(clipId, effectId);
    project.tracks[0].clips[0].effects[0].resourcePath = "/luts/first.cube";

    SetEffectResourceCommand cmd(clipId, effectId, "/luts/second.cube");
    ASSERT_TRUE(cmd.apply(project).isOk());
    EXPECT_EQ(project.tracks[0].clips[0].effects[0].resourcePath, "/luts/second.cube");
    ASSERT_TRUE(cmd.revert(project).isOk());
    EXPECT_EQ(project.tracks[0].clips[0].effects[0].resourcePath, "/luts/first.cube");
}

TEST(SetEffectResourceCommand, AnEmptyPathClearsTheResourceWithoutRemovingTheEffect) {
    ClipId clipId;
    Uuid   effectId;
    Project project = makeProjectWithCurveEffect(clipId, effectId);
    project.tracks[0].clips[0].effects[0].resourcePath = "/luts/look.cube";

    SetEffectResourceCommand cmd(clipId, effectId, "");
    ASSERT_TRUE(cmd.apply(project).isOk());
    EXPECT_TRUE(project.tracks[0].clips[0].effects[0].resourcePath.empty());
    EXPECT_EQ(project.tracks[0].clips[0].effects.size(), 1u)
        << "clearing a LUT must not remove the effect it was on";
}

// Requirement 7.8 reaching the command: a path that cannot be read is still accepted, or a
// project that opens on one machine would be un-editable on another, and an agent setting up
// a project before its assets arrive is an ordinary workflow.
TEST(SetEffectResourceCommand, ANonExistentPathIsAcceptedRatherThanRefused) {
    ClipId clipId;
    Uuid   effectId;
    Project project = makeProjectWithCurveEffect(clipId, effectId);
    SetEffectResourceCommand cmd(clipId, effectId, "/definitely/not/here.cube");
    EXPECT_TRUE(cmd.apply(project).isOk());
    EXPECT_EQ(project.tracks[0].clips[0].effects[0].resourcePath, "/definitely/not/here.cube");
}

TEST(SetEffectResourceCommand, AMissingClipOrEffectIsRefusedAndRevertBeforeApplyFails) {
    ClipId clipId;
    Uuid   effectId;
    Project project = makeProjectWithCurveEffect(clipId, effectId);

    SetEffectResourceCommand noClip(Uuid::generateV4(), effectId, "/luts/a.cube");
    auto result = noClip.apply(project);
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::NotFound);

    SetEffectResourceCommand noEffect(clipId, Uuid::generateV4(), "/luts/a.cube");
    EXPECT_TRUE(noEffect.apply(project).isError());

    SetEffectResourceCommand never(clipId, effectId, "/luts/a.cube");
    result = never.revert(project);
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::FailedPrecondition);

    // Nothing was changed by any of the three refusals.
    EXPECT_TRUE(project.tracks[0].clips[0].effects[0].resourcePath.empty());
}

// --- AddEffectCommand ------------------------------------------------------

TEST(AddEffectCommand, AppendsAndRevertRemoves) {
    Uuid trackId;
    Project project = makeProjectWithOneTrack(trackId);
    const ClipId a = Uuid::generateV4();
    project.tracks[0].clips = {makeClip(a, ms(0), ms(0), ms(1000))};

    const Effect effect = Effect::brightness(0.2);
    AddEffectCommand cmd(a, effect);
    ASSERT_TRUE(cmd.apply(project).isOk());
    ASSERT_EQ(project.tracks[0].clips[0].effects.size(), 1u);
    EXPECT_EQ(project.tracks[0].clips[0].effects[0].id, effect.id);

    ASSERT_TRUE(cmd.revert(project).isOk());
    EXPECT_TRUE(project.tracks[0].clips[0].effects.empty());
}

TEST(AddEffectCommand, MissingClipFails) {
    Uuid trackId;
    Project project = makeProjectWithOneTrack(trackId);
    AddEffectCommand cmd(Uuid::generateV4(), Effect::blur(2.0));
    const auto result = cmd.apply(project);
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::NotFound);
}

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
// Ripple editing and gap management (usable-editor task 8; Requirement 5)
//
// Each of the three commands is checked for the three things Requirement 5.3 and
// task 8.4 ask of it: the shift is exactly the stated amount, the track still
// satisfies the ordered/non-overlapping invariant afterwards (asserted through the
// engine, which rejects and rolls back any command that breaks it), and the whole
// change reverses in ONE undo. Refusals are checked to leave the project untouched.
// ===========================================================================

// A project with `trackCount` empty video tracks; ids are returned in order.
Project makeProjectWithTracks(std::size_t trackCount, std::vector<Uuid>& trackIdsOut) {
    Project project;
    project.id = Uuid::generateV4();
    project.name = "ripple";
    for (std::size_t i = 0; i < trackCount; ++i) {
        Track track;
        track.id = Uuid::generateV4();
        track.kind = TrackKind::Video;
        trackIdsOut.push_back(track.id);
        project.tracks.push_back(std::move(track));
    }
    return project;
}

// Three one-second clips separated by half-second gaps: 0..1000, 1500..2500,
// 3000..4000. The gaps are what a ripple edit closes.
Project makeRippleProject(Uuid& trackIdOut, ClipId& a, ClipId& b, ClipId& c) {
    std::vector<Uuid> ids;
    Project project = makeProjectWithTracks(1, ids);
    trackIdOut = ids[0];
    a = Uuid::generateV4();
    b = Uuid::generateV4();
    c = Uuid::generateV4();
    project.tracks[0].clips.push_back(makeClip(a, ms(0), ms(0), ms(1000)));
    project.tracks[0].clips.push_back(makeClip(b, ms(1500), ms(0), ms(1000)));
    project.tracks[0].clips.push_back(makeClip(c, ms(3000), ms(0), ms(1000)));
    return project;
}

TEST(RippleDeleteCommand, ShiftsLaterClipsEarlierByTheRemovedDuration) {
    Uuid trackId;
    ClipId a, b, c;
    TimelineEngine engine(makeRippleProject(trackId, a, b, c));

    // Removing the middle clip (one second long) pulls only what followed it.
    ASSERT_TRUE(engine.apply(std::make_unique<RippleDeleteCommand>(b)).changed());

    const Project after = engine.snapshot();
    ASSERT_EQ(after.tracks[0].clips.size(), 2u);
    EXPECT_EQ(after.tracks[0].clips[0].id, a);
    EXPECT_EQ(after.tracks[0].clips[0].timelineStart, ms(0));   // before it: unmoved
    EXPECT_EQ(after.tracks[0].clips[1].id, c);
    EXPECT_EQ(after.tracks[0].clips[1].timelineStart, ms(2000));  // 3000 - 1000
    EXPECT_EQ(after.tracks[0].clips[1].duration(), ms(1000));     // duration untouched
    EXPECT_TRUE(checkTimelineInvariants(after).isOk());
}

TEST(RippleDeleteCommand, ReversesInOneUndo) {
    Uuid trackId;
    ClipId a, b, c;
    TimelineEngine engine(makeRippleProject(trackId, a, b, c));
    const Project before = engine.snapshot();

    ASSERT_TRUE(engine.apply(std::make_unique<RippleDeleteCommand>(a)).changed());
    ASSERT_EQ(engine.snapshot().tracks[0].clips.size(), 2u);

    // ONE undo, not one per shifted clip (Requirement 5.3).
    ASSERT_TRUE(engine.undo().changed());
    const Project restored = engine.snapshot();
    ASSERT_EQ(restored.tracks[0].clips.size(), 3u);
    for (std::size_t i = 0; i < restored.tracks[0].clips.size(); ++i) {
        EXPECT_EQ(restored.tracks[0].clips[i].id, before.tracks[0].clips[i].id);
        EXPECT_EQ(restored.tracks[0].clips[i].timelineStart,
                  before.tracks[0].clips[i].timelineStart);
    }
    EXPECT_FALSE(engine.canUndo());  // exactly one entry was recorded
}

TEST(RippleDeleteCommand, AnUnknownClipChangesNothing) {
    Uuid trackId;
    ClipId a, b, c;
    TimelineEngine engine(makeRippleProject(trackId, a, b, c));

    const CommandResult result =
        engine.apply(std::make_unique<RippleDeleteCommand>(Uuid::generateV4()));
    EXPECT_FALSE(result.changed());
    EXPECT_EQ(engine.snapshot().tracks[0].clips.size(), 3u);
    EXPECT_FALSE(engine.canUndo());
}

TEST(RippleTrimCommand, EndEdgeShiftsLaterClipsByTheDurationChange) {
    Uuid trackId;
    ClipId a, b, c;
    TimelineEngine engine(makeRippleProject(trackId, a, b, c));

    // Shorten the first clip from 1000ms to 600ms: a -400ms duration change.
    ASSERT_TRUE(engine.apply(std::make_unique<RippleTrimCommand>(
                                 a, RippleTrimCommand::Edge::End, ms(600),
                                 FrameRate::fps25(), ms(1000)))
                    .changed());

    const Project after = engine.snapshot();
    EXPECT_EQ(after.tracks[0].clips[0].duration(), ms(600));
    EXPECT_EQ(after.tracks[0].clips[0].timelineStart, ms(0));      // leading edge fixed
    EXPECT_EQ(after.tracks[0].clips[1].timelineStart, ms(1100));   // 1500 - 400
    EXPECT_EQ(after.tracks[0].clips[2].timelineStart, ms(2600));   // 3000 - 400
    EXPECT_EQ(after.tracks[0].clips[1].duration(), ms(1000));      // followers unretimed
    EXPECT_TRUE(checkTimelineInvariants(after).isOk());

    ASSERT_TRUE(engine.undo().changed());
    const Project restored = engine.snapshot();
    EXPECT_EQ(restored.tracks[0].clips[0].duration(), ms(1000));
    EXPECT_EQ(restored.tracks[0].clips[1].timelineStart, ms(1500));
    EXPECT_EQ(restored.tracks[0].clips[2].timelineStart, ms(3000));
    EXPECT_FALSE(engine.canUndo());
}

TEST(RippleTrimCommand, StartEdgeLeavesTheTrailingEdgeAndLaterClipsWhereTheyWere) {
    Uuid trackId;
    ClipId a, b, c;
    TimelineEngine engine(makeRippleProject(trackId, a, b, c));

    // Move the first clip's in-point 200ms later: its leading edge moves with it,
    // its trailing edge does not, so nothing after it may move.
    ASSERT_TRUE(engine.apply(std::make_unique<RippleTrimCommand>(
                                 a, RippleTrimCommand::Edge::Start, ms(200),
                                 FrameRate::fps25(), ms(1000)))
                    .changed());

    const Project after = engine.snapshot();
    EXPECT_EQ(after.tracks[0].clips[0].sourceIn, ms(200));
    EXPECT_EQ(after.tracks[0].clips[0].timelineStart, ms(200));
    EXPECT_EQ(after.tracks[0].clips[0].timelineEnd(), ms(1000));  // unchanged
    EXPECT_EQ(after.tracks[0].clips[1].timelineStart, ms(1500));  // untouched
    EXPECT_EQ(after.tracks[0].clips[2].timelineStart, ms(3000));  // untouched
    EXPECT_TRUE(checkTimelineInvariants(after).isOk());
}

// --- PR 397: multicam ripple-trim synchronisation --------------------------
//
// The backlog entry's own acceptance check, expressed as a test: two clips on
// DIFFERENT tracks in one clipGroup, plus a third ungrouped clip; extending the
// in-point of one grouped clip moves both grouped clips by exactly that duration,
// leaves the ungrouped clip alone, and undoes as one history entry.
TEST(RippleTrimCommand, KeepsGroupedMulticamAnglesSynchronised) {
    std::vector<Uuid> trackIds;
    Project project = makeProjectWithTracks(3, trackIds);

    // Both grouped angles start at the same place with the same source range, and
    // both have source to spare before their in-point so the trim can extend it.
    const ClipId angleOne = Uuid::generateV4();
    const ClipId angleTwo = Uuid::generateV4();
    const ClipId ungrouped = Uuid::generateV4();
    project.tracks[0].clips.push_back(makeClip(angleOne, ms(1000), ms(500), ms(1500)));
    project.tracks[1].clips.push_back(makeClip(angleTwo, ms(1000), ms(500), ms(1500)));
    project.tracks[2].clips.push_back(makeClip(ungrouped, ms(1000), ms(500), ms(1500)));

    ClipGroup group;
    group.id = Uuid::generateV4();
    group.clipIds = {angleOne, angleTwo};
    project.clipGroups.push_back(group);

    TimelineEngine engine(std::move(project));

    // Extend the in-point by 300ms (500 -> 200): each grouped clip's leading edge
    // moves 300ms EARLIER, which is what "moves by exactly that duration" means.
    ASSERT_TRUE(engine.apply(std::make_unique<RippleTrimCommand>(
                                 angleOne, RippleTrimCommand::Edge::Start, ms(200),
                                 FrameRate::fps25(), ms(1500)))
                    .changed());

    const Project after = engine.snapshot();
    EXPECT_EQ(after.tracks[0].clips[0].sourceIn, ms(200));
    EXPECT_EQ(after.tracks[0].clips[0].timelineStart, ms(700));  // 1000 - 300
    // The other angle followed by the same amount and stays aligned with the first.
    EXPECT_EQ(after.tracks[1].clips[0].sourceIn, ms(200));
    EXPECT_EQ(after.tracks[1].clips[0].timelineStart, ms(700));
    EXPECT_EQ(after.tracks[1].clips[0].timelineStart, after.tracks[0].clips[0].timelineStart);
    EXPECT_EQ(after.tracks[1].clips[0].duration(), after.tracks[0].clips[0].duration());
    // The ungrouped clip is untouched.
    EXPECT_EQ(after.tracks[2].clips[0].sourceIn, ms(500));
    EXPECT_EQ(after.tracks[2].clips[0].timelineStart, ms(1000));
    EXPECT_TRUE(checkTimelineInvariants(after).isOk());

    // One history entry for the whole cross-track change.
    ASSERT_TRUE(engine.undo().changed());
    const Project restored = engine.snapshot();
    EXPECT_EQ(restored.tracks[0].clips[0].timelineStart, ms(1000));
    EXPECT_EQ(restored.tracks[0].clips[0].sourceIn, ms(500));
    EXPECT_EQ(restored.tracks[1].clips[0].timelineStart, ms(1000));
    EXPECT_EQ(restored.tracks[1].clips[0].sourceIn, ms(500));
    EXPECT_FALSE(engine.canUndo());
}

TEST(RippleTrimCommand, AGroupedAngleThatCannotAbsorbTheTrimRefusesTheWholeEdit) {
    std::vector<Uuid> trackIds;
    Project project = makeProjectWithTracks(2, trackIds);

    // The second angle has no source before its in-point, so extending by 300ms
    // cannot apply to it; the whole command must then change nothing at all.
    const ClipId angleOne = Uuid::generateV4();
    const ClipId angleTwo = Uuid::generateV4();
    project.tracks[0].clips.push_back(makeClip(angleOne, ms(1000), ms(500), ms(1500)));
    project.tracks[1].clips.push_back(makeClip(angleTwo, ms(1000), ms(0), ms(1000)));

    ClipGroup group;
    group.id = Uuid::generateV4();
    group.clipIds = {angleOne, angleTwo};
    project.clipGroups.push_back(group);

    TimelineEngine engine(std::move(project));
    const Project before = engine.snapshot();

    const CommandResult result = engine.apply(std::make_unique<RippleTrimCommand>(
        angleOne, RippleTrimCommand::Edge::Start, ms(200), FrameRate::fps25(), ms(1500)));
    EXPECT_FALSE(result.changed());

    const Project after = engine.snapshot();
    EXPECT_EQ(after.tracks[0].clips[0].timelineStart, before.tracks[0].clips[0].timelineStart);
    EXPECT_EQ(after.tracks[0].clips[0].sourceIn, before.tracks[0].clips[0].sourceIn);
    EXPECT_EQ(after.tracks[1].clips[0].timelineStart, before.tracks[1].clips[0].timelineStart);
    EXPECT_EQ(after.tracks[1].clips[0].sourceIn, before.tracks[1].clips[0].sourceIn);
    EXPECT_FALSE(engine.canUndo());
}

TEST(CloseGapCommand, ClosesTheFollowingGapAndLeavesDurationsUnchanged) {
    Uuid trackId;
    ClipId a, b, c;
    TimelineEngine engine(makeRippleProject(trackId, a, b, c));

    // The gap after the first clip is 500ms (it ends at 1000, b starts at 1500).
    ASSERT_TRUE(engine.apply(std::make_unique<CloseGapCommand>(a)).changed());

    const Project after = engine.snapshot();
    EXPECT_EQ(after.tracks[0].clips[0].timelineStart, ms(0));     // the clip itself stays
    EXPECT_EQ(after.tracks[0].clips[1].timelineStart, ms(1000));  // abuts its predecessor
    EXPECT_EQ(after.tracks[0].clips[2].timelineStart, ms(2500));  // 3000 - 500
    for (const Clip& clip : after.tracks[0].clips) {
        EXPECT_EQ(clip.duration(), ms(1000));  // Requirement 5.5: durations unchanged
    }
    EXPECT_TRUE(checkTimelineInvariants(after).isOk());

    ASSERT_TRUE(engine.undo().changed());
    EXPECT_EQ(engine.snapshot().tracks[0].clips[1].timelineStart, ms(1500));
    EXPECT_FALSE(engine.canUndo());
}

TEST(CloseGapCommand, RefusesWhenTheClipIsLastOnItsTrack) {
    Uuid trackId;
    ClipId a, b, c;
    TimelineEngine engine(makeRippleProject(trackId, a, b, c));
    const Project before = engine.snapshot();

    const CommandResult result = engine.apply(std::make_unique<CloseGapCommand>(c));
    EXPECT_FALSE(result.changed());
    const Project after = engine.snapshot();
    for (std::size_t i = 0; i < after.tracks[0].clips.size(); ++i) {
        EXPECT_EQ(after.tracks[0].clips[i].timelineStart,
                  before.tracks[0].clips[i].timelineStart);
    }
    EXPECT_FALSE(engine.canUndo());
}

TEST(CloseGapCommand, RefusesWhenNoGapFollows) {
    std::vector<Uuid> trackIds;
    Project project = makeProjectWithTracks(1, trackIds);
    const ClipId a = Uuid::generateV4();
    const ClipId b = Uuid::generateV4();
    // Abutting clips: the second begins exactly where the first ends.
    project.tracks[0].clips.push_back(makeClip(a, ms(0), ms(0), ms(1000)));
    project.tracks[0].clips.push_back(makeClip(b, ms(1000), ms(0), ms(1000)));
    TimelineEngine engine(std::move(project));

    const CommandResult result = engine.apply(std::make_unique<CloseGapCommand>(a));
    EXPECT_FALSE(result.changed());
    EXPECT_EQ(engine.snapshot().tracks[0].clips[1].timelineStart, ms(1000));
    EXPECT_FALSE(engine.canUndo());
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

    ASSERT_TRUE(engine.apply(std::make_unique<SetProjectSettingsCommand>(
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
    ASSERT_TRUE(engine.apply(std::make_unique<SetProjectSettingsCommand>(
                                 std::nullopt, std::nullopt, ColorSpace::DisplayP3))
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
    project.tracks[0].clips.push_back(
        makeClip(clipId, ms(1500), ms(200), ms(1700)));
    const Clip before = project.tracks[0].clips[0];
    TimelineEngine engine(std::move(project));

    ASSERT_TRUE(engine.apply(std::make_unique<SetProjectSettingsCommand>(
                                 FrameRate::fps24(), std::nullopt, std::nullopt))
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

// ===========================================================================
// Text and titles (usable-editor task 12; Requirement 9)
// ===========================================================================

// A text clip is placed through the existing AddClipCommand — no new command —
// and, because its assetRef is left at its default (nil, "invalid") value,
// AddClipCommand's own asset-registration step adds nothing to Project.assets,
// exactly the behaviour that lets a text clip carry no source media at all.
TEST(AddClipCommand, PlacesATextClipWithoutRegisteringAnyAsset) {
    Uuid trackId;
    TimelineEngine engine(makeProjectWithOneTextTrack(trackId));

    const ClipId clipId = Uuid::generateV4();
    ASSERT_TRUE(engine.apply(std::make_unique<AddClipCommand>(
                                 trackId, makeTextClip(clipId, ms(0), ms(2000), "Hello")))
                    .changed());

    const Project snap = engine.snapshot();
    ASSERT_EQ(snap.tracks[0].clips.size(), 1u);
    const Clip& clip = snap.tracks[0].clips[0];
    EXPECT_TRUE(clip.isTextClip());
    EXPECT_EQ(clip.textStyle->content, "Hello");
    EXPECT_FALSE(clip.assetRef.isValid());
    EXPECT_TRUE(snap.assets.empty());
}

TEST(SetTextContentCommand, ChangesTheStringAndUndoRestoresIt) {
    Uuid trackId;
    Project project = makeProjectWithOneTextTrack(trackId);
    const ClipId clipId = Uuid::generateV4();
    project.tracks[0].clips.push_back(makeTextClip(clipId, ms(0), ms(1000), "Original"));
    TimelineEngine engine(std::move(project));

    ASSERT_TRUE(engine.apply(std::make_unique<SetTextContentCommand>(clipId, "Changed"))
                    .changed());
    EXPECT_EQ(engine.snapshot().tracks[0].clips[0].textStyle->content, "Changed");

    ASSERT_TRUE(engine.undo().changed());
    EXPECT_EQ(engine.snapshot().tracks[0].clips[0].textStyle->content, "Original");
}

TEST(SetTextContentCommand, RefusesAClipThatIsNotATextClip) {
    Uuid trackId;
    Project project = makeProjectWithOneTrack(trackId);
    const ClipId clipId = Uuid::generateV4();
    project.tracks[0].clips.push_back(makeClip(clipId, ms(0), ms(0), ms(1000)));
    TimelineEngine engine(std::move(project));

    const CommandResult result =
        engine.apply(std::make_unique<SetTextContentCommand>(clipId, "New text"));
    EXPECT_FALSE(result.changed());
    EXPECT_FALSE(engine.canUndo());
}

// Requirement 9.2: any subset of font family, size, colour, alignment and
// position changes in one undoable edit — mirroring
// SetProjectSettingsCommand's own optional-field pattern. Only two of the nine
// possible fields are supplied here; the rest must be left exactly as they were.
TEST(SetTextStyleCommand, ChangesOnlyTheSuppliedFieldsAndUndoRestoresAll) {
    Uuid trackId;
    Project project = makeProjectWithOneTextTrack(trackId);
    const ClipId clipId = Uuid::generateV4();
    Clip clip = makeTextClip(clipId, ms(0), ms(1000), "Title");
    clip.textStyle->fontFamily = "serif";
    clip.textStyle->pointSize = 18.0;
    clip.textStyle->colorR = 0.2;
    clip.textStyle->alignment = TextAlignment::Left;
    clip.textStyle->x = 0.1;
    clip.textStyle->y = 0.9;
    const TextStyle before = *clip.textStyle;
    project.tracks[0].clips.push_back(std::move(clip));
    TimelineEngine engine(std::move(project));

    // Only pointSize and alignment are supplied; everything else is std::nullopt.
    ASSERT_TRUE(engine.apply(std::make_unique<SetTextStyleCommand>(
                                 clipId, std::nullopt, 36.0, std::nullopt, std::nullopt,
                                 std::nullopt, std::nullopt, TextAlignment::Right,
                                 std::nullopt, std::nullopt))
                    .changed());

    const TextStyle after = *engine.snapshot().tracks[0].clips[0].textStyle;
    EXPECT_EQ(after.pointSize, 36.0);               // changed
    EXPECT_EQ(after.alignment, TextAlignment::Right); // changed
    EXPECT_EQ(after.fontFamily, before.fontFamily);   // untouched
    EXPECT_EQ(after.colorR, before.colorR);           // untouched
    EXPECT_EQ(after.x, before.x);                     // untouched
    EXPECT_EQ(after.y, before.y);                     // untouched

    ASSERT_TRUE(engine.undo().changed());
    const TextStyle restored = *engine.snapshot().tracks[0].clips[0].textStyle;
    EXPECT_EQ(restored.pointSize, before.pointSize);
    EXPECT_EQ(restored.alignment, before.alignment);
    EXPECT_EQ(restored.fontFamily, before.fontFamily);
}

// Requirement 9's own numeric bound (point size must be > 0) is enforced by
// TextStyle::isValid(), and a change that would violate it is refused with the
// project left exactly as it was — the same own-invariant check
// SetProjectSettingsCommand already establishes for fps/canvas.
TEST(SetTextStyleCommand, RefusesAChangeThatWouldMakeTheStyleInvalidAndLeavesItUnchanged) {
    Uuid trackId;
    Project project = makeProjectWithOneTextTrack(trackId);
    const ClipId clipId = Uuid::generateV4();
    project.tracks[0].clips.push_back(makeTextClip(clipId, ms(0), ms(1000), "Title"));
    TimelineEngine engine(std::move(project));

    const CommandResult result = engine.apply(std::make_unique<SetTextStyleCommand>(
        clipId, std::nullopt, -5.0, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
        std::nullopt, std::nullopt, std::nullopt));  // pointSize -5 is not > 0
    EXPECT_FALSE(result.changed());
    EXPECT_EQ(engine.snapshot().tracks[0].clips[0].textStyle->pointSize, 24.0);  // default, untouched
    EXPECT_FALSE(engine.canUndo());
}

TEST(SetTextStyleCommand, RefusesAClipThatIsNotATextClip) {
    Uuid trackId;
    Project project = makeProjectWithOneTrack(trackId);
    const ClipId clipId = Uuid::generateV4();
    project.tracks[0].clips.push_back(makeClip(clipId, ms(0), ms(0), ms(1000)));
    TimelineEngine engine(std::move(project));

    const CommandResult result = engine.apply(std::make_unique<SetTextStyleCommand>(
        clipId, std::nullopt, 36.0, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
        std::nullopt, std::nullopt, std::nullopt));
    EXPECT_FALSE(result.changed());
    EXPECT_FALSE(engine.canUndo());
}

// Requirement 9.1's round-trip half at the domain level: a text clip's own
// existing operations — move, trim, split — already work through the identical
// commands every other clip uses, since a text clip is just a Clip whose
// textStyle happens to be set. Moving one is the simplest of the three to prove.
TEST(MoveClipCommand, MovesATextClipThroughTheIdenticalCommandEveryOtherClipUses) {
    Uuid trackId;
    Project project = makeProjectWithOneTextTrack(trackId);
    const ClipId clipId = Uuid::generateV4();
    project.tracks[0].clips.push_back(makeTextClip(clipId, ms(0), ms(1000), "Title"));
    TimelineEngine engine(std::move(project));

    ASSERT_TRUE(engine.apply(std::make_unique<MoveClipCommand>(clipId, ms(5000))).changed());

    const Project snap = engine.snapshot();
    const Clip& moved = snap.tracks[0].clips[0];
    EXPECT_EQ(moved.timelineStart, ms(5000));
    EXPECT_TRUE(moved.isTextClip());
    EXPECT_EQ(moved.textStyle->content, "Title");  // the move touched nothing else
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

// ---------------------------------------------------------------------------
// Captions and transcription (usable-editor task 13; Requirement 10)
// ---------------------------------------------------------------------------

TEST(AddClipCommand, PlacesACaptionCueWithoutRegisteringAnyAsset) {
    Uuid trackId;
    TimelineEngine engine(makeProjectWithOneCaptionTrack(trackId));

    const ClipId clipId = Uuid::generateV4();
    ASSERT_TRUE(engine.apply(std::make_unique<AddClipCommand>(
                                 trackId, makeCaptionCue(clipId, ms(0), ms(2000), "Hello")))
                    .changed());

    const Project snap = engine.snapshot();
    ASSERT_EQ(snap.tracks[0].clips.size(), 1u);
    const Clip& clip = snap.tracks[0].clips[0];
    EXPECT_TRUE(clip.isCaptionCue());
    EXPECT_EQ(*clip.captionText, "Hello");
    EXPECT_FALSE(clip.assetRef.isValid());
    EXPECT_TRUE(snap.assets.empty());
}

TEST(SetCaptionTextCommand, ChangesTheStringAndUndoRestoresIt) {
    Uuid trackId;
    Project project = makeProjectWithOneCaptionTrack(trackId);
    const ClipId clipId = Uuid::generateV4();
    project.tracks[0].clips.push_back(makeCaptionCue(clipId, ms(0), ms(1000), "Original"));
    TimelineEngine engine(std::move(project));

    ASSERT_TRUE(engine.apply(std::make_unique<SetCaptionTextCommand>(clipId, "Changed"))
                    .changed());
    EXPECT_EQ(*engine.snapshot().tracks[0].clips[0].captionText, "Changed");

    ASSERT_TRUE(engine.undo().changed());
    EXPECT_EQ(*engine.snapshot().tracks[0].clips[0].captionText, "Original");
}

TEST(SetCaptionTextCommand, RefusesAnEmptyString) {
    Uuid trackId;
    Project project = makeProjectWithOneCaptionTrack(trackId);
    const ClipId clipId = Uuid::generateV4();
    project.tracks[0].clips.push_back(makeCaptionCue(clipId, ms(0), ms(1000), "Original"));
    TimelineEngine engine(std::move(project));

    const CommandResult result = engine.apply(std::make_unique<SetCaptionTextCommand>(clipId, ""));
    EXPECT_FALSE(result.changed());
    EXPECT_EQ(*engine.snapshot().tracks[0].clips[0].captionText, "Original");
    EXPECT_FALSE(engine.canUndo());
}

TEST(SetCaptionTextCommand, RefusesAClipThatIsNotACaptionCue) {
    Uuid trackId;
    Project project = makeProjectWithOneTrack(trackId);
    const ClipId clipId = Uuid::generateV4();
    project.tracks[0].clips.push_back(makeClip(clipId, ms(0), ms(0), ms(1000)));
    TimelineEngine engine(std::move(project));

    const CommandResult result =
        engine.apply(std::make_unique<SetCaptionTextCommand>(clipId, "New text"));
    EXPECT_FALSE(result.changed());
    EXPECT_FALSE(engine.canUndo());
}

// Requirement 10.2: "retime" changes timelineStart and/or duration together in
// ONE undoable edit — mirroring SetTextStyleCommand's own optional-field
// pattern, but over the two timing fields RetimeCaptionCueCommand owns.
TEST(RetimeCaptionCueCommand, ChangesBothFieldsTogetherAndUndoRestoresThem) {
    Uuid trackId;
    Project project = makeProjectWithOneCaptionTrack(trackId);
    const ClipId clipId = Uuid::generateV4();
    project.tracks[0].clips.push_back(makeCaptionCue(clipId, ms(0), ms(1000), "Hello"));
    TimelineEngine engine(std::move(project));

    ASSERT_TRUE(engine.apply(std::make_unique<RetimeCaptionCueCommand>(clipId, ms(2000), ms(500)))
                    .changed());

    const Clip& after = engine.snapshot().tracks[0].clips[0];
    EXPECT_EQ(after.timelineStart, ms(2000));
    EXPECT_EQ(after.duration(), ms(500));
    EXPECT_EQ(*after.captionText, "Hello");  // retiming touched nothing else

    ASSERT_TRUE(engine.undo().changed());
    const Clip& restored = engine.snapshot().tracks[0].clips[0];
    EXPECT_EQ(restored.timelineStart, ms(0));
    EXPECT_EQ(restored.duration(), ms(1000));
}

TEST(RetimeCaptionCueCommand, ChangesOnlyTheSuppliedField) {
    Uuid trackId;
    Project project = makeProjectWithOneCaptionTrack(trackId);
    const ClipId clipId = Uuid::generateV4();
    project.tracks[0].clips.push_back(makeCaptionCue(clipId, ms(1000), ms(500), "Hello"));
    TimelineEngine engine(std::move(project));

    // Only the start is supplied; duration must be left exactly as it was.
    ASSERT_TRUE(
        engine.apply(std::make_unique<RetimeCaptionCueCommand>(clipId, ms(3000), std::nullopt))
            .changed());

    const Clip& after = engine.snapshot().tracks[0].clips[0];
    EXPECT_EQ(after.timelineStart, ms(3000));
    EXPECT_EQ(after.duration(), ms(500));
}

TEST(RetimeCaptionCueCommand, RefusesAMoveThatWouldOverlapAnotherCueOnTheSameTrack) {
    Uuid trackId;
    Project project = makeProjectWithOneCaptionTrack(trackId);
    const ClipId firstId = Uuid::generateV4();
    const ClipId secondId = Uuid::generateV4();
    project.tracks[0].clips.push_back(makeCaptionCue(firstId, ms(0), ms(1000), "First"));
    project.tracks[0].clips.push_back(makeCaptionCue(secondId, ms(2000), ms(1000), "Second"));
    TimelineEngine engine(std::move(project));

    // Moving "Second" to overlap "First" must be refused and leave both cues
    // exactly where they were (mirroring MoveClipCommand's own overlap rule).
    const CommandResult result =
        engine.apply(std::make_unique<RetimeCaptionCueCommand>(secondId, ms(500), std::nullopt));
    EXPECT_FALSE(result.changed());
    EXPECT_EQ(engine.snapshot().tracks[0].clips.size(), 2u);
    EXPECT_FALSE(engine.canUndo());
}

TEST(RetimeCaptionCueCommand, RefusesAClipThatIsNotACaptionCue) {
    Uuid trackId;
    Project project = makeProjectWithOneTrack(trackId);
    const ClipId clipId = Uuid::generateV4();
    project.tracks[0].clips.push_back(makeClip(clipId, ms(0), ms(0), ms(1000)));
    TimelineEngine engine(std::move(project));

    const CommandResult result =
        engine.apply(std::make_unique<RetimeCaptionCueCommand>(clipId, ms(500), std::nullopt));
    EXPECT_FALSE(result.changed());
    EXPECT_FALSE(engine.canUndo());
}

// Requirement 10.1's round-trip half at the domain level: a caption cue's own
// existing operations already work through the identical commands every other
// clip uses, since a caption cue is just a Clip whose captionText happens to
// be set. Removing one via DeleteClipCommand is what timeline.remove_caption_cue
// reuses directly.
TEST(DeleteClipCommand, RemovesACaptionCueThroughTheIdenticalCommandEveryOtherClipUses) {
    Uuid trackId;
    Project project = makeProjectWithOneCaptionTrack(trackId);
    const ClipId clipId = Uuid::generateV4();
    project.tracks[0].clips.push_back(makeCaptionCue(clipId, ms(0), ms(1000), "Hello"));
    TimelineEngine engine(std::move(project));

    ASSERT_TRUE(engine.apply(std::make_unique<DeleteClipCommand>(clipId)).changed());
    EXPECT_TRUE(engine.snapshot().tracks[0].clips.empty());

    ASSERT_TRUE(engine.undo().changed());
    ASSERT_EQ(engine.snapshot().tracks[0].clips.size(), 1u);
    EXPECT_EQ(*engine.snapshot().tracks[0].clips[0].captionText, "Hello");
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
