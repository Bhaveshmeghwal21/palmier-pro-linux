// SPDX-License-Identifier: GPL-3.0-or-later
//
// Property-based test for no-partial-edits atomicity (task 14.3).
//
// Design property P2 (design.md "Correctness Properties"):
//
//     For any command `c` (including a generative request that fails at the
//     provider), the operation either fully applies or leaves the project
//     unchanged; there is never a partial mutation.
//
// This is the atomicity guarantee of Requirement 6.6 ("IF a generation request
// fails at the provider, THEN ... leave the timeline and project library
// unchanged") generalized to the whole editing surface: the TimelineEngine
// snapshots the project before every apply() and rolls back on command failure
// or on any timeline-invariant violation (core/TimelineEngine.cpp, task 3.2),
// and the generative pipeline (services/GenerativeMediaCoordinator.cpp +
// services/GenerativeClient.cpp, tasks 14.1/14.2) never touches the library or
// the timeline when generation fails at the provider.
//
// The file exercises P2 along two complementary axes, both asserting the same
// all-or-nothing shape:
//
//   1. Timeline command atomicity (ApplyIsAllOrNothing). Over arbitrary-length
//      sequences of arbitrary concrete EditCommands driven through a
//      TimelineEngine — deliberately mixing commands that SUCCEED (append a
//      clip, delete an existing clip, add an effect, ...) with commands that
//      FAIL (a null command, delete of an unknown clip, a move to a negative
//      position or onto another clip, an overlapping add, a malformed reorder, a
//      boundary split) — every apply is asserted to be all-or-nothing:
//        * if the engine reports a change, the resulting project satisfies every
//          timeline invariant (a fully-consistent state, never a half-applied
//          one); and
//        * if the engine does NOT report a change (a failed or no-op command),
//          the project snapshot is byte-for-byte identical to the pre-apply
//          snapshot.
//      A guaranteed-failing command (the null command) is injected every step so
//      the failure path — the crux of "no partial edits" — is always exercised.
//
//   2. Generative provider-failure atomicity (ProviderFailureLeavesProjectAnd
//      LibraryUnchanged). A GenerativeMediaCoordinator wired to the REAL
//      GenerativeClientRunner over a GenerativeClient whose backend fails the job
//      at the provider is asked to generate-and-place with an arbitrary valid
//      prompt and placement. The request must fail, and BOTH the timeline
//      (byte-for-byte) and the media library (asset count) must be exactly as
//      they were before the call — Requirement 6.6's "leave the timeline and
//      project library unchanged".
//
// _Requirements: 6.6_

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include "core/Clip.hpp"
#include "core/CommandResult.hpp"
#include "core/Duration.hpp"
#include "core/EditCommand.hpp"
#include "core/EditCommands.hpp"
#include "core/Effect.hpp"
#include "core/Error.hpp"
#include "core/FrameRate.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/MediaManager.hpp"
#include "core/Project.hpp"
#include "core/Resolution.hpp"
#include "core/Track.hpp"
#include "core/TimelineEngine.hpp"
#include "core/Transition.hpp"
#include "core/Uuid.hpp"
#include "services/GenerativeClient.hpp"
#include "services/GenerativeMediaCoordinator.hpp"

