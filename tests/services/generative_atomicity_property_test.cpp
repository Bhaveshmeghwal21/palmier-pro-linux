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
//
// ---------------------------------------------------------------------------
// Task 10.7 extension — Property 67: A failed or timed-out job leaves nothing
// behind (Requirements 12.7, 12.10).
// ---------------------------------------------------------------------------
//
// Requirement 12.7 (a job that reports `failed` after submission) and Requirement
// 12.10 (a job that reaches no terminal status within the configured timeout) make
// the same three-part claim about what must NOT exist afterwards: the project, the
// media library and the undo history are in their pre-submission state, and no
// partially retrieved media is retained. The property below asserts exactly that,
// for both antecedents, over the SAME path a real `generation.generate` call takes.
//
// The rig is the lifecycle suite's `GenerationRig` (task 10.6,
// tests/services/generative_lifecycle_property_test.cpp) with one addition: the
// `GenerativeClient` is built with an injected budget AND an injected clock, so a
// timeout is a property of the exchange rather than of wall time. Everything from
// the `McpToolExecutor` down to `HostedGenerativeBackend` is product code; the
// injected `GenerativeHttpTransport` is the only route from any of it to a socket,
// and it is the seam every failure in this property is injected through:
//
//   * a provider-side `failed` status                — a scripted poll response;
//   * a result retrieval that fails after `succeeded` — a scripted 5xx response;
//   * a job that never reaches a terminal status      — a transport that answers
//     every poll with `running` and advances the VIRTUAL clock as it does so.
//
// The third case is why there is no `sleep` anywhere in this file. The budget is
// generated across Requirement 12.10's configurable range (10 to 3600 seconds) and
// is crossed entirely on the virtual clock, which only the transport advances; the
// property additionally asserts that the REAL elapsed time stayed far below the
// budget, so a future change that made the timeout wait for wall time would fail
// here rather than merely make the suite slow.
//
// Each of the three failures is run twice, on a fresh rig each time: once through
// the `McpToolExecutor` (the shared tool surface, which is also where an
// invocation-level rollback would happen) and once directly against the
// `GenerativeMediaCoordinator` (where no such rollback can mask a leftover). The
// second route is what keeps the undo-stack half of the property honest: the
// executor undoes whatever an invocation applied, so an assertion made only
// through it could not distinguish "nothing was ever applied" from "something was
// applied and then undone". The redo depth is asserted unchanged for the same
// reason — an applied-then-rolled-back mutation would show up there.
//
// _Requirements: 12.7, 12.10_

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
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
#include "services/AuthenticationService.hpp"
#include "services/ByokCredentialManager.hpp"
#include "services/ByokCredentials.hpp"
#include "services/GenerativeBackendRegistry.hpp"
#include "services/GenerativeClient.hpp"
#include "services/GenerativeHttpTransport.hpp"
#include "services/GenerativeMediaCoordinator.hpp"
#include "services/HostedGenerativeBackend.hpp"
#include "services/Json.hpp"
#include "services/McpToolExecutor.hpp"
#include "services/ProjectSession.hpp"
#include "services/ProjectStore.hpp"
#include "services/SecretStore.hpp"
#include "services/ToolRegistry.hpp"

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

// ===========================================================================
// Property 67: A failed or timed-out job leaves nothing behind
// (task 10.7; Requirements 12.7, 12.10)
// ===========================================================================
//
// Everything this property needs lives in its own namespace so that it adds to
// the file rather than reaching into the helpers the two properties above were
// written around (this rig needs a project whose assets are registered and a
// dedicated empty target lane, which `makeSeedProject` above deliberately does
// not provide).

