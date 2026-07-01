// SPDX-License-Identifier: GPL-3.0-or-later
//
// Property-based test for split contiguity and duration conservation (task 3.6).
//
// Design property P8 (design.md "Correctness Properties"):
//
//     For any clip and any interior playhead position, splitting the clip
//     produces two contiguous, non-overlapping clips whose combined duration
//     equals the original clip duration and whose combined source range equals
//     the original source range.
//
// This is the timeline-editing guarantee of Requirement 2.5 ("split a clip at
// the playhead ... into two independent clips at that boundary"). The
// SplitClipCommand is implemented in core/EditCommands.cpp (task 3.3) and driven
// through the TimelineEngine (task 3.2); this file adds the dedicated RapidCheck
// property that exercises the split across arbitrary clips and interior playhead
// positions.
//
// Strategy: build a single-track project seeded with ONE generated clip whose
// geometry (timelineStart, sourceIn, and length) is expressed in whole frames so
// interior frame-aligned playheads are exact. Generate an interior playhead
// strictly between the clip's start and end, apply the split through a
// TimelineEngine, and then independently verify — from the resulting clip
// geometry, not by appealing to the command's own arithmetic — that:
//   * the split committed and produced exactly two clips;
//   * the two clips are CONTIGUOUS: the right half begins exactly where the left
//     half ends on the timeline (right.timelineStart == left.timelineEnd());
//   * the two clips are NON-OVERLAPPING and leave no gap (the contiguity above,
//     plus ordered starts);
//   * DURATION is conserved: left.duration() + right.duration() equals the
//     original clip's timeline duration, and the block still spans exactly
//     [originalStart, originalEnd);
//   * the SOURCE RANGE is conserved: the left half keeps the original sourceIn,
//     the right half keeps the original sourceOut, the cut is contiguous in
//     source (left.sourceOut == right.sourceIn), and the two source spans sum to
//     the original source span.
//
// _Requirements: 2.5_

#include "core/EditCommands.hpp"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include "core/Clip.hpp"
#include "core/CommandResult.hpp"
#include "core/Duration.hpp"
#include "core/FrameRate.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/Project.hpp"
#include "core/TimelineEngine.hpp"
#include "core/Track.hpp"
#include "core/Uuid.hpp"

namespace palmier {
namespace {

// A single-track project seeded with `clip`; the clip is the only content, so
// the split has an unambiguous target and the engine starts from a valid state.
Project makeProjectWithClip(const Clip& clip) {
    Project project;
    project.id = Uuid::generateV4();
    project.name = "prop8";
    project.timelineFps = FrameRate::fps30();

    Track track;
    track.id = Uuid::generateV4();
    track.kind = TrackKind::Video;
    track.clips.push_back(clip);
    project.tracks.push_back(std::move(track));
    return project;
}

// Feature: palmier-pro-linux, Property 8: Split contiguity and duration
// conservation — splitting at an interior playhead yields two contiguous,
// non-overlapping clips whose combined duration and source range equal the
// original.
// Validates: Requirements 2.5
RC_GTEST_PROP(TimelineSplitProperties,
              InteriorSplitIsContiguousAndConservesDurationAndSource,
              ()) {
    const FrameRate fps = FrameRate::fps30();

    // Clip geometry in whole frames so a frame-aligned interior playhead is
    // exact (no rounding when converting frames <-> Duration). A length of at
    // least two frames guarantees an interior cut exists.
    const std::int64_t startFrames = *rc::gen::inRange<std::int64_t>(0, 1000);
    const std::int64_t inFrames = *rc::gen::inRange<std::int64_t>(0, 500);
    const std::int64_t lengthFrames = *rc::gen::inRange<std::int64_t>(2, 2000);
    // Split offset strictly inside the clip: 1 .. lengthFrames-1 frames in.
    const std::int64_t splitFrames =
        *rc::gen::inRange<std::int64_t>(1, lengthFrames);  // inRange upper is exclusive

    Clip clip;
    clip.id = Uuid::generateV4();
    clip.assetRef = MediaAssetRef(Uuid::generateV4(), "mem://asset");
    clip.timelineStart = fps.durationForFrames(startFrames);
    clip.sourceIn = fps.durationForFrames(inFrames);
    clip.sourceOut = clip.sourceIn + fps.durationForFrames(lengthFrames);

    // Capture the original geometry before the split for the conservation checks.
    const ClipId originalId = clip.id;
    const Duration originalStart = clip.timelineStart;
    const Duration originalEnd = clip.timelineEnd();
    const Duration originalDuration = clip.duration();
    const Duration originalSourceIn = clip.sourceIn;
    const Duration originalSourceOut = clip.sourceOut;

    const Duration playhead = originalStart + fps.durationForFrames(splitFrames);
    // The playhead is strictly interior by construction; assert to be explicit.
    RC_ASSERT(playhead > originalStart);
    RC_ASSERT(playhead < originalEnd);

    TimelineEngine engine(makeProjectWithClip(clip));
    const CommandResult applied =
        engine.apply(std::make_unique<SplitClipCommand>(originalId, playhead));

    // An interior split must commit (Requirement 2.5).
    RC_ASSERT(applied.changed());

    const Project after = engine.snapshot();
    RC_ASSERT(after.tracks.size() == 1u);
    const std::vector<Clip>& clips = after.tracks[0].clips;

    // Exactly two clips result from splitting the single original.
    RC_ASSERT(clips.size() == 2u);
    const Clip& left = clips[0];
    const Clip& right = clips[1];

    // Contiguity: the right half starts exactly where the left half ends, so the
    // two clips abut with neither gap nor overlap on the timeline.
    RC_ASSERT(right.timelineStart == left.timelineEnd());

    // Non-overlapping / ordered: left starts before right on the timeline.
    RC_ASSERT(left.timelineStart < right.timelineStart);

    // The split occurs exactly at the playhead.
    RC_ASSERT(right.timelineStart == playhead);

    // The block still spans exactly the original extent on the timeline.
    RC_ASSERT(left.timelineStart == originalStart);
    RC_ASSERT(right.timelineEnd() == originalEnd);

    // Duration conservation: the two halves' durations sum to the original's,
    // and each half is strictly positive (a real, non-degenerate split).
    RC_ASSERT(left.duration().isPositive());
    RC_ASSERT(right.duration().isPositive());
    RC_ASSERT(left.duration() + right.duration() == originalDuration);

    // Source-range conservation: the left half keeps the original in-point, the
    // right half keeps the original out-point, the cut is contiguous in source
    // (no source frames dropped or duplicated), and the two source spans sum to
    // the original source span.
    RC_ASSERT(left.sourceIn == originalSourceIn);
    RC_ASSERT(right.sourceOut == originalSourceOut);
    RC_ASSERT(left.sourceOut == right.sourceIn);
    const Duration leftSourceSpan = left.sourceOut - left.sourceIn;
    const Duration rightSourceSpan = right.sourceOut - right.sourceIn;
    RC_ASSERT(leftSourceSpan + rightSourceSpan == (originalSourceOut - originalSourceIn));

    // The left half retains the original clip's identity (Requirement 2.5: the
    // original clip is split in place); the right half is a fresh, distinct clip.
    RC_ASSERT(left.id == originalId);
    RC_ASSERT(right.id != originalId);
}

}  // namespace
}  // namespace palmier