namespace palmier {
namespace {

using services::GenerationAuthorization;
using services::GenerationJob;
using services::GenerationMediaType;
using services::GenerationPhase;
using services::GenerationPlacement;
using services::GenerationRequest;
using services::GenerationStatus;
using services::GeneratedMediaPlacement;
using services::GenerativeClient;
using services::GenerativeClientRunner;
using services::GenerativeMediaCoordinator;
using services::IGenerationGate;
using services::IGenerativeBackend;
using services::JobId;
using services::MediaAsset;
using services::TimelineEnginePlacer;

constexpr Duration ms(std::int64_t v) { return Duration::fromMilliseconds(v); }

// --- Structural (byte-for-byte) equality -----------------------------------
//
// The domain types carry no whole-struct operator==, so "unchanged" is checked
// field-by-field across every attribute a command (timeline or generative) can
// touch: clip geometry, effects, transitions, track structure, and — for the
// generative path, which appends to Project.assets — the project asset table.

bool effectsEqual(const Effect& a, const Effect& b) {
    return a.id == b.id && a.type == b.type && a.parameters == b.parameters;
}

bool transitionsEqual(const std::optional<Transition>& a,
                      const std::optional<Transition>& b) {
    if (a.has_value() != b.has_value()) return false;
    if (!a.has_value()) return true;
    return a->id == b->id && a->kind == b->kind && a->duration == b->duration;
}

bool clipsEqual(const Clip& a, const Clip& b) {
    if (a.id != b.id) return false;
    if (a.assetRef.assetId != b.assetRef.assetId) return false;
    if (a.assetRef.sourcePath != b.assetRef.sourcePath) return false;
    if (a.timelineStart != b.timelineStart) return false;
    if (a.sourceIn != b.sourceIn) return false;
    if (a.sourceOut != b.sourceOut) return false;
    if (a.gain != b.gain) return false;
    if (a.opacity != b.opacity) return false;
    if (a.effects.size() != b.effects.size()) return false;
    for (std::size_t i = 0; i < a.effects.size(); ++i) {
        if (!effectsEqual(a.effects[i], b.effects[i])) return false;
    }
    return transitionsEqual(a.transitionIn, b.transitionIn);
}

bool tracksEqual(const Track& a, const Track& b) {
    if (a.id != b.id || a.kind != b.kind || a.muted != b.muted || a.locked != b.locked) {
        return false;
    }
    if (a.clips.size() != b.clips.size()) return false;
    for (std::size_t i = 0; i < a.clips.size(); ++i) {
        if (!clipsEqual(a.clips[i], b.clips[i])) return false;
    }
    return true;
}

bool assetsEqual(const std::vector<MediaAssetRef>& a,
                 const std::vector<MediaAssetRef>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].assetId != b[i].assetId || a[i].sourcePath != b[i].sourcePath) {
            return false;
        }
    }
    return true;
}

bool projectsEqual(const Project& a, const Project& b) {
    if (a.id != b.id || a.name != b.name) return false;
    if (a.timelineFps != b.timelineFps) return false;
    if (!(a.canvas == b.canvas)) return false;
    if (a.colorSpace != b.colorSpace) return false;
    if (a.version != b.version) return false;
    if (!assetsEqual(a.assets, b.assets)) return false;
    if (a.tracks.size() != b.tracks.size()) return false;
    for (std::size_t i = 0; i < a.tracks.size(); ++i) {
        if (!tracksEqual(a.tracks[i], b.tracks[i])) return false;
    }
    return true;
}

// --- Snapshot helpers -------------------------------------------------------

struct ClipLocator {
    Uuid   trackId;
    ClipId clipId;
    Clip   clip;
};

std::vector<ClipLocator> allClips(const Project& p) {
    std::vector<ClipLocator> out;
    for (const Track& t : p.tracks) {
        for (const Clip& c : t.clips) {
            out.push_back(ClipLocator{t.id, c.id, c});
        }
    }
    return out;
}

Duration trackEnd(const Project& p, const Uuid& trackId) {
    Duration end = Duration::zero();
    for (const Track& t : p.tracks) {
        if (t.id != trackId) continue;
        for (const Clip& c : t.clips) {
            if (c.timelineEnd() > end) end = c.timelineEnd();
        }
    }
    return end;
}

Clip makeClip(ClipId id, Duration start, Duration in, Duration out) {
    Clip clip;
    clip.id = id;
    clip.assetRef = MediaAssetRef(Uuid::generateV4(), "mem://asset");
    clip.timelineStart = start;
    clip.sourceIn = in;
    clip.sourceOut = out;
    return clip;
}