namespace property67 {

using services::AuthBackend;
using services::AuthServiceGenerationGate;
using services::AuthenticationService;
using services::BackendSession;
using services::ByokCredential;
using services::ByokCredentialManager;
using services::ByokProviderValidator;
using services::GenerativeBackend;
using services::GenerativeBackendRequest;
using services::GenerativeBackendSelection;
using services::GenerativeHttpRequest;
using services::GenerativeHttpResponse;
using services::GenerativeHttpTransport;
using services::HostedGenerativeBackend;
using services::InMemorySecretStore;
using services::InvocationSource;
using services::Json;
using services::LoginCredentials;
using services::McpToolExecutor;
using services::ProjectSession;
using services::serializeProject;
using services::ToolRegistry;
using services::ToolRegistryHooks;

constexpr const char* kGenerateTool = "generation.generate";

/// A location, not a credential. `.invalid` is reserved by RFC 2606, so even a
/// bug that did send a request could not reach a real service.
constexpr const char* kEndpointBase = "https://generative.invalid";

/// The secret-store scope the hosted client reads its credential under.
constexpr const char* kUserScope = "default";

/// Placeholder credential values, each NAMING ITSELF, so task 10.8's
/// repository-hygiene checker reads them as descriptions of a secret rather than
/// as one.
constexpr const char* kStoredHostedCredential = "stored-hosted-account-token-placeholder";
constexpr const char* kStoredProviderKey = "stored-byok-provider-key-placeholder";

/// The two models the rig's BYOK credentials authorize.
constexpr const char* kVideoModel = "sota-video-1";
constexpr const char* kImageModel = "sota-image-1";

// ---------------------------------------------------------------------------
// The virtual clock: the only thing that makes time pass in this property
// ---------------------------------------------------------------------------

/// A monotonic clock that advances only when it is told to. The stalling
/// transport below advances it as it answers each poll, which is how Requirement
/// 12.10's timeout is reached without any real waiting: the budget can be an hour
/// and the test still finishes in microseconds.
class VirtualClock {
public:
    using TimePoint = std::chrono::steady_clock::time_point;

    [[nodiscard]] TimePoint now() const noexcept { return origin_ + elapsed_; }
    void advance(std::chrono::milliseconds by) noexcept { elapsed_ += by; }
    [[nodiscard]] std::chrono::milliseconds elapsed() const noexcept { return elapsed_; }

    /// A `GenerativeClient::Clock` reading this clock. The returned callable holds
    /// a reference, so this object must outlive the client.
    [[nodiscard]] services::GenerativeClient::Clock reader() noexcept {
        return [this] { return now(); };
    }

private:
    /// A fixed non-zero epoch, so a time point produced here is never confused
    /// with a default-constructed one.
    TimePoint origin_ = TimePoint{} + std::chrono::hours(1000);
    std::chrono::milliseconds elapsed_{0};
};

// ---------------------------------------------------------------------------
// Transports — the seam every failure in this property is injected through
// ---------------------------------------------------------------------------

/// Records every exchange, and classifies it the way the wire protocol shapes it:
/// a POST to the collection is a submit, a POST to `.../cancel` is a cancel, and a
/// GET is a poll or a result fetch.
class RecordingTransport : public GenerativeHttpTransport {
public:
    [[nodiscard]] static bool isCancel(const GenerativeHttpRequest& request) {
        return request.method == "POST" && request.url.size() >= 7 &&
               request.url.compare(request.url.size() - 7, 7, "/cancel") == 0;
    }
    [[nodiscard]] static bool isSubmit(const GenerativeHttpRequest& request) {
        return request.method == "POST" && !isCancel(request);
    }
    [[nodiscard]] static bool isResultFetch(const GenerativeHttpRequest& request) {
        return request.method == "GET" && request.url.size() >= 7 &&
               request.url.compare(request.url.size() - 7, 7, "/result") == 0;
    }
    [[nodiscard]] static bool isPoll(const GenerativeHttpRequest& request) {
        return request.method == "GET" && !isResultFetch(request);
    }

    [[nodiscard]] std::size_t count(bool (*kind)(const GenerativeHttpRequest&)) const {
        return static_cast<std::size_t>(std::count_if(requests.begin(), requests.end(),
                                                      [kind](const GenerativeHttpRequest& r) {
                                                          return kind(r);
                                                      }));
    }

