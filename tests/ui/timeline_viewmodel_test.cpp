// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/ui/timeline_viewmodel_test.cpp — unit tests for the Qt-free timeline
// presentation adapter (task 19.2; Requirements 2.1-2.7).
//
// These exercise TimelineViewModel directly, without any Qt runtime (the sandbox
// has no Qt 6). They verify the two responsibilities of the adapter:
//
//   1. Model-shape projection — the engine's Project is surfaced as a stable
//      tracks x clips grid a QAbstractItemModel can read verbatim, across the
//      full 1-50 track range (Requirement 2.1).
//   2. Gesture -> EditCommand mapping — each interactive gesture (drag-move,
//      trim, split, reorder) drives the SAME concrete EditCommand through the
//      engine, and the CommandResult is classified into the exact indication
//      Requirement 2 calls for: an invalid drop retains the clip (2.3), a split
//      that misses reports nothing-to-split and changes nothing (2.6), a reorder
//      preserves the clip count (2.7), a trim lands on the boundary (2.4), and
//      undo with empty history is a no-op with an indication (2.10).

#include "ui/TimelineViewModel.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "core/Clip.hpp"
#include "core/Duration.hpp"
#include "core/FrameRate.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/Project.hpp"
#include "core/Resolution.hpp"
#include "core/TimelineEngine.hpp"
#include "core/Track.hpp"
#include "core/Uuid.hpp"