// A valid starting project: `videoTracks` video lanes + one audio lane, each
// seeded with a single non-overlapping clip so both success and failure command
// constructions have real targets from step 0.
Project makeSeedProject(int videoTracks) {
    Project project;
    project.id = Uuid::generateV4();
    project.name = "atomicity";
    project.timelineFps = FrameRate::fps30();
    project.canvas = Resolution::hd1080();

    const int total = videoTracks + 1;  // + one audio track
    for (int i = 0; i < total; ++i) {
        Track track;
        track.id = Uuid::generateV4();
        track.kind = (i < videoTracks) ? TrackKind::Video : TrackKind::Audio;
        track.clips.push_back(makeClip(Uuid::generateV4(), ms(0), ms(0), ms(1000)));
        project.tracks.push_back(std::move(track));
    }
    return project;
}

// ===========================================================================
// Property 2 (timeline commands): apply() is all-or-nothing.
// ===========================================================================

// Feature: palmier-pro-linux, Property 2: No partial edits (atomicity) — any
// command either fully applies (leaving a fully-consistent project) or leaves
// the project byte-for-byte unchanged; never a partial mutation.
// Validates: Requirements 6.6
RC_GTEST_PROP(TimelineEngineAtomicityProperties,
              ApplyIsAllOrNothing,
              ()) {
    const int videoTracks = *rc::gen::inRange(1, 4);   // 1..3 video lanes
    const int numCommands = *rc::gen::inRange(1, 25);  // arbitrary edit sequence

    TimelineEngine engine(makeSeedProject(videoTracks));
    const FrameRate fps = FrameRate::fps30();
    const Duration sourceDuration = ms(600000);  // 10-minute virtual source

    for (int step = 0; step < numCommands; ++step) {
        const Project before = engine.snapshot();
        const std::vector<ClipLocator> clips = allClips(before);

        // Choose a command kind. Kinds 0-3 are constructed to SUCCEED; kinds 4-8
        // are constructed to FAIL (rejected by the command or by the engine's
        // invariant check). With no clips only the "append" success path applies.
        const int kind = clips.empty() ? 0 : *rc::gen::inRange(0, 9);

        std::unique_ptr<EditCommand> cmd;
        bool expectFailure = false;

        switch (kind) {
            case 0: {  // SUCCESS — append a clip past the track end (never overlaps).
                const std::size_t ti = static_cast<std::size_t>(
                    *rc::gen::inRange(0, static_cast<int>(before.tracks.size())));
                const Uuid trackId = before.tracks[ti].id;
                const Duration start = trackEnd(before, trackId) +
                                       ms(*rc::gen::inRange(0, 500));
                const std::int64_t frames = *rc::gen::inRange(1, 60);
                cmd = std::make_unique<AddClipCommand>(
                    trackId, makeClip(Uuid::generateV4(), start, ms(0),
                                      fps.durationForFrames(frames)));
                break;
            }
            case 1: {  // SUCCESS — delete an existing clip.
                const ClipLocator& loc = clips[static_cast<std::size_t>(
                    *rc::gen::inRange(0, static_cast<int>(clips.size())))];
                cmd = std::make_unique<DeleteClipCommand>(loc.clipId);
                break;
            }
            case 2: {  // SUCCESS — move a clip past its track's end (valid drop).
                const ClipLocator& loc = clips[static_cast<std::size_t>(
                    *rc::gen::inRange(0, static_cast<int>(clips.size())))];
                const Duration newStart = trackEnd(before, loc.trackId) +
                                          ms(*rc::gen::inRange(0, 500));
                cmd = std::make_unique<MoveClipCommand>(loc.clipId, newStart);
                break;
            }
            case 3: {  // SUCCESS — append an effect to an arbitrary clip.
                const ClipLocator& loc = clips[static_cast<std::size_t>(
                    *rc::gen::inRange(0, static_cast<int>(clips.size())))];
                const double amount =
                    static_cast<double>(*rc::gen::inRange(0, 100)) / 100.0;
                cmd = std::make_unique<AddEffectCommand>(
                    loc.clipId, Effect::brightness(amount));
                break;
            }
            case 4: {  // FAILURE — delete a clip that does not exist.
                cmd = std::make_unique<DeleteClipCommand>(Uuid::generateV4());
                expectFailure = true;
                break;
            }
            case 5: {  // FAILURE — move a clip to a negative timeline position.
                const ClipLocator& loc = clips[static_cast<std::size_t>(
                    *rc::gen::inRange(0, static_cast<int>(clips.size())))];
                cmd = std::make_unique<MoveClipCommand>(
                    loc.clipId, ms(-(*rc::gen::inRange(1, 5000))));
                expectFailure = true;
                break;
            }
            case 6: {  // FAILURE — add a clip that overlaps an existing one
                       // (same track, same start) -> invariant rejects + rolls back.
                const ClipLocator& loc = clips[static_cast<std::size_t>(
                    *rc::gen::inRange(0, static_cast<int>(clips.size())))];
                const std::int64_t frames = *rc::gen::inRange(1, 60);
                cmd = std::make_unique<AddClipCommand>(
                    loc.trackId, makeClip(Uuid::generateV4(), loc.clip.timelineStart,
                                          ms(0), fps.durationForFrames(frames)));
                expectFailure = true;
                break;
            }
            case 7: {  // FAILURE — malformed reorder (an extra unknown id: wrong
                       // length AND references a clip not on the track).
                const std::size_t ti = static_cast<std::size_t>(
                    *rc::gen::inRange(0, static_cast<int>(before.tracks.size())));
                const Track& track = before.tracks[ti];
                std::vector<ClipId> order;
                for (const Clip& c : track.clips) order.push_back(c.id);
                order.push_back(Uuid::generateV4());  // unknown id, wrong length
                cmd = std::make_unique<ReorderClipsCommand>(track.id, std::move(order));
                expectFailure = true;
                break;
            }
            default: {  // case 8 — FAILURE: split at the clip's own start (a
                        // non-interior playhead) leaves everything unchanged (2.6).
                const ClipLocator& loc = clips[static_cast<std::size_t>(
                    *rc::gen::inRange(0, static_cast<int>(clips.size())))];
                cmd = std::make_unique<SplitClipCommand>(loc.clipId, loc.clip.timelineStart);
                expectFailure = true;
                break;
            }
        }

        RC_ASSERT(cmd != nullptr);
        const CommandResult applied = engine.apply(std::move(cmd));

        // The all-or-nothing core of P2.
        if (applied.changed()) {
            // Fully applied -> the resulting project is a fully-consistent
            // timeline (every invariant holds), never a half-applied state.
            RC_ASSERT(checkTimelineInvariants(engine.snapshot()).isOk());
        } else {
            // Failed / no-op -> the project is byte-for-byte unchanged.
            RC_ASSERT(projectsEqual(engine.snapshot(), before));
        }

        // Commands built to fail must indeed leave the project unchanged.
        if (expectFailure) {
            RC_ASSERT(!applied.changed());
            RC_ASSERT(projectsEqual(engine.snapshot(), before));
        }

        // Inject a guaranteed-failing command (the null command) EVERY step so
        // the failure path is always exercised: it must fail and leave the
        // project exactly as it is.
        const Project preNull = engine.snapshot();
        const CommandResult nullResult = engine.apply(std::unique_ptr<EditCommand>{});
        RC_ASSERT(nullResult.isError());
        RC_ASSERT(projectsEqual(engine.snapshot(), preNull));
    }
}