    std::vector<GenerativeHttpRequest> requests;
};

/// Replays scripted responses. Used for the two provider-side failures, which are
/// entirely a matter of what the endpoint answers.
class ScriptedTransport final : public RecordingTransport {
public:
    [[nodiscard]] Result<GenerativeHttpResponse> send(
        const GenerativeHttpRequest& request) override {
        requests.push_back(request);
        if (responses.empty()) {
            return err<GenerativeHttpResponse>(
                makeError(ErrorCode::Internal, "the test script ran out of responses"));
        }
        GenerativeHttpResponse next = responses.front();
        responses.erase(responses.begin());
        return Result<GenerativeHttpResponse>(std::move(next));
    }

    std::vector<GenerativeHttpResponse> responses;
};

/// A provider that accepts the job and then never finishes it: every poll is
/// answered `running`, and answering it advances the virtual clock. This is the
/// timeout injection of Requirement 12.10 — the job reaches no terminal status,
/// and the budget elapses on the clock the client was built with.
class StallingTransport final : public RecordingTransport {
public:
    StallingTransport(VirtualClock& clock, std::chrono::milliseconds advancePerPoll,
                      std::string jobId)
        : clock_(clock), advancePerPoll_(advancePerPoll), jobId_(std::move(jobId)) {}

    [[nodiscard]] Result<GenerativeHttpResponse> send(
        const GenerativeHttpRequest& request) override {
        requests.push_back(request);

        if (isCancel(request)) {
            // The provider accepts the cancellation the timeout issues (12.10).
            return GenerativeHttpResponse{200, "{}"};
        }
        if (isSubmit(request)) {
            return GenerativeHttpResponse{201, R"({"id":")" + jobId_ + R"("})"};
        }
        if (isResultFetch(request)) {
            // A timed-out job is never fetched; seeing this would mean the client
            // treated a non-terminal job as complete.
            ADD_FAILURE() << "a stalled job must never have its result fetched";
            return GenerativeHttpResponse{500, R"({"message":"not available"})"};
        }

        // A poll: still working, and answering took time on the virtual clock.
        clock_.advance(advancePerPoll_);
        return GenerativeHttpResponse{200, R"({"status":"running","progress":10})"};
    }

private:
    VirtualClock& clock_;
    std::chrono::milliseconds advancePerPoll_;
    std::string jobId_;
};

// ---------------------------------------------------------------------------
// Offline collaborators for the real auth gate
// ---------------------------------------------------------------------------

/// Accepts any well-formed BYOK credential; contacts no provider.
class AcceptingProviderValidator final : public ByokProviderValidator {
public:
    [[nodiscard]] Result<void> validate(const ByokCredential& credential) override {
        if (!credential.isWellFormed()) {
            return err(makeError(ErrorCode::InvalidArgument,
                                 "a BYOK credential needs a provider and a key"));
        }
        return ok();
    }
};

/// The rig authorizes through BYOK, so the hosted login backend is never reached.
class UnusedAuthBackend final : public AuthBackend {
public:
    [[nodiscard]] Result<BackendSession> authenticate(const LoginCredentials&) override {
        ADD_FAILURE() << "the rig authorizes through BYOK and must never log in";
        return err<BackendSession>(makeError(ErrorCode::Internal, "unused"));
    }
};

// ---------------------------------------------------------------------------
// Seed project
// ---------------------------------------------------------------------------

Clip makeSeededClip(ClipId id, const MediaAssetRef& asset, Duration start, Duration in,
                    Duration out) {
    Clip clip;
    clip.id = id;
    clip.assetRef = asset;
    clip.timelineStart = start;
    clip.sourceIn = in;
    clip.sourceOut = out;
    return clip;
}

/// `videoTracks` seeded video lanes, one seeded audio lane, and one EMPTY video
/// lane at the back which is the generation target — so any in-range position is a
/// legal placement and a failure is never confused with a rejected placement.
/// Every seeded clip's asset is registered in `Project.assets`, so the serialized
/// document the property compares is one the `.palmier` store would accept.
Project makeGenerationSeedProject(int videoTracks) {
    Project project;
    project.id = Uuid::generateV4();
    project.name = "generative-atomicity";
    project.timelineFps = FrameRate::fps30();
    project.canvas = Resolution::hd1080();

    const auto addSeededTrack = [&project](TrackKind kind) {
        Track track;
        track.id = Uuid::generateV4();
        track.kind = kind;
        const MediaAssetRef asset(Uuid::generateV4(), "mem://seed");
        project.assets.push_back(asset);
        track.clips.push_back(makeSeededClip(Uuid::generateV4(), asset, ms(0), ms(0), ms(1000)));
        project.tracks.push_back(std::move(track));
    };

    for (int i = 0; i < videoTracks; ++i) addSeededTrack(TrackKind::Video);
    addSeededTrack(TrackKind::Audio);

    Track target;
    target.id = Uuid::generateV4();
    target.kind = TrackKind::Video;
    project.tracks.push_back(std::move(target));
    return project;
}

// ---------------------------------------------------------------------------
// The generate hook (mirrors app/ApplicationComposition.cpp's makeGenerateHook)
// ---------------------------------------------------------------------------

[[nodiscard]] services::Tool::Handler makeGenerateHook(GenerativeMediaCoordinator& coordinator,
                                                       const GenerativeBackend* backend) {
    return [&coordinator, backend](const Json& input) -> Result<Json> {
        if (backend != nullptr) {
            const std::string unmet = backend->unmetPrecondition();
            if (!unmet.empty()) {
                return err<Json>(makeError(ErrorCode::FailedPrecondition, unmet));
            }
        }

        GenerationRequest request;
        request.model = input.stringOr("model");
        request.prompt = input.stringOr("prompt");
        request.mediaType = (input.stringOr("mediaType", "video") == "image")
                                ? GenerationMediaType::Image
                                : GenerationMediaType::Video;

        GenerationPlacement placement;
        const std::optional<Uuid> trackId = Uuid::parse(input.stringOr("trackId"));
        if (!trackId.has_value()) {
            return err<Json>(makeError(ErrorCode::InvalidArgument,
                                       "generation.generate: 'trackId' must be a valid UUID"));
        }
        placement.trackId = *trackId;
        placement.framePosition = input.intOr("framePosition", 0);
        placement.sourceIn = Duration::fromNanoseconds(input.intOr("sourceInTicks", 0));
        placement.sourceOut = Duration::fromNanoseconds(input.intOr("sourceOutTicks", 0));

        Result<GeneratedMediaPlacement> placed =
            coordinator.generateAndPlace(request, placement);
        if (placed.isError()) {
            return err<Json>(placed.error());
        }

        const GeneratedMediaPlacement& result = placed.value();
        Json out = Json::object();
        out.set("assetId", result.asset.assetId.toString());
        out.set("sourcePath", result.asset.sourcePath);
        out.set("clipId", result.clipId.toString());
        out.set("timelineStartTicks",
                static_cast<std::int64_t>(result.timelineStart.ticks()));
        return out;
    };
}

// ---------------------------------------------------------------------------
// The rig: the whole generation.generate path over one injected transport,
// with an injected timeout budget and an injected clock
// ---------------------------------------------------------------------------

class GenerationRig {
public:
    GenerationRig(GenerativeHttpTransport& transport, int videoTracks,
                  std::chrono::milliseconds budget, services::GenerativeClient::Clock clock) {
        const Result<void> stored =
            secretStore_.store(HostedGenerativeBackend::credentialKey(kUserScope),
                               kStoredHostedCredential);
        EXPECT_TRUE(stored.isOk());

        auth_.setByokManager(byok_);
        for (const char* model : {kVideoModel, kImageModel}) {
            const Result<void> saved =
                auth_.saveByokCredentials(ByokCredential{model, kStoredProviderKey});
            EXPECT_TRUE(saved.isOk());
        }

        GenerativeBackendRequest selection;
        selection.id = std::string(services::kGenerativeBackendHosted);
        selection.endpoint.baseUrl = kEndpointBase;
        selection.secretStore = &secretStore_;
        selection.transport = &transport;
        selection.userId = kUserScope;
        selection.credentials = [](std::string_view) { return true; };
        selection_ = services::selectGenerativeBackend(selection);
        EXPECT_EQ(selection_.id, "hosted");
        EXPECT_TRUE(selection_.startupError.empty()) << selection_.startupError;
        EXPECT_TRUE(selection_.backend->unmetPrecondition().empty());

        // The injected budget and clock are the whole reason this rig exists apart
        // from the lifecycle suite's: Requirement 12.10's timeout is reached on the
        // clock the transport advances, never on wall time.
        client_ = std::make_unique<GenerativeClient>(*selection_.backend, budget,
                                                    std::move(clock));
        runner_ = std::make_unique<GenerativeClientRunner>(*client_);

        const CommandResult seeded =
            session_.engine().reset(makeGenerationSeedProject(videoTracks));
        EXPECT_TRUE(seeded.changed()) << seeded.message();

        placer_ = std::make_unique<TimelineEnginePlacer>(session_.engine());
        placer_->setMediaLibrary(session_.mediaLibrary());
        coordinator_ = std::make_unique<GenerativeMediaCoordinator>(
            gate_, *runner_, session_.mediaLibrary(), *placer_);

        ToolRegistryHooks hooks;
        hooks.generate = makeGenerateHook(*coordinator_, selection_.backend.get());
        registry_ = services::buildDefaultToolRegistry(&session_, std::move(hooks));
        executor_ = std::make_unique<McpToolExecutor>(registry_, &session_);
    }

