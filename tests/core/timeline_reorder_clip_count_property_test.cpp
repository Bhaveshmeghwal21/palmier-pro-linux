// SPDX-License-Identifier: GPL-3.0-or-later
//
// Property-based test for reorder clip-count conservation (task 3.7).
//
// Design property P9 (design.md "Correctness Properties"):
//
//     For any track and any reordering of its clips, the total number of clips
//     on that track is unchanged.
//
// This is the direct expression of Requirement 2.7: "WHEN a user reorders clips
// on a track, THE Timeline_Editor SHALL preserve the total count of clips on
// that track." Reordering only permutes (and repacks) the clips already present;
// it must never create or destroy clips — whether the requested order is a valid
// permutation (accepted) or a malformed one such as a wrong-length list, a list
// containing an unknown id, or one with a duplicate (rejected and rolled back).
// In every case the track's clip count after the attempt equals the count
// before it.
//
// Strategy: build a single-track timeline holding N contiguous, non-overlapping
// clips (a valid starting project for the engine), then drive an arbitrary
// sequence of ReorderClipsCommand attempts through a TimelineEngine. Each step
// generates one of several kinds of "new order":
//   * a genuine permutation of the current clip ids (accepted);
//   * the identity / reversed order (accepted);
//   * a list with an unknown id, a missing id, a duplicate, or an extra id
//     (rejected as a non-permutation).
// After EVERY reorder attempt — accepted or rejected — we assert the track still
// holds exactly N clips. Because reorder never changes the *set* of clip ids,
// the count stays fixed at its initial value for the whole sequence.
//
// _Requirements: 2.7_

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
#include "core/MediaAssetRef.hpp"
#include "core/Project.hpp"
#include "core/TimelineEngine.hpp"
#include "core/Track.hpp"
#include "core/Uuid.hpp"

namespace palmier {
namespace {

Duration ms(std::int64_t v) { return Duration::fromMilliseconds(v); }

// A project with a single video track holding `clipCount` contiguous,
// non-overlapping clips (so the engine accepts it as a valid initial state).
// The track id is returned so the generated reorder commands can target it.
Project makeProjectWithClips(std::size_t clipCount, Uuid& trackIdOut) {
    Project project;
    project.id = Uuid::generateV4();
    project.name = "prop9";

    Track track;
    track.id = Uuid::generateV4();
    track.kind = TrackKind::Video;

    Duration cursor = Duration::zero();
    for (std::size_t i = 0; i < clipCount; ++i) {
        Clip clip;
        clip.id = Uuid::generateV4();
        clip.assetRef = MediaAssetRef(Uuid::generateV4(), "mem://asset");
        clip.timelineStart = cursor;
        clip.sourceIn = ms(0);
        clip.sourceOut = ms(500);  // 500ms each
        track.clips.push_back(std::move(clip));
        cursor = cursor + ms(500);
    }

    trackIdOut = track.id;
    project.tracks.push_back(std::move(track));
    return project;
}

// The current clip ids of the single track, in track order.
std::vector<ClipId> currentClipIds(const Project& project) {
    std::vector<ClipId> ids;
    for (const Clip& clip : project.tracks[0].clips) {
        ids.push_back(clip.id);
    }
    return ids;
}

// Number of clips on the single track.
std::size_t trackClipCount(const Project& project) {
    return project.tracks[0].clips.size();
}

// Feature: palmier-pro-linux, Property 9: Reorder preserves clip count
// — any reordering of a track's clips (valid permutation or malformed request)
// leaves the track's total clip count unchanged.
// Validates: Requirements 2.7
RC_GTEST_PROP(TimelineReorderClipCountProperties,
              EveryReorderAttemptPreservesTrackClipCount,
              ()) {
    // A track with at least one clip; a modest upper bound keeps each generated
    // case fast while still exercising many distinct orderings.
    const std::size_t clipCount =
        static_cast<std::size_t>(*rc::gen::inRange(1, 8));

    Uuid trackId;
    TimelineEngine engine(makeProjectWithClips(clipCount, trackId));

    // The count we must conserve across every reorder attempt.
    const std::size_t expectedCount = clipCount;
    RC_ASSERT(trackClipCount(engine.snapshot()) == expectedCount);

    const int steps = *rc::gen::inRange(1, 30);

    for (int step = 0; step < steps; ++step) {
        const Project before = engine.snapshot();
        const std::vector<ClipId> ids = currentClipIds(before);

        // Build an arbitrary permutation of the current ids by stable-sorting
        // them under generated priorities (RapidCheck has no direct shuffle).
        std::vector<ClipId> permutation;
        {
            std::vector<std::pair<int, ClipId>> keyed;
            keyed.reserve(ids.size());
            for (const ClipId& id : ids) {
                keyed.emplace_back(*rc::gen::inRange(0, 1000), id);
            }
            std::stable_sort(keyed.begin(), keyed.end(),
                             [](const auto& a, const auto& b) { return a.first < b.first; });
            for (const auto& [key, id] : keyed) {
                permutation.push_back(id);
            }
        }

        // Choose the kind of reorder request. Kinds 0-2 are valid permutations
        // (accepted); kinds 3-6 are malformed (rejected as non-permutations).
        // Malformed variants that need at least one clip to construct are only
        // chosen when applicable; otherwise we fall back to the permutation.
        const int kind = *rc::gen::inRange(0, 7);
        std::vector<ClipId> newOrder;
        switch (kind) {
            case 0:  // arbitrary permutation
                newOrder = permutation;
                break;
            case 1:  // identity order
                newOrder = ids;
                break;
            case 2:  // reversed order
                newOrder.assign(ids.rbegin(), ids.rend());
                break;
            case 3:  // one id replaced by an unknown id -> not a permutation
                newOrder = permutation;
                newOrder[static_cast<std::size_t>(
                    *rc::gen::inRange(0, static_cast<int>(newOrder.size())))] =
                    Uuid::generateV4();
                break;
            case 4:  // a missing id (wrong length) -> not a permutation
                newOrder = permutation;
                if (newOrder.size() > 1) {
                    newOrder.pop_back();
                }
                break;
            case 5:  // a duplicated id (wrong multiset) -> not a permutation
                newOrder = permutation;
                newOrder.push_back(newOrder.front());
                break;
            case 6:  // an extra unknown id appended -> not a permutation
                newOrder = permutation;
                newOrder.push_back(Uuid::generateV4());
                break;
            default:
                newOrder = permutation;
                break;
        }

        (void)engine.apply(
            std::make_unique<ReorderClipsCommand>(trackId, std::move(newOrder)));

        // P9: whether the reorder was accepted or rejected and rolled back, the
        // track's clip count is unchanged.
        RC_ASSERT(trackClipCount(engine.snapshot()) == expectedCount);
    }
}

}  // namespace
}  // namespace palmier