// ===========================================================================
// Property 2 (generative path): a provider failure leaves the project AND the
// media library unchanged (Requirement 6.6).
// ===========================================================================

// A gate that always authorizes, so the request reaches the (failing) provider.
class AlwaysAuthorizeGate : public IGenerationGate {
public:
    Result<GenerationAuthorization> authorize(const GenerationRequest&) const override {
        return GenerationAuthorization{"bearer-token"};
    }
};

// A hosted backend whose job fails at the provider: submit is accepted, the job
// reports a terminal Failed status, and fetchResult would surface the reason.
// This drives the REAL GenerativeClient failure path (poll -> Failed ->
// fetchResult error) end to end.
class ProviderFailsBackend : public IGenerativeBackend {
public:
    explicit ProviderFailsBackend(std::string reason) : reason_(std::move(reason)) {}

    Result<JobId> submit(const GenerationRequest&, std::string_view) override {
        return JobId{"job-1"};
    }
    Result<GenerationStatus> poll(const JobId&, std::string_view) override {
        return GenerationStatus{GenerationPhase::Failed, 0, reason_};
    }
    Result<MediaAsset> fetchResult(const JobId&, std::string_view) override {
        return err<MediaAsset>(makeError(ErrorCode::Internal, reason_));
    }
    Result<void> cancel(const JobId&, std::string_view) override { return ok(); }

private:
    std::string reason_;
};

