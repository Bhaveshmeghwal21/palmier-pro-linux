// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/services/timeline_track_tools_property_test.cpp — the universally
// quantified behaviour of the two track tools stage 4 added to the shared tool
// surface (task 4.7).
//
// Two properties live here, one per tool:
//
//   Property 9  — `timeline.add_track` appends after the last track of its kind,
//                 leaves every pre-existing track and clip untouched, and mints
//                 an identifier unique within the project (Requirement 3.3).
//   Property 15 — `timeline.remove_track` drops the named track and every clip
//                 on it, preserves the relative order of the remaining tracks,
//                 and reports the resulting track and clip counts
//                 (Requirement 3.10).
//
// Both properties drive the REAL tool surface (`buildDefaultToolRegistry` over a
// real `services::ProjectSession`), so what they exercise is the whole path a
// GUI, MCP or agent invocation takes: argument parsing, the core
// AddTrackCommand / RemoveTrackCommand, and TimelineEngine::apply with its
// invariant enforcement and undo recording.
//
// How a generated project becomes the current project
// ---------------------------------------------------
// Through `TimelineEngine::reset`, which is the same commit path
// `ProjectSession::openProject` uses. reset() validates the project and enforces
// the timeline invariants, so every generated project here is a LEGAL one: a
// positive frame rate and canvas, a supported schema version, non-nil unique
// identifiers (`Uuid::generateV4`), every clip's `assetRef` resolving in the
// project's asset table, and each track's clips ordered by `timelineStart` and
// laid out with a positive gap so no two ever overlap.
//
// Cost control: Property 15's stated bounds allow 20 tracks x 50 clips = 1000
// clips per case, which no 100-case run needs to pay for on every case. The
// generator draws a shape profile instead — mostly small projects, with a
// deliberate minority at each stated extreme (20 tracks; a 50-clip track).
//
// _Requirements: 3.3, 3.10_

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/Clip.hpp"
#include "core/Duration.hpp"
#include "core/Effect.hpp"
#include "core/FrameRate.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/Project.hpp"
#include "core/Resolution.hpp"
#include "core/Result.hpp"
#include "core/Track.hpp"
#include "core/Transition.hpp"
#include "core/Uuid.hpp"
#include "services/Json.hpp"
#include "services/ProjectSession.hpp"
#include "services/ToolRegistry.hpp"