    [[nodiscard]] TimelineEngine& engine() noexcept { return session_.engine(); }
    [[nodiscard]] MediaManager& library() noexcept { return session_.mediaLibrary(); }
    [[nodiscard]] McpToolExecutor& executor() noexcept { return *executor_; }
    [[nodiscard]] GenerativeMediaCoordinator& coordinator() noexcept { return *coordinator_; }

    /// The generation target: the empty video lane at the back of the seed.
    [[nodiscard]] Uuid targetTrackId() { return session_.engine().snapshot().tracks.back().id; }

    /// The first seeded lane, used for the prior edits that vary the undo depth.
    [[nodiscard]] Uuid seededTrackId() { return session_.engine().snapshot().tracks.front().id; }

private:
    UnusedAuthBackend authBackend_;
    AcceptingProviderValidator validator_;
    InMemorySecretStore secretStore_;
    ByokCredentialManager byok_{validator_, secretStore_, kUserScope};
    AuthenticationService auth_{authBackend_};
    AuthServiceGenerationGate gate_{auth_};
    GenerativeBackendSelection selection_;
    std::unique_ptr<GenerativeClient> client_;
    std::unique_ptr<GenerativeClientRunner> runner_;
    ProjectSession session_;
    std::unique_ptr<TimelineEnginePlacer> placer_;
    std::unique_ptr<GenerativeMediaCoordinator> coordinator_;
    ToolRegistry registry_;
    std::unique_ptr<McpToolExecutor> executor_;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::size_t totalClips(const Project& project) {
    std::size_t clips = 0;
    for (const Track& track : project.tracks) clips += track.clips.size();
    return clips;
}

Duration lastClipEnd(const Project& project, const Uuid& trackId) {
    Duration end = Duration::zero();
    for (const Track& track : project.tracks) {
        if (track.id != trackId) continue;
        for (const Clip& clip : track.clips) {
            if (clip.timelineEnd() > end) end = clip.timelineEnd();
        }
    }
    return end;
}

/// A valid `generation.generate` argument object: every field inside the declared
/// schema, so the ONLY thing that can fail the request is the job itself.
Json validArguments(const Uuid& trackId, bool isVideo, const std::string& prompt,
                    std::int64_t framePosition, std::int64_t sourceOutTicks) {
    Json args = Json::object();
    args.set("prompt", prompt);
    args.set("model", isVideo ? kVideoModel : kImageModel);
    args.set("mediaType", isVideo ? "video" : "image");
    args.set("trackId", trackId.toString());
    args.set("framePosition", framePosition);
    args.set("sourceInTicks", static_cast<std::int64_t>(0));
    args.set("sourceOutTicks", sourceOutTicks);
    return args;
}

/// The accepted-submission prefix of a scripted exchange: the job is created, and
/// then reports `nonTerminalPolls` in-flight transitions. Both provider failures
/// below happen AFTER submission, which is Requirement 12.7's antecedent.
void scriptSubmissionAndProgress(ScriptedTransport& transport, const std::string& jobId,
                                 int nonTerminalPolls) {
    transport.responses.push_back({201, std::string(R"({"id":")") + jobId + R"("})"});
    for (int i = 0; i < nonTerminalPolls; ++i) {
        const char* phase = (i % 2 == 0) ? "queued" : "running";
        transport.responses.push_back(
            {200, std::string(R"({"status":")") + phase + R"(","progress":)" +
                      std::to_string(10 * (i + 1)) + "}"});
    }
}

/// Which route the failed request is issued over. Both are the same product path;
/// they differ only in whether the executor's invocation-level rollback is in
/// front of it, which is what keeps the undo-stack assertion honest.
enum class Route { ToolSurface, Coordinator };

// Feature: end-to-end-editor-integration, Property 67: A failed or timed-out job
// leaves nothing behind — a generation job that reports `failed` after
// submission, whose result cannot be retrieved, or that reaches no terminal
// status within the configured timeout, leaves no media-library entry, no
// timeline clip and no undo-history entry, and reports the job identifier
// together with the failure reason or the elapsed timeout limit.
// Validates: Requirements 12.7, 12.10
RC_GTEST_PROP(GenerativeAtomicityProperties, AFailedOrTimedOutJobLeavesNothingBehind, ()) {
    // --- generated inputs ---------------------------------------------------
    const int videoTracks = *rc::gen::inRange(1, 4);
    const int priorEdits = *rc::gen::inRange(0, 4);        // varies the baseline undo depth
    const int nonTerminalPolls = *rc::gen::inRange(0, 4);  // queued/running before the failure
    const bool isVideo = *rc::gen::element(true, false);
    const int promptLength = *rc::gen::inRange(1, 2001);
    const std::int64_t clipSeconds = *rc::gen::inRange(1, 6);
    const std::string failureReason = *rc::gen::element<std::string>(
        "the provider rejected the prompt", "the model is overloaded",
        "content policy refusal", "an internal provider error occurred");

    // Requirement 12.10's configured job timeout: default 600 s, configurable
    // between 10 and 3600 s. The whole range is generated, and the whole range is
    // crossed on the virtual clock.
    const std::int64_t budgetSeconds = *rc::gen::inRange<std::int64_t>(10, 3601);
    const int pollsToTimeout = *rc::gen::inRange(1, 5);

    const std::string prompt(static_cast<std::size_t>(promptLength), 'p');

    // Runs ONE failed generation on a FRESH rig and asserts the whole of
    // Requirement 12.7/12.10's "nothing left behind". Returns the error the
    // surface reported, so the caller can additionally check what it names.
    const auto runFailedGeneration = [&](GenerativeHttpTransport& transport,
                                         services::GenerativeClient::Clock clock,
                                         std::chrono::milliseconds budget,
                                         Route route) -> Error {
        GenerationRig rig(transport, videoTracks, budget, std::move(clock));
        TimelineEngine& engine = rig.engine();

        // Prior edits on a SEEDED lane (never the target), so the undo depth the
        // failure must not change is an arbitrary number rather than always zero:
        // a leftover entry shows up as a +1 rather than hiding in an empty history.
        const Uuid seededTrack = rig.seededTrackId();
        for (int i = 0; i < priorEdits; ++i) {
            const Project staged = engine.snapshot();
            const MediaAssetRef asset(Uuid::generateV4(), "mem://prior");
            const Duration start = lastClipEnd(staged, seededTrack) + ms(100 * (i + 1));
            const CommandResult applied = engine.apply(std::make_unique<AddClipCommand>(
                seededTrack, makeSeededClip(Uuid::generateV4(), asset, start, ms(0), ms(500))));
            RC_ASSERT(applied.changed());
        }

        // A placement inside [0, current timeline duration] on the empty target
        // lane, so nothing about the placement itself can refuse this request.
        const Uuid targetTrack = rig.targetTrackId();
        const Project before = engine.snapshot();
        const std::int64_t maxFrames =
            before.timelineFps.framesForDuration(timelineDuration(before));
        const std::int64_t framePosition = maxFrames > 0 ? maxFrames / 2 : 0;

        // --- the exact pre-submission state ---------------------------------
        const std::string beforeDocument = serializeProject(before);
        const std::size_t undoBefore = engine.undoDepth();
        const std::size_t redoBefore = engine.redoDepth();
        const std::size_t libraryBefore = rig.library().assetCount();
        const std::size_t clipsBefore = totalClips(before);
        const std::size_t assetsBefore = before.assets.size();

        Error reported;
        if (route == Route::ToolSurface) {
            const Json arguments = validArguments(targetTrack, isVideo, prompt, framePosition,
                                                  clipSeconds * 1000000000);
            const Result<Json> executed =
                rig.executor().executeTool(kGenerateTool, arguments, InvocationSource::Mcp);
            RC_ASSERT(executed.isError());
            reported = executed.error();
        } else {
            GenerationRequest request;
            request.model = isVideo ? kVideoModel : kImageModel;
            request.mediaType =
                isVideo ? GenerationMediaType::Video : GenerationMediaType::Image;
            request.prompt = prompt;

            GenerationPlacement where;
            where.trackId = targetTrack;
            where.framePosition = framePosition;
            where.sourceIn = Duration::zero();
            where.sourceOut = Duration::fromSeconds(static_cast<double>(clipSeconds));

            const Result<GeneratedMediaPlacement> placed =
                rig.coordinator().generateAndPlace(request, where);
            RC_ASSERT(placed.isError());
            reported = placed.error();
        }

        // --- nothing left behind, in each of the three places named ---------
        const Project after = engine.snapshot();

        // (1) no media-library entry — neither in the session's MediaManager nor in
        //     the project's own asset table (the orphan a library-before-placement
        //     ordering would leave behind).
        RC_ASSERT(rig.library().assetCount() == libraryBefore);
        RC_ASSERT(after.assets.size() == assetsBefore);

        // (2) no timeline clip.
        RC_ASSERT(totalClips(after) == clipsBefore);

        // (3) no undo-stack entry. And no redo entry either: a mutation that was
        //     applied and then rolled back would be visible there, so this is what
        //     distinguishes "nothing happened" from "something was undone".
        RC_ASSERT(engine.undoDepth() == undoBefore);
        RC_ASSERT(engine.redoDepth() == redoBefore);

        // The whole project, as a document and structurally: the pre-submission
        // state, byte for byte (Requirements 12.7, 12.10).
        RC_ASSERT(serializeProject(after) == beforeDocument);
        RC_ASSERT(projectsEqual(after, before));

        return reported;
    };

    const std::chrono::milliseconds budget(budgetSeconds * 1000);

    for (const Route route : {Route::ToolSurface, Route::Coordinator}) {
        // ===================================================================
        // 12.7 — the job reports `failed` after submission.
        // ===================================================================
        {
            VirtualClock clock;  // never advanced: this failure is not a timeout
            const std::string jobId = "job-failed-1";
            ScriptedTransport transport;
            scriptSubmissionAndProgress(transport, jobId, nonTerminalPolls);
            transport.responses.push_back(
                {200, std::string(R"({"status":"failed","progress":40,"reason":")") +
                          failureReason + R"("})"});

            const Error reported =
                runFailedGeneration(transport, clock.reader(), budget, route);

            // 12.7 — the report identifies the job and carries the failure reason.
            RC_ASSERT(reported.message().find(jobId) != std::string::npos);
            RC_ASSERT(reported.message().find(failureReason) != std::string::npos);

            // The job was submitted (so this really is a post-submission failure)
            // and its result was never fetched, so there is no partially retrieved
            // media to retain.
            RC_ASSERT(transport.count(&RecordingTransport::isSubmit) == 1u);
            RC_ASSERT(transport.count(&RecordingTransport::isResultFetch) == 0u);
        }

        // ===================================================================
        // 12.7 — the job succeeded but its media cannot be retrieved. The
        // "retain no partially retrieved media file" half: the retrieval is the
        // only step that could have produced a partial file.
        // ===================================================================
        {
            VirtualClock clock;
            const std::string jobId = "job-failed-2";
            ScriptedTransport transport;
            scriptSubmissionAndProgress(transport, jobId, nonTerminalPolls);
            transport.responses.push_back({200, R"({"status":"succeeded","progress":100})"});
            transport.responses.push_back(
                {503, std::string(R"({"message":")") + failureReason + R"("})"});

            const Error reported =
                runFailedGeneration(transport, clock.reader(), budget, route);

            // 12.7 — reported against the job that produced it, with the provider's
            // own reason preserved.
            RC_ASSERT(reported.message().find(jobId) != std::string::npos);
            RC_ASSERT(reported.message().find(failureReason) != std::string::npos);
            RC_ASSERT(transport.count(&RecordingTransport::isResultFetch) == 1u);
        }

        // ===================================================================
        // 12.10 — the job reaches no terminal status within the configured
        // timeout. Injected entirely through the transport seam: it answers every
        // poll `running` and advances the virtual clock as it does so.
        // ===================================================================
        {
            VirtualClock clock;
            const std::string jobId = "job-stalled";
            const std::chrono::milliseconds advancePerPoll(
                budget.count() / pollsToTimeout + 1);
            StallingTransport transport(clock, advancePerPoll, jobId);

            // Real time, so the claim "no wall-clock wait" is asserted rather than
            // asserted-by-inspection.
            const std::chrono::steady_clock::time_point realStart =
                std::chrono::steady_clock::now();
            const Error reported =
                runFailedGeneration(transport, clock.reader(), budget, route);
            const std::chrono::milliseconds realElapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - realStart);

            // 12.10 — a failure identifying the job and stating that the timeout
            // limit elapsed, naming the limit that was actually configured.
            RC_ASSERT(reported.code() == ErrorCode::Timeout);
            RC_ASSERT(reported.message().find(jobId) != std::string::npos);
            RC_ASSERT(reported.message().find(std::to_string(budgetSeconds)) !=
                      std::string::npos);
            RC_ASSERT(reported.message().find("timeout limit elapsed") != std::string::npos);

            // The timeout was reached on the virtual clock, and only there: the
            // budget elapsed virtually while real time did not come close to it.
            RC_ASSERT(clock.elapsed() >= budget);
            RC_ASSERT(realElapsed < budget / 2);

            // The exchange is exactly the polling loop the budget allows: the job
            // was submitted, polled until the budget was crossed — the k-th poll is
            // the first whose accumulated advance reaches it — and then cancelled at
            // the provider, which is the last thing sent, because a timed-out job is
            // no longer tracked (12.10's "stop tracking that job").
            const std::size_t polls = transport.count(&RecordingTransport::isPoll);
            RC_ASSERT(transport.count(&RecordingTransport::isSubmit) == 1u);
            RC_ASSERT(polls >= 1u);
            RC_ASSERT(static_cast<std::int64_t>(polls) * advancePerPoll.count() >=
                      budget.count());
            RC_ASSERT(static_cast<std::int64_t>(polls - 1u) * advancePerPoll.count() <
                      budget.count());
            RC_ASSERT(transport.count(&RecordingTransport::isCancel) == 1u);
            RC_ASSERT(RecordingTransport::isCancel(transport.requests.back()));
        }
    }
}

}  // namespace property67

}  // namespace
}  // namespace palmier
