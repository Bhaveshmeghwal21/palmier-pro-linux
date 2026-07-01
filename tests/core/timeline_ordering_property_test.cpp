// SPDX-License-Identifier: GPL-3.0-or-later
//
// Property-based test for the track ordering and non-overlap invariant (task 3.5).
//
// Design property P3 (design.md "Correctness Properties"):
//
//     After any sequence of commands, every track's clips remain ordered by
//     `timelineStart` and non-overlapping outside explicit transition regions.
//
// This is the timeline-editing counterpart of Requirements 2.2 and 2.3:
//   * 2.2 — a clip dragged to a new *valid* position is moved there and the
//           project state updated (a successful move keeps the track valid); and
//   * 2.3 — a drop that would overlap an existing clip on the same track is
//           rejected and the clip retained at its original position (a rejected
//           edit leaves the track valid and unchanged).
// Together they mean: no matter what mix of edits the user performs, a track's
// clip list is always a sorted, non-overlapping sequence (the one exception
// being an incoming clip's explicit transition region, which is the only place
// adjacent clips are permitted to overlap — design.md Track validation).
//
// The mechanism under test is the TimelineEngine's invariant enforcement (task
// 3.2) driving the concrete editing commands (task 3.3): every apply() either
// commits an invariant-satisfying result or rolls back to the prior state, so
// the invariant holds after *every* command regardless of success or failure.
//
// Strategy: drive a TimelineEngine that starts with a small number of empty
// tracks through an arbitrary sequence of editing commands — add, move, trim,
// split, reorder, and delete — generating each command's parameters (target
// clip/track, positions, boundaries, permutations) from the current live state.
// After EACH applied command we independently verify, without appealing to the
// engine's own invariant routine, that every track's clips are ordered by
// timelineStart and do not overlap beyond the incoming clip's transition region.
//
// _Requirements: 2.2, 2.3_

#include "core/EditCommands.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include "core/Clip.hpp"
#include "core/Duration.hpp"
#include "core/FrameRate.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/Project.hpp"
#include "core/TimelineEngine.hpp"
#include "core/Track.hpp"
#include "core/Transition.hpp"
#include "core/Uuid.hpp"

