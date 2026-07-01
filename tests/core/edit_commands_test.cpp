// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for the concrete editing commands (task 3.3) — AddClipCommand,
// DeleteClipCommand, MoveClipCommand, TrimClipCommand, SplitClipCommand,
// ReorderClipsCommand, and AddEffectCommand.
//
// These exercise both the command semantics themselves and their apply()/revert()
// round-trip. Where an operation must respect a timeline invariant (overlap
// rejection on move/add) the command is driven through a TimelineEngine so the
// engine's invariant enforcement and atomic rollback are also covered; where the
// focus is the exact-inverse revert(), commands are applied directly to a Project.
//
// _Requirements: 2.2, 2.3, 2.4, 2.5, 2.6, 2.7_

#include "core/EditCommands.hpp"

#include <memory>

#include <gtest/gtest.h>

#include "core/Clip.hpp"
#include "core/FrameRate.hpp"
#include "core/Project.hpp"
#include "core/TimelineEngine.hpp"
#include "core/Track.hpp"
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

}  // namespace
}  // namespace palmier