namespace palmier::services {
namespace {

// ---------------------------------------------------------------------------
// Tool-call helpers
// ---------------------------------------------------------------------------

[[nodiscard]] const char* kindName(TrackKind kind) {
    return kind == TrackKind::Video ? "video" : "audio";
}

[[nodiscard]] Json kindArgs(TrackKind kind) {
    Json args = Json::object();
    args.set("kind", std::string(kindName(kind)));
    return args;
}

[[nodiscard]] Json trackIdArgs(const Uuid& trackId) {
    Json args = Json::object();
    args.set("trackId", trackId.toString());
    return args;
}

[[nodiscard]] std::size_t totalClipCount(const Project& project) {
    std::size_t count = 0;
    for (const Track& track : project.tracks) count += track.clips.size();
    return count;
}

// ---------------------------------------------------------------------------
// Generators. Imperative style (RapidCheck's `*gen` draw operator) so a case
// shrinks and replays as one unit.
//
// Identities are always drawn with Uuid::generateV4() rather than byte-wise:
// a shrunk byte draw collapses to the nil UUID and to duplicates, which the
// engine's validation rejects — the generated project would then be illegal
// rather than a smaller counterexample.
// ---------------------------------------------------------------------------

/// The shape envelope one generated project is drawn inside.
struct Shape {
    int maxTracks = 20;         ///< Property 9: 0-20. Property 15: 1-20.
    int minTracks = 0;
    int maxClipsPerTrack = 50;  ///< Property 15 bound: 0-50 clips per track.
    int clipBudget = 120;       ///< Total clips across all tracks.
};

/// Draw this case's profile: mostly small projects, with a deliberate minority
/// at the upper end of each stated bound so the extremes are covered too.
Shape drawShape(int minTracks) {
    Shape shape;
    shape.minTracks = minTracks;
    switch (*rc::gen::inRange(0, 6)) {
        case 0:  // Maximal track count.
            shape.maxTracks = 20;
            shape.maxClipsPerTrack = 3;
            shape.clipBudget = 60;
            break;
        case 1:  // A single maximal-length track.
            shape.maxTracks = 2;
            shape.maxClipsPerTrack = 50;
            shape.clipBudget = 100;
            break;
        default:  // The common, small case.
            shape.maxTracks = 6;
            shape.maxClipsPerTrack = 6;
            shape.clipBudget = 24;
            break;
    }
    if (shape.maxTracks < shape.minTracks) shape.maxTracks = shape.minTracks;
    return shape;
}

/// One project with interleaved video/audio tracks. Clips are laid out
/// sequentially with a positive gap and carry no transition, so every track
/// satisfies the engine's ordering and non-overlap invariants, and every
/// `assetRef` resolves in the asset table.
Project drawProject(const Shape& shape) {
    Project project;
    project.id = Uuid::generateV4();
    project.name = "track-tool-property";
    project.timelineFps = FrameRate::fps30();
    project.canvas = Resolution::hd1080();

    const int assetCount = *rc::gen::inRange(1, 4);
    for (int a = 0; a < assetCount; ++a) {
        project.assets.push_back(
            MediaAssetRef(Uuid::generateV4(),
                          "/media/source" + std::to_string(a) + ".mp4"));
    }

    int clipsRemaining = shape.clipBudget;
    const int trackCount = *rc::gen::inRange(shape.minTracks, shape.maxTracks + 1);
    for (int t = 0; t < trackCount; ++t) {
        Track track;
        track.id = Uuid::generateV4();
        track.kind = *rc::gen::element<TrackKind>(TrackKind::Video, TrackKind::Audio);
        track.name = "track " + std::to_string(t);
        track.muted = *rc::gen::arbitrary<bool>();
        track.locked = *rc::gen::arbitrary<bool>();

        const int wanted = *rc::gen::inRange(0, shape.maxClipsPerTrack + 1);
        const int clipCount = wanted < clipsRemaining ? wanted : (clipsRemaining > 0 ? clipsRemaining : 0);
        clipsRemaining -= clipCount;

        std::int64_t cursorMs = *rc::gen::inRange<std::int64_t>(0, 1'001);
        for (int c = 0; c < clipCount; ++c) {
            Clip clip;
            clip.id = Uuid::generateV4();
            clip.assetRef =
                project.assets[*rc::gen::inRange<std::size_t>(0, project.assets.size())];

            const std::int64_t sourceInMs = *rc::gen::inRange<std::int64_t>(0, 60'001);
            const std::int64_t lengthMs = *rc::gen::inRange<std::int64_t>(20, 5'001);
            clip.sourceIn = Duration::fromMilliseconds(sourceInMs);
            clip.sourceOut = Duration::fromMilliseconds(sourceInMs + lengthMs);
            clip.timelineStart = Duration::fromMilliseconds(cursorMs);
            // A strictly positive gap to the next clip: ordered, never overlapping.
            cursorMs += lengthMs + *rc::gen::inRange<std::int64_t>(1, 501);

            clip.gain = static_cast<double>(*rc::gen::inRange(0, 2'001)) / 1'000.0;
            clip.opacity = static_cast<double>(*rc::gen::inRange(0, 1'001)) / 1'000.0;

            track.clips.push_back(std::move(clip));
        }

        project.tracks.push_back(std::move(track));
    }

    return project;
}

// ---------------------------------------------------------------------------
// Comparison helpers — "compares equal before and after" for a track and its
// clips.
// ---------------------------------------------------------------------------

void assertSameClip(const Clip& a, const Clip& b) {
    RC_ASSERT(a.id == b.id);
    RC_ASSERT(a.assetRef.assetId == b.assetRef.assetId);
    RC_ASSERT(a.assetRef.sourcePath == b.assetRef.sourcePath);
    RC_ASSERT(a.timelineStart.ticks() == b.timelineStart.ticks());
    RC_ASSERT(a.sourceIn.ticks() == b.sourceIn.ticks());
    RC_ASSERT(a.sourceOut.ticks() == b.sourceOut.ticks());
    RC_ASSERT(a.gain == b.gain);
    RC_ASSERT(a.opacity == b.opacity);
    RC_ASSERT(a.effects.size() == b.effects.size());
    for (std::size_t i = 0; i < a.effects.size(); ++i) {
        RC_ASSERT(a.effects[i].id == b.effects[i].id);
        RC_ASSERT(a.effects[i].type == b.effects[i].type);
        RC_ASSERT(a.effects[i].parameters == b.effects[i].parameters);
    }
    RC_ASSERT(a.transitionIn.has_value() == b.transitionIn.has_value());
    if (a.transitionIn.has_value()) {
        RC_ASSERT(a.transitionIn->id == b.transitionIn->id);
        RC_ASSERT(a.transitionIn->kind == b.transitionIn->kind);
        RC_ASSERT(a.transitionIn->duration.ticks() == b.transitionIn->duration.ticks());
    }
}

void assertSameTrack(const Track& a, const Track& b) {
    RC_ASSERT(a.id == b.id);
    RC_ASSERT(a.kind == b.kind);
    RC_ASSERT(a.name == b.name);
    RC_ASSERT(a.muted == b.muted);
    RC_ASSERT(a.locked == b.locked);
    RC_ASSERT(a.clips.size() == b.clips.size());
    for (std::size_t c = 0; c < a.clips.size(); ++c) {
        assertSameClip(a.clips[c], b.clips[c]);
    }
}

/// Every track identifier in `project` is distinct.
void assertTrackIdentifiersAreUnique(const Project& project) {
    std::unordered_set<Uuid> seen;
    for (const Track& track : project.tracks) {
        RC_ASSERT(!track.id.isNil());
        RC_ASSERT(seen.insert(track.id).second);
    }
}

/// The index the tool must insert a track of `kind` at: immediately after the
/// last existing track of that kind, or at the end when the project holds none.
[[nodiscard]] std::size_t expectedInsertIndex(const Project& project, TrackKind kind) {
    std::size_t index = project.tracks.size();
    for (std::size_t i = 0; i < project.tracks.size(); ++i) {
        if (project.tracks[i].kind == kind) index = i + 1;
    }
    return index;
}

// ---------------------------------------------------------------------------
// Feature: end-to-end-editor-integration, Property 9: add_track appends after
// the last track of its kind — for any project and any sequence of
// `timeline.add_track` calls, each new track is positioned immediately after the
// last pre-existing track of the same kind, every pre-existing track and clip
// compares equal before and after, and each returned track identifier is unique
// within the project.
//
// Requirement 3.3: "WHEN `timeline.add_track` is invoked with a track kind of
// `video` or `audio` while the current project holds fewer than 64 tracks of
// that kind, THE Project_Session SHALL append the new track after the last
// existing track of that kind, leave all existing tracks and clips unchanged,
// and return a track identifier unique within the project."
//
// **Validates: Requirements 3.3**
// ---------------------------------------------------------------------------
RC_GTEST_PROP(TimelineTrackToolProperties,
              AddTrackAppendsAfterTheLastTrackOfItsKind,
              ()) {
    // 0-20 interleaved video/audio tracks to start from.
    const Project initial = drawProject(drawShape(0));

    ProjectSession session;
    RC_ASSERT(session.engine().reset(initial).isOk());
    const ToolRegistry registry = buildDefaultToolRegistry(session);

    // A sequence of 1-20 `video`/`audio` kinds. With at most 20 starting tracks
    // and at most 20 additions, the 64-per-kind cap is never reached, so every
    // call in the sequence is expected to succeed.
    const int callCount = *rc::gen::inRange(1, 21);
    std::unordered_set<Uuid> returnedIds;

    for (int call = 0; call < callCount; ++call) {
        const TrackKind kind =
            *rc::gen::element<TrackKind>(TrackKind::Video, TrackKind::Audio);

        const Project     before = session.engine().snapshot();
        const std::size_t expectedIndex = expectedInsertIndex(before, kind);

        const Result<Json> invoked = registry.invoke("timeline.add_track", kindArgs(kind));
        RC_ASSERT(invoked.isOk());
        const Json& out = invoked.value();

        // The reported placement, kind and resulting count.
        RC_ASSERT(out.stringOr("kind") == std::string(kindName(kind)));
        RC_ASSERT(out.intOr("index") == static_cast<std::int64_t>(expectedIndex));
        RC_ASSERT(out.intOr("trackCount") ==
                  static_cast<std::int64_t>(before.tracks.size() + 1));

        const Project after = session.engine().snapshot();
        RC_ASSERT(after.tracks.size() == before.tracks.size() + 1);

        // The new track sits immediately after the last pre-existing track of
        // its kind, carries the returned identifier and holds no clips.
        const Track& added = after.tracks[expectedIndex];
        RC_ASSERT(added.id.toString() == out.stringOr("trackId"));
        RC_ASSERT(added.kind == kind);
        RC_ASSERT(added.clips.empty());

        // Every pre-existing track — and every clip on it — compares equal, in
        // its original relative order, with the inserted track excised.
        std::size_t source = 0;
        for (std::size_t i = 0; i < after.tracks.size(); ++i) {
            if (i == expectedIndex) continue;
            assertSameTrack(before.tracks[source], after.tracks[i]);
            ++source;
        }
        RC_ASSERT(source == before.tracks.size());

        // The returned identifier is unique within the project, and distinct
        // from every identifier the sequence has returned so far.
        assertTrackIdentifiersAreUnique(after);
        RC_ASSERT(returnedIds.insert(added.id).second);
    }
}

// ---------------------------------------------------------------------------
// Feature: end-to-end-editor-integration, Property 15: remove_track preserves
// the order of remaining tracks — for any project and any track identifier
// present in it, `timeline.remove_track` removes that track and every clip on
// it, leaves the remaining tracks in their original relative order, and returns
// the resulting track and clip counts.
//
// Requirement 3.10: "WHEN `timeline.remove_track` is invoked with a track
// identifier present in the current project, THE Project_Session SHALL remove
// that track and every clip on it, preserve the relative order of the remaining
// tracks, and return the resulting track count and clip count."
//
// **Validates: Requirements 3.10**
// ---------------------------------------------------------------------------
RC_GTEST_PROP(TimelineTrackToolProperties,
              RemoveTrackPreservesTheOrderOfRemainingTracks,
              ()) {
    // 1-20 tracks and 0-50 clips each: a removal target always exists.
    const Project initial = drawProject(drawShape(1));
    RC_ASSERT(!initial.tracks.empty());

    ProjectSession session;
    RC_ASSERT(session.engine().reset(initial).isOk());
    const ToolRegistry registry = buildDefaultToolRegistry(session);

    // A generated index into the track list.
    const std::size_t victim = *rc::gen::inRange<std::size_t>(0, initial.tracks.size());
    const Uuid        trackId = initial.tracks[victim].id;

    const std::size_t clipsBefore = totalClipCount(initial);
    const std::size_t clipsRemoved = initial.tracks[victim].clips.size();

    const Result<Json> invoked = registry.invoke("timeline.remove_track", trackIdArgs(trackId));
    RC_ASSERT(invoked.isOk());
    const Json& out = invoked.value();

    // The returned counts describe the project that results.
    RC_ASSERT(out.stringOr("trackId") == trackId.toString());
    RC_ASSERT(out.intOr("trackCount") ==
              static_cast<std::int64_t>(initial.tracks.size() - 1));
    RC_ASSERT(out.intOr("clipCount") ==
              static_cast<std::int64_t>(clipsBefore - clipsRemoved));

    const Project after = session.engine().snapshot();
    RC_ASSERT(after.tracks.size() == initial.tracks.size() - 1);
    RC_ASSERT(totalClipCount(after) == clipsBefore - clipsRemoved);

    // The track is gone, and so is every clip that was on it.
    std::unordered_set<Uuid> remainingClipIds;
    for (const Track& track : after.tracks) {
        RC_ASSERT(track.id != trackId);
        for (const Clip& clip : track.clips) remainingClipIds.insert(clip.id);
    }
    for (const Clip& clip : initial.tracks[victim].clips) {
        RC_ASSERT(remainingClipIds.find(clip.id) == remainingClipIds.end());
    }

    // The remaining tracks keep their original relative order, each with its
    // clips untouched.
    std::size_t source = 0;
    for (std::size_t i = 0; i < after.tracks.size(); ++i) {
        if (source == victim) ++source;
        assertSameTrack(initial.tracks[source], after.tracks[i]);
        ++source;
    }
}

}  // namespace
}  // namespace palmier::services