// Feature: palmier-pro-linux, Property 2: No partial edits (atomicity) — a
// generative request that fails at the provider leaves the timeline
// (byte-for-byte) and the media library unchanged.
// Validates: Requirements 6.6
RC_GTEST_PROP(GenerativeProviderFailureAtomicityProperties,
              ProviderFailureLeavesProjectAndLibraryUnchanged,
              ()) {
    // An arbitrary valid project (1..3 video lanes + audio) inside an engine.
    const int videoTracks = *rc::gen::inRange(1, 4);
    TimelineEngine engine(makeSeedProject(videoTracks));
    const Uuid trackId = engine.snapshot().tracks.front().id;

    // A media library that may already contain some assets, so we can assert the
    // count is preserved (not merely "still empty") across the failed request.
    MediaManager library;
    const int preexistingAssets = *rc::gen::inRange(0, 4);
    for (int i = 0; i < preexistingAssets; ++i) {
        RC_ASSERT(library.importAsset(MediaAssetRef(Uuid::generateV4(),
                                                    "mem://pre")).isOk());
    }

    // Real generative stack whose provider fails the job.
    ProviderFailsBackend backend("provider rejected the prompt");
    GenerativeClient client(backend);
    GenerativeClientRunner runner(client);
    AlwaysAuthorizeGate gate;
    TimelineEnginePlacer placer(engine);
    GenerativeMediaCoordinator coordinator(gate, runner, library, placer);

    // Arbitrary valid prompt (1..2000 chars) and media type.
    const int promptLen = *rc::gen::inRange(1, 2001);
    const bool isVideo = *rc::gen::element(true, false);
    GenerationRequest request;
    request.model = isVideo ? "veo" : "gpt-image";
    request.mediaType = isVideo ? GenerationMediaType::Video : GenerationMediaType::Image;
    request.prompt = std::string(static_cast<std::size_t>(promptLen), 'p');

    // A valid placement at frame 0 (in bounds even for an empty timeline).
    GenerationPlacement where;
    where.trackId = trackId;
    where.framePosition = 0;
    where.sourceIn = Duration::zero();
    where.sourceOut = Duration::fromSeconds(2.0);

    // Capture the exact pre-request state.
    const Project beforeProject = engine.snapshot();
    const std::size_t beforeAssetCount = library.assetCount();

    const Result<GeneratedMediaPlacement> result =
        coordinator.generateAndPlace(request, where);

    // 6.6 — the request fails, and nothing was partially applied: the timeline is
    // byte-for-byte unchanged and the library gained no asset.
    RC_ASSERT(result.isError());
    RC_ASSERT(projectsEqual(engine.snapshot(), beforeProject));
    RC_ASSERT(library.assetCount() == beforeAssetCount);
}

}  // namespace
}  // namespace palmier