namespace palmier {
namespace {

Duration ms(std::int64_t v) { return Duration::fromMilliseconds(v); }

// A project with `trackCount` empty video tracks; the track ids are returned so
// the generated commands can target them.
Project makeProjectWithTracks(std::size_t trackCount, std::vector<Uuid>& trackIdsOut) {
    Project project;
    project.id = Uuid::generateV4();
    project.name = "prop3";
    for (std::size_t i = 0; i < trackCount; ++i) {
        Track track;
        track.id = Uuid::generateV4();
        track.kind = TrackKind::Video;
        trackIdsOut.push_back(track.id);
        project.tracks.push_back(std::move(track));
    }
    return project;
}

// Every clip id currently present anywhere in the project (in track/clip order).
std::vector<ClipId> allClipIds(const Project& project) {
    std::vector<ClipId> ids;
    for (const Track& track : project.tracks) {
        for (const Clip& clip : track.clips) {
            ids.push_back(clip.id);
        }
    }
    return ids;
}

// The independent oracle for property P3: assert that one track's clips are
// ordered by timelineStart and do not overlap beyond the incoming (later) clip's
// explicit transition region. This deliberately re-derives the check from the
// clip geometry rather than calling the engine's checkTimelineInvariants, so the
// test genuinely validates the observable invariant instead of restating the
// engine's own code.
void assertTrackOrderedAndNonOverlapping(const Track& track) {
    const std::vector<Clip>& clips = track.clips;
    for (std::size_t i = 1; i < clips.size(); ++i) {
        const Clip& previous = clips[i - 1];
        const Clip& current = clips[i];

        // Ordered by timelineStart.
        RC_ASSERT(current.timelineStart >= previous.timelineStart);

        // Non-overlapping outside the incoming clip's transition region: the
        // only permitted overlap is at most the length of current.transitionIn.
        const Duration allowedOverlap = current.transitionIn.has_value()
                                            ? current.transitionIn->duration
                                            : Duration::zero();
        const Duration overlap = previous.timelineEnd() - current.timelineStart;
        RC_ASSERT(overlap <= allowedOverlap);
    }
}

void assertProjectInvariant(const Project& project) {
    for (const Track& track : project.tracks) {
        assertTrackOrderedAndNonOverlapping(track);
    }
}

// Feature: palmier-pro-linux, Property 3: Track ordering and non-overlap invariant
// — after any command sequence, every track's clips remain ordered by
// timelineStart and non-overlapping outside transitions.
// Validates: Requirements 2.2, 2.3
RC_GTEST_PROP(TimelineOrderingProperties,
              EveryCommandSequenceKeepsTracksOrderedAndNonOverlapping,
              ()) {
    // A modest multi-track timeline (Requirement 2.1 permits 1-50 tracks); a
    // handful is enough to exercise cross-track and within-track behaviour while
    // keeping each generated case fast.
    const std::size_t trackCount = static_cast<std::size_t>(*rc::gen::inRange(1, 4));
    std::vector<Uuid> trackIds;
    TimelineEngine engine(makeProjectWithTracks(trackCount, trackIds));

    // The engine starts valid (empty tracks); confirm before any edits.
    assertProjectInvariant(engine.snapshot());

    // An arbitrary-length sequence of editing commands.
    const int steps = *rc::gen::inRange(1, 40);

    for (int step = 0; step < steps; ++step) {
        const Project before = engine.snapshot();
        const std::vector<ClipId> clipIds = allClipIds(before);

        // Choose an operation. AddClip is always available; the clip-targeting
        // operations are only meaningful once at least one clip exists, so when
        // the timeline is empty we force an add.
        const int op = clipIds.empty() ? 0 : *rc::gen::inRange(0, 6);

        switch (op) {
            case 0: {  // AddClip — place a fresh clip on a random track.
                const std::size_t trackIdx =
                    static_cast<std::size_t>(*rc::gen::inRange(
                        0, static_cast<int>(trackIds.size())));
                Clip clip;
                clip.id = Uuid::generateV4();
                clip.assetRef = MediaAssetRef(Uuid::generateV4(), "mem://asset");
                clip.timelineStart = ms(*rc::gen::inRange(0, 6000));
                clip.sourceIn = ms(0);
                clip.sourceOut = ms(*rc::gen::inRange(1, 2000));
                // Occasionally attach an incoming transition so the permitted
                // overlap region is exercised (and the oracle's transition
                // allowance is meaningful).
                if (*rc::gen::inRange(0, 4) == 0) {
                    clip.transitionIn = Transition(
                        Uuid::generateV4(), TransitionKind::Crossfade,
                        ms(*rc::gen::inRange(0, 400)));
                }
                (void)engine.apply(std::make_unique<AddClipCommand>(trackIds[trackIdx],
                                                                    std::move(clip)));
                break;
            }
            case 1: {  // MoveClip — reposition an existing clip.
                const ClipId target = clipIds[static_cast<std::size_t>(
                    *rc::gen::inRange(0, static_cast<int>(clipIds.size())))];
                const Duration newStart = ms(*rc::gen::inRange(0, 6000));
                (void)engine.apply(std::make_unique<MoveClipCommand>(target, newStart));
                break;
            }
            case 2: {  // TrimClip — trim a random edge of an existing clip.
                const ClipId target = clipIds[static_cast<std::size_t>(
                    *rc::gen::inRange(0, static_cast<int>(clipIds.size())))];
                const auto edge = (*rc::gen::inRange(0, 2) == 0)
                                      ? TrimClipCommand::Edge::Start
                                      : TrimClipCommand::Edge::End;
                const Duration boundary = ms(*rc::gen::inRange(0, 4000));
                const Duration sourceDuration = ms(*rc::gen::inRange(1, 8000));
                (void)engine.apply(std::make_unique<TrimClipCommand>(
                    target, edge, boundary, FrameRate::fps24(), sourceDuration));
                break;
            }
            case 3: {  // SplitClip — split an existing clip at a random playhead.
                const ClipId target = clipIds[static_cast<std::size_t>(
                    *rc::gen::inRange(0, static_cast<int>(clipIds.size())))];
                const Duration playhead = ms(*rc::gen::inRange(0, 8000));
                (void)engine.apply(std::make_unique<SplitClipCommand>(target, playhead));
                break;
            }
            case 4: {  // ReorderClips — apply an arbitrary permutation to a track.
                const std::size_t trackIdx =
                    static_cast<std::size_t>(*rc::gen::inRange(
                        0, static_cast<int>(before.tracks.size())));
                const Track& track = before.tracks[trackIdx];
                if (track.clips.empty()) {
                    break;  // nothing to reorder on this track
                }
                // Build an arbitrary permutation of the track's clip ids by
                // sorting them under generated priorities.
                std::vector<std::pair<int, ClipId>> keyed;
                keyed.reserve(track.clips.size());
                for (const Clip& clip : track.clips) {
                    keyed.emplace_back(*rc::gen::inRange(0, 1000), clip.id);
                }
                std::stable_sort(keyed.begin(), keyed.end(),
                                 [](const auto& a, const auto& b) { return a.first < b.first; });
                std::vector<ClipId> newOrder;
                newOrder.reserve(keyed.size());
                for (const auto& [key, id] : keyed) {
                    newOrder.push_back(id);
                }
                (void)engine.apply(std::make_unique<ReorderClipsCommand>(
                    track.id, std::move(newOrder)));
                break;
            }
            case 5: {  // DeleteClip — remove an existing clip.
                const ClipId target = clipIds[static_cast<std::size_t>(
                    *rc::gen::inRange(0, static_cast<int>(clipIds.size())))];
                (void)engine.apply(std::make_unique<DeleteClipCommand>(target));
                break;
            }
            default:
                break;
        }

        // P3: after every command — whether it committed or was rejected and
        // rolled back — every track stays ordered and non-overlapping.
        assertProjectInvariant(engine.snapshot());
    }
}

}  // namespace
}  // namespace palmier