namespace palmier::ui {
namespace {

using palmier::Clip;
using palmier::Duration;
using palmier::FrameRate;
using palmier::MediaAssetRef;
using palmier::Project;
using palmier::TimelineEngine;
using palmier::Track;
using palmier::TrackKind;
using palmier::Uuid;

Duration ms(std::int64_t v) { return Duration::fromMilliseconds(v); }

// A clip [startMs, startMs+durMs) drawing [0, durMs) of a fresh source asset.
Clip makeClip(std::int64_t startMs, std::int64_t durMs) {
    Clip c;
    c.id = Uuid::generateV4();
    c.assetRef = MediaAssetRef(Uuid::generateV4(), "mem://clip");
    c.timelineStart = ms(startMs);
    c.sourceIn = ms(0);
    c.sourceOut = ms(durMs);
    return c;
}

Track makeTrack(TrackKind kind = TrackKind::Video) {
    Track t;
    t.id = Uuid::generateV4();
    t.kind = kind;
    return t;
}

Project makeProject() {
    Project p;
    p.id = Uuid::generateV4();
    p.name = "test";
    p.timelineFps = FrameRate::fps30();
    p.canvas = palmier::Resolution{1920, 1080};
    return p;
}

// A project with `n` video tracks, each carrying one 1000 ms clip.
Project makeProjectWithTracks(std::size_t n) {
    Project p = makeProject();
    for (std::size_t i = 0; i < n; ++i) {
        Track t = makeTrack();
        t.clips.push_back(makeClip(0, 1000));
        p.tracks.push_back(std::move(t));
    }
    return p;
}

// --- Model shape ----------------------------------------------------------

TEST(TimelineViewModel, ProjectsTracksAndClipsAsGrid) {
    Project p = makeProject();
    Track t = makeTrack();
    Clip a = makeClip(0, 1000);
    Clip b = makeClip(1000, 1000);
    t.clips = {a, b};
    p.tracks.push_back(t);

    TimelineEngine engine(p);
    TimelineViewModel vm(engine);

    ASSERT_EQ(vm.trackCount(), 1u);
    EXPECT_EQ(vm.clipCount(0), 2u);

    const auto track = vm.trackAt(0);
    ASSERT_TRUE(track.has_value());
    EXPECT_EQ(track->id, t.id);
    EXPECT_EQ(track->clipCount, 2u);
    EXPECT_EQ(track->kind, TrackKind::Video);

    const auto clip0 = vm.clipAt(0, 0);
    ASSERT_TRUE(clip0.has_value());
    EXPECT_EQ(clip0->id, a.id);
    EXPECT_EQ(clip0->timelineStart, ms(0));
    EXPECT_EQ(clip0->duration, ms(1000));

    const auto located = vm.locate(b.id);
    ASSERT_TRUE(located.has_value());
    EXPECT_EQ(located->first, 0u);
    EXPECT_EQ(located->second, 1u);

    // Out-of-range access is safe and empty.
    EXPECT_FALSE(vm.trackAt(5).has_value());
    EXPECT_FALSE(vm.clipAt(0, 9).has_value());
    EXPECT_EQ(vm.clipCount(9), 0u);
}

// Requirement 2.1: the timeline supports between 1 and 50 tracks.
TEST(TimelineViewModel, SupportsOneToFiftyTracks) {
    {
        Project p = makeProjectWithTracks(1);
        TimelineEngine engine(p);
        TimelineViewModel vm(engine);
        EXPECT_EQ(vm.trackCount(), 1u);
        EXPECT_TRUE(vm.trackCountSupported());
        EXPECT_TRUE(vm.canAddTrack());
    }
    {
        Project p = makeProjectWithTracks(50);
        TimelineEngine engine(p);
        TimelineViewModel vm(engine);
        EXPECT_EQ(vm.trackCount(), 50u);
        EXPECT_TRUE(vm.trackCountSupported());
        EXPECT_FALSE(vm.canAddTrack());  // at the 50-track cap

        // Every track/clip is addressable across the full range.
        for (std::size_t i = 0; i < 50; ++i) {
            ASSERT_TRUE(vm.trackAt(i).has_value());
            EXPECT_EQ(vm.clipCount(i), 1u);
            EXPECT_TRUE(vm.clipAt(i, 0).has_value());
        }
    }
    {
        Project p = makeProjectWithTracks(51);
        TimelineEngine engine(p);
        TimelineViewModel vm(engine);
        EXPECT_EQ(vm.trackCount(), 51u);
        EXPECT_FALSE(vm.trackCountSupported());  // beyond the supported maximum
    }
    {
        Project empty = makeProject();
        TimelineEngine engine(empty);
        TimelineViewModel vm(engine);
        EXPECT_EQ(vm.trackCount(), 0u);
        EXPECT_FALSE(vm.trackCountSupported());  // 0 tracks is below the minimum
    }
}

// --- Drag-move gesture (Requirements 2.2, 2.3) -----------------------------

TEST(TimelineViewModel, MoveClipToValidPositionApplies) {
    Project p = makeProject();
    Track t = makeTrack();
    Clip a = makeClip(0, 1000);
    t.clips = {a};
    p.tracks.push_back(t);

    TimelineEngine engine(p);
    TimelineViewModel vm(engine);

    const GestureResult r = vm.moveClip(a.id, ms(5000));
    EXPECT_TRUE(r.changed());
    EXPECT_EQ(r.indication, GestureIndication::Applied);
    EXPECT_EQ(vm.lastIndication(), GestureIndication::Applied);

    const auto moved = vm.clipAt(0, 0);
    ASSERT_TRUE(moved.has_value());
    EXPECT_EQ(moved->timelineStart, ms(5000));
}

TEST(TimelineViewModel, MoveClipOntoOverlapIsInvalidDropAndRetainsClip) {
    Project p = makeProject();
    Track t = makeTrack();
    Clip a = makeClip(0, 1000);
    Clip b = makeClip(1000, 1000);
    t.clips = {a, b};
    p.tracks.push_back(t);

    TimelineEngine engine(p);
    TimelineViewModel vm(engine);

    // Dropping A at 1500 would overlap B [1000,2000).
    const GestureResult r = vm.moveClip(a.id, ms(1500));
    EXPECT_FALSE(r.changed());
    EXPECT_EQ(r.indication, GestureIndication::InvalidDrop);
    EXPECT_EQ(vm.lastIndication(), GestureIndication::InvalidDrop);

    // Requirement 2.3: the clip is retained at its original position.
    const auto located = vm.locate(a.id);
    ASSERT_TRUE(located.has_value());
    const auto clip = vm.clipAt(located->first, located->second);
    ASSERT_TRUE(clip.has_value());
    EXPECT_EQ(clip->timelineStart, ms(0));
}

TEST(TimelineViewModel, MoveClipToNegativePositionIsInvalidDrop) {
    Project p = makeProject();
    Track t = makeTrack();
    Clip a = makeClip(1000, 1000);
    t.clips = {a};
    p.tracks.push_back(t);

    TimelineEngine engine(p);
    TimelineViewModel vm(engine);

    const GestureResult r = vm.moveClip(a.id, ms(-500));
    EXPECT_FALSE(r.changed());
    EXPECT_EQ(r.indication, GestureIndication::InvalidDrop);
}

TEST(TimelineViewModel, MoveUnknownClipIsRejectedNotInvalidDrop) {
    Project p = makeProjectWithTracks(1);
    TimelineEngine engine(p);
    TimelineViewModel vm(engine);

    const GestureResult r = vm.moveClip(Uuid::generateV4(), ms(0));
    EXPECT_FALSE(r.changed());
    EXPECT_EQ(r.indication, GestureIndication::Rejected);
}

// --- Trim gesture (Requirement 2.4) ----------------------------------------

TEST(TimelineViewModel, TrimEndUpdatesDurationToBoundary) {
    Project p = makeProject();
    Track t = makeTrack();
    Clip a = makeClip(0, 2000);  // source [0,2000)
    t.clips = {a};
    p.tracks.push_back(t);

    TimelineEngine engine(p);
    TimelineViewModel vm(engine);

    // Trim the out-point to 1500 ms; source is 2000 ms long.
    const GestureResult r = vm.trimClipEnd(a.id, ms(1500), FrameRate::fps30(), ms(2000));
    EXPECT_TRUE(r.changed());

    const auto clip = vm.clipAt(0, 0);
    ASSERT_TRUE(clip.has_value());
    EXPECT_EQ(clip->sourceOut, ms(1500));
    EXPECT_EQ(clip->duration, ms(1500));
}

// --- Split gesture (Requirements 2.5, 2.6) ---------------------------------

TEST(TimelineViewModel, SplitInsideClipCreatesTwoContiguousClips) {
    Project p = makeProject();
    Track t = makeTrack();
    Clip a = makeClip(0, 2000);
    t.clips = {a};
    p.tracks.push_back(t);

    TimelineEngine engine(p);
    TimelineViewModel vm(engine);

    const GestureResult r = vm.splitClip(a.id, ms(1200));
    EXPECT_TRUE(r.changed());
    EXPECT_EQ(r.indication, GestureIndication::Applied);
    ASSERT_EQ(r.addedClips.size(), 1u);  // the right-half id

    ASSERT_EQ(vm.clipCount(0), 2u);
    const auto left = vm.clipAt(0, 0);
    const auto right = vm.clipAt(0, 1);
    ASSERT_TRUE(left.has_value());
    ASSERT_TRUE(right.has_value());
    EXPECT_EQ(left->id, a.id);                 // left keeps the original id
    EXPECT_EQ(left->timelineEnd(), ms(1200));  // contiguous at the playhead
    EXPECT_EQ(right->timelineStart, ms(1200));
    EXPECT_EQ(right->id, r.addedClips.front());
    // Combined duration is conserved.
    EXPECT_EQ(left->duration + right->duration, ms(2000));
}

TEST(TimelineViewModel, SplitOutsideAnyClipReportsNothingToSplit) {
    Project p = makeProject();
    Track t = makeTrack();
    Clip a = makeClip(0, 1000);
    t.clips = {a};
    p.tracks.push_back(t);

    TimelineEngine engine(p);
    TimelineViewModel vm(engine);

    const GestureResult r = vm.splitClip(a.id, ms(5000));  // playhead past the clip
    EXPECT_FALSE(r.changed());
    EXPECT_EQ(r.indication, GestureIndication::NothingToSplit);
    EXPECT_EQ(vm.clipCount(0), 1u);  // unchanged (Requirement 2.6)
}

// --- Reorder gesture (Requirement 2.7) -------------------------------------

TEST(TimelineViewModel, ReorderPreservesClipCount) {
    Project p = makeProject();
    Track t = makeTrack();
    Clip a = makeClip(0, 1000);
    Clip b = makeClip(1000, 1000);
    Clip c = makeClip(2000, 1000);
    t.clips = {a, b, c};
    p.tracks.push_back(t);

    TimelineEngine engine(p);
    TimelineViewModel vm(engine);

    const GestureResult r = vm.reorderClips(t.id, {c.id, b.id, a.id});
    EXPECT_TRUE(r.changed());
    EXPECT_EQ(vm.clipCount(0), 3u);  // count preserved

    // Reordered, repacked contiguously from the earliest start (0).
    const auto first = vm.clipAt(0, 0);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->id, c.id);
    EXPECT_EQ(first->timelineStart, ms(0));
}

// --- Undo / redo (Requirements 2.9, 2.10) ----------------------------------

TEST(TimelineViewModel, UndoWithEmptyHistoryIsNoOp) {
    Project p = makeProjectWithTracks(1);
    TimelineEngine engine(p);
    TimelineViewModel vm(engine);

    EXPECT_FALSE(vm.canUndo());
    const GestureResult r = vm.undo();
    EXPECT_FALSE(r.changed());
    EXPECT_EQ(r.outcome, palmier::CommandOutcome::NoOp);
    EXPECT_EQ(r.indication, GestureIndication::NoOp);
}

TEST(TimelineViewModel, UndoThenRedoRoundTripsAMove) {
    Project p = makeProject();
    Track t = makeTrack();
    Clip a = makeClip(0, 1000);
    t.clips = {a};
    p.tracks.push_back(t);

    TimelineEngine engine(p);
    TimelineViewModel vm(engine);

    ASSERT_TRUE(vm.moveClip(a.id, ms(4000)).changed());
    EXPECT_EQ(vm.clipAt(0, 0)->timelineStart, ms(4000));

    ASSERT_TRUE(vm.canUndo());
    const GestureResult undo = vm.undo();
    EXPECT_TRUE(undo.changed());
    EXPECT_EQ(vm.clipAt(0, 0)->timelineStart, ms(0));  // restored

    ASSERT_TRUE(vm.canRedo());
    const GestureResult redo = vm.redo();
    EXPECT_TRUE(redo.changed());
    EXPECT_EQ(vm.clipAt(0, 0)->timelineStart, ms(4000));  // reproduced
}

// --- Change reflection -----------------------------------------------------

TEST(TimelineViewModel, ChangeListenerFiresOnEdit) {
    Project p = makeProject();
    Track t = makeTrack();
    Clip a = makeClip(0, 1000);
    t.clips = {a};
    p.tracks.push_back(t);

    TimelineEngine engine(p);
    TimelineViewModel vm(engine);

    int notifications = 0;
    vm.setChangeListener([&](const palmier::ChangeSet&) { ++notifications; });

    ASSERT_TRUE(vm.moveClip(a.id, ms(3000)).changed());
    EXPECT_EQ(notifications, 1);

    // A rejected gesture emits no ChangeSet (nothing changed).
    (void)vm.moveClip(Uuid::generateV4(), ms(0));
    EXPECT_EQ(notifications, 1);
}

// An edit issued directly on the engine (as the MCP server / agent would) is
// reflected into the adapter's cached view too.
TEST(TimelineViewModel, ReflectsEditsIssuedDirectlyOnTheEngine) {
    Project p = makeProject();
    Track t = makeTrack();
    Clip a = makeClip(0, 1000);
    t.clips = {a};
    p.tracks.push_back(t);

    TimelineEngine engine(p);
    TimelineViewModel vm(engine);

    const auto result = engine.apply(std::make_unique<palmier::MoveClipCommand>(a.id, ms(7000)));
    ASSERT_TRUE(result.changed());

    const auto clip = vm.clipAt(0, 0);
    ASSERT_TRUE(clip.has_value());
    EXPECT_EQ(clip->timelineStart, ms(7000));
}

}  // namespace
}  // namespace palmier::ui
