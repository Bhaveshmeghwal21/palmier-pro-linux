// SPDX-License-Identifier: GPL-3.0-or-later
//
// Property-based test for trim adjusting a clip's duration to the new boundary
// (task 3.8).
//
// Design property P10 (design.md "Correctness Properties"):
//
//     For any clip and any valid trim boundary, after trimming the clip's
//     duration equals `sourceOut - sourceIn` for the new boundary.
//
// This is the timeline-editing counterpart of Requirement 2.4: when a user trims
// the start or end of a clip, the Timeline_Editor updates the clip duration to
// the trimmed boundary, constrained to a minimum clip duration of one frame and a
// maximum equal to the clip's original source duration.
//
// The mechanism under test is TrimClipCommand (task 3.3): trimming the End edge
// moves the out-point (sourceOut) to the requested boundary with timelineStart
// fixed; trimming the Start edge moves the in-point (sourceIn) and shifts
// timelineStart by the same delta so the retained content keeps its timeline
// position. Both edges keep the resulting duration in [1 frame, sourceDuration].
//
// Strategy: for a single clip on a single track, generate a *valid* new boundary
// for a randomly chosen edge, expressed in whole frames so it lands squarely
// inside the permitted [1 frame, sourceDuration] band (no clamping needed — the
// clamp is exercised separately by the edit-command unit tests). After applying
// the trim we assert, independently of the command's internals, that:
//   * the trim succeeded;
//   * the moved edge landed exactly on the requested boundary and the opposite
//     edge is unchanged;
//   * the resulting clip duration equals `sourceOut - sourceIn` for the new
//     boundary (the property);
//   * that duration stays within [1 frame, sourceDuration] (the 2.4 constraint);
//   * a Start trim shifts timelineStart by the in-point delta (End leaves it
//     fixed), so the retained content keeps its place on the timeline.
//
// _Requirements: 2.4_

#include "core/EditCommands.hpp"

#include <cstdint>

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include "core/Clip.hpp"
#include "core/Duration.hpp"
#include "core/FrameRate.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/Project.hpp"
#include "core/Track.hpp"
#include "core/Uuid.hpp"

namespace palmier {
namespace {

// A project with a single video track holding exactly `clip`, so a trim can be
// applied and observed in isolation. The track id is irrelevant here.
Project makeProjectWithClip(const Clip& clip) {
    Project project;
    project.id = Uuid::generateV4();
    project.name = "prop10";
    Track track;
    track.id = Uuid::generateV4();
    track.kind = TrackKind::Video;
    track.clips.push_back(clip);
    project.tracks.push_back(std::move(track));
    return project;
}

// Feature: palmier-pro-linux, Property 10: Trim adjusts duration to boundary
// — after a valid trim the clip duration equals `sourceOut - sourceIn` for the
// new boundary, constrained to [1 frame, source duration].
// Validates: Requirements 2.4
RC_GTEST_PROP(TimelineTrimProperties,
              ValidTrimSetsDurationToNewBoundary,
              ()) {
    // Exact integer frame rates keep whole-frame boundaries representable with no
    // rounding, so generated boundaries land inside the permitted band exactly.
    const int fpsChoice = *rc::gen::element(24, 25, 30, 50, 60);
    const FrameRate fps(fpsChoice, 1);
    const Duration minFrame = fps.frameDuration();

    // Source media at least two frames long so a one-frame-minimum trim always
    // fits; the clip's initial range is any sub-range of at least one frame.
    const int totalFrames = *rc::gen::inRange(2, 4001);
    const Duration sourceDuration = fps.durationForFrames(totalFrames);

    const int sourceInFrames = *rc::gen::inRange(0, totalFrames - 1);       // [0, totalFrames-2]
    const int sourceOutFrames =
        *rc::gen::inRange(sourceInFrames + 1, totalFrames + 1);             // [in+1, totalFrames]
    const int startFrames = *rc::gen::inRange(0, 1001);

    Clip clip;
    clip.id = Uuid::generateV4();
    clip.assetRef = MediaAssetRef(Uuid::generateV4(), "mem://asset");
    clip.timelineStart = fps.durationForFrames(startFrames);
    clip.sourceIn = fps.durationForFrames(sourceInFrames);
    clip.sourceOut = fps.durationForFrames(sourceOutFrames);

    const Duration origIn = clip.sourceIn;
    const Duration origOut = clip.sourceOut;
    const Duration origStart = clip.timelineStart;

    const bool trimEnd = *rc::gen::arbitrary<bool>();

    // The new boundary (in whole frames) and the resulting expected edges.
    TrimClipCommand::Edge edge = TrimClipCommand::Edge::End;
    Duration newBoundary = Duration::zero();
    Duration expectedIn = origIn;
    Duration expectedOut = origOut;

    if (trimEnd) {
        // New out-point in [sourceIn + 1 frame, sourceDuration]; start unchanged.
        const int newOutFrames =
            *rc::gen::inRange(sourceInFrames + 1, totalFrames + 1);         // [in+1, totalFrames]
        edge = TrimClipCommand::Edge::End;
        newBoundary = fps.durationForFrames(newOutFrames);
        expectedIn = origIn;
        expectedOut = newBoundary;
    } else {
        // New in-point in [0, sourceOut - 1 frame]; the clip shifts by the delta.
        const int newInFrames = *rc::gen::inRange(0, sourceOutFrames);      // [0, out-1]
        edge = TrimClipCommand::Edge::Start;
        newBoundary = fps.durationForFrames(newInFrames);
        expectedIn = newBoundary;
        expectedOut = origOut;
    }

    Project project = makeProjectWithClip(clip);
    TrimClipCommand cmd(clip.id, edge, newBoundary, fps, sourceDuration);
    const auto result = cmd.apply(project);

    // A boundary generated inside the permitted band must be accepted.
    RC_ASSERT(result.isOk());

    const Clip& trimmed = project.tracks[0].clips[0];

    // The moved edge lands exactly on the requested boundary; the other is fixed.
    RC_ASSERT(trimmed.sourceIn == expectedIn);
    RC_ASSERT(trimmed.sourceOut == expectedOut);

    // Property 10: the clip duration equals sourceOut - sourceIn for the new
    // boundary.
    const Duration expectedDuration = expectedOut - expectedIn;
    RC_ASSERT(trimmed.duration() == expectedDuration);
    RC_ASSERT(trimmed.sourceOut - trimmed.sourceIn == expectedDuration);

    // Requirement 2.4 constraint: duration stays within [1 frame, sourceDuration].
    RC_ASSERT(trimmed.duration() >= minFrame);
    RC_ASSERT(trimmed.duration() <= sourceDuration);

    // Retained content keeps its timeline position: an End trim leaves
    // timelineStart fixed; a Start trim shifts it by the in-point delta (never
    // below zero).
    if (edge == TrimClipCommand::Edge::End) {
        RC_ASSERT(trimmed.timelineStart == origStart);
    } else {
        Duration expectedStart = origStart + (expectedIn - origIn);
        if (expectedStart.isNegative()) {
            expectedStart = Duration::zero();
        }
        RC_ASSERT(trimmed.timelineStart == expectedStart);
    }
}

}  // namespace
}  // namespace palmier
