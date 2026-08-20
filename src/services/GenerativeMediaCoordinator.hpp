// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/GenerativeMediaCoordinator.hpp — ties the in-timeline generative
// pipeline together (design.md "Component 5: Generative AI Client & Auth";
// Requirements 6.2, 6.5, 6.7, 9.7). Task 14.2.
//
// The GenerativeClient (task 14.1) owns the submit -> poll -> fetch job lifecycle
// against the hosted backend, but it deliberately performs no prompt validation,
// no entitlement gating, and no project mutation. This coordinator supplies that
// editor-side policy and wiring: it is the single entry point the UI, MCP server,
// and in-app agent use to "generate media from a prompt and drop it on the
// timeline". Its responsibilities:
//
//   * 6.7 — validate the prompt length (1..2000 characters). An empty or
//           over-length prompt is rejected with InvalidArgument and a message
//           indicating the allowed length, and NOTHING downstream runs, so the
//           timeline and project library are left unchanged.
//   * 6.5 / 9.7 — gate the request on authentication/entitlement: an active
//           subscription OR valid BYOK credentials authorize generation; without
//           either the request is rejected (Unauthenticated) with an indication
//           to authenticate, again leaving the timeline unchanged.
//   * 6.2 — on a successful generation, add the returned media to the project
//           media library (MediaManager) and place it on the timeline at the
//           user-specified position, measured in frames from the timeline start
//           (0 to the current timeline duration).
//
// Rejection/failure ordering guarantees the timeline is never partially mutated:
// prompt validation, entitlement gating, and frame-position bounds are all
// checked BEFORE any generation runs, and the actual placement flows through the
// TimelineEngine's atomic apply (which rolls back on any invariant violation).
// This underpins property P2 (no partial edits / atomicity, task 14.3).
//
// Every external collaborator is reached through a narrow seam
// (IGenerationGate / IGenerationRunner / ITimelinePlacement) so the coordinator's
// policy is unit-testable with mocks and no real network, auth backend, or GPU.
// Concrete adapters over the real AuthenticationService, GenerativeClient, and
// TimelineEngine are provided below for the composition root (task 21.1).

#ifndef PALMIER_SERVICES_GENERATIVEMEDIACOORDINATOR_HPP
#define PALMIER_SERVICES_GENERATIVEMEDIACOORDINATOR_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "core/Duration.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/Result.hpp"
#include "core/Uuid.hpp"
#include "services/GenerationModelCatalog.hpp"
#include "services/GenerativeClient.hpp"

namespace palmier {
class MediaManager;   // core/MediaManager.hpp — the project media library.
class TimelineEngine; // core/TimelineEngine.hpp — the authoritative timeline.
} // namespace palmier

namespace palmier::services {

class AuthenticationService; // services/AuthenticationService.hpp
class GenerativeClient;      // services/GenerativeClient.hpp

/// Prompt-length bounds enforced by the coordinator (Requirement 6.7): a prompt
/// must be at least 1 and at most 2000 characters.
inline constexpr std::size_t kMinPromptLength = 1;
inline constexpr std::size_t kMaxPromptLength = 2000;

// ---------------------------------------------------------------------------
// Placement request / result value types
// ---------------------------------------------------------------------------

/// Where a generated clip should land on the timeline (Requirement 6.2).
///
///   * trackId       — the target track (must exist in the current project).
///   * framePosition — position measured in frames from the timeline start;
///                     valid range is 0 to the current timeline duration in
///                     frames (a negative or beyond-duration value is rejected).
///   * sourceIn/Out  — the source range within the generated media the placed
///                     clip presents; sourceOut must strictly exceed sourceIn.
struct GenerationPlacement {
    Uuid         trackId;
    std::int64_t framePosition = 0;
    Duration     sourceIn = Duration::zero();
    Duration     sourceOut = Duration::zero();
};

/// The outcome of a successful generate-and-place: the generated asset that was
/// added to the library, the id of the newly created timeline clip, and the
/// resolved timeline start position the clip was placed at.
struct GeneratedMediaPlacement {
    MediaAssetRef asset;
    Uuid          clipId;
    Duration      timelineStart;
};

// ---------------------------------------------------------------------------
// Seams
// ---------------------------------------------------------------------------

/// The bearer authorization to use for a generation request.
struct GenerationAuthorization {
    std::string authToken; ///< Non-empty bearer authorizing the hosted request.
};

/// Entitlement gate (Requirements 6.5, 9.7): decides whether a generative
/// request is authorized (active subscription OR valid BYOK) and yields the
/// bearer to submit with. A rejection returns ErrorCode::Unauthenticated.
class IGenerationGate {
public:
    virtual ~IGenerationGate() = default;
    [[nodiscard]] virtual Result<GenerationAuthorization> authorize(
        const GenerationRequest& request) const = 0;
};

/// Runs a generation to completion (Requirement 6.1/6.3/6.4/6.6/6.8). Wraps the
/// GenerativeClient submit -> poll -> fetch lifecycle behind one call so the
/// coordinator does not depend on the polling mechanics. Returns the generated
/// MediaAsset, or forwards a provider/timeout error unchanged.
class IGenerationRunner {
public:
    virtual ~IGenerationRunner() = default;
    [[nodiscard]] virtual Result<MediaAsset> generate(const GenerationRequest& request,
                                                      std::string_view authToken) = 0;
};

/// Places a generated clip on the timeline (Requirement 6.2). Implementations
/// MUST be atomic: on any failure the timeline is left byte-for-byte unchanged.
struct TimelinePlacementRequest {
    Uuid          trackId;
    MediaAssetRef asset;
    std::int64_t  framePosition = 0;
    Duration      sourceIn = Duration::zero();
    Duration      sourceOut = Duration::zero();
};
class ITimelinePlacement {
public:
    virtual ~ITimelinePlacement() = default;
    [[nodiscard]] virtual Result<GeneratedMediaPlacement> place(
        const TimelinePlacementRequest& request) = 0;

    /// Whether `trackId` names a track in the CURRENT project (Requirement 12.9).
    /// Deliberately independent of `place()`, which also needs the generated
    /// asset: this lets the coordinator refuse an unknown track before running a
    /// generation, rather than discovering the same absence only once `place()`
    /// is finally reachable.
    [[nodiscard]] virtual bool trackExists(const Uuid& trackId) const = 0;
};

// ---------------------------------------------------------------------------
// GenerativeMediaCoordinator
// ---------------------------------------------------------------------------

/// Orchestrates prompt validation, entitlement gating, generation, and placement.
/// All references must outlive the coordinator.
///
/// Thread-affinity: instances are not internally synchronized; callers sharing
/// one across threads must provide external synchronization.
class GenerativeMediaCoordinator {
public:
    /// `catalog` is optional: null means every request is validated exactly as
    /// it was before PR 406/396/395 (a plain prompt-to-video/image request needs
    /// no catalog lookup at all, and this coordinator predates the catalog's
    /// existence). A non-null catalog enables the two additional checks Requests
    /// 14's backlog entries ask for: `mode: "upscale"` requires the selected
    /// model to declare `servesUpscale`, and `mediaType: "audio"` requires the
    /// selected model to declare an `audioDurationRange` that
    /// `request.requestedDuration` falls within. `catalog`, when non-null, must
    /// outlive the coordinator, matching every other reference this constructor
    /// takes.
    GenerativeMediaCoordinator(IGenerationGate& gate,
                               IGenerationRunner& runner,
                               MediaManager& library,
                               ITimelinePlacement& placement,
                               const GenerationModelCatalog* catalog = nullptr);

    /// Validate the prompt, gate on entitlement, generate the media, add it to
    /// the library, and place it on the timeline at `where`.
    ///
    /// Failure ordering (nothing downstream runs once a step rejects):
    ///   * empty / >2000-char prompt          -> InvalidArgument (Req 6.7);
    ///   * sourceOut <= sourceIn               -> InvalidArgument;
    ///   * unknown target track                -> NotFound (Req 12.9), checked
    ///                                            before generation ever runs;
    ///   * `mode: "upscale"` naming a model absent from the catalog, or one that
    ///     does not declare `servesUpscale`    -> InvalidArgument naming the
    ///                                            rejected model id (PR 396),
    ///                                            checked only when a catalog is
    ///                                            installed;
    ///   * `mode: "upscale"` with a nil or unknown `sourceAssetId`
    ///                                          -> InvalidArgument / NotFound
    ///                                            (PR 396);
    ///   * `mediaType: "audio"` naming a model absent from the catalog, one with
    ///     no declared audio duration range, or a `requestedDuration` outside
    ///     that range                          -> InvalidArgument naming the
    ///                                            permitted range (PR 395),
    ///                                            checked only when a catalog is
    ///                                            installed;
    ///   * no active subscription and no BYOK  -> Unauthenticated (Req 6.5/9.7);
    ///   * generation fails / times out        -> forwarded error (Req 6.6/6.8);
    ///   * placement rejected (bounds/overlap) -> forwarded error, timeline
    ///                                            unchanged (Req 6.2).
    /// On success the returned placement carries the generated asset, the new
    /// clip id, and its resolved timeline start.
    [[nodiscard]] Result<GeneratedMediaPlacement> generateAndPlace(
        const GenerationRequest& request, const GenerationPlacement& where);

    /// Validate a prompt in isolation (Requirement 6.7). ok() when the prompt is
    /// 1..2000 characters; otherwise InvalidArgument with a message indicating
    /// the allowed length.
    [[nodiscard]] static Result<void> validatePrompt(std::string_view prompt);

private:
    /// The catalog-driven checks (PR 396's upscale gate, PR 395's duration-range
    /// gate). A no-op returning ok() when no catalog is installed, so a plain
    /// prompt-to-video/image request is unaffected either way.
    [[nodiscard]] Result<void> checkAgainstCatalog(const GenerationRequest& request) const;

    IGenerationGate&              gate_;
    IGenerationRunner&            runner_;
    MediaManager&                 library_;
    ITimelinePlacement&           placement_;
    const GenerationModelCatalog* catalog_ = nullptr;
};

// ---------------------------------------------------------------------------
// Concrete adapters for the composition root (task 21.1)
// ---------------------------------------------------------------------------

/// IGenerationGate backed by the real AuthenticationService (Req 6.5, 9.7).
///
/// Authorized when the current session holds an Active subscription entitlement,
/// or when a valid BYOK credential is authorized for the request's provider. The
/// provider is derived from the request via `providerResolver` (defaults to the
/// request's model id). The bearer returned is the session token when logged in,
/// otherwise a BYOK-scoped token.
class AuthServiceGenerationGate : public IGenerationGate {
public:
    using ProviderResolver = std::function<std::string(const GenerationRequest&)>;

    explicit AuthServiceGenerationGate(const AuthenticationService& auth,
                                       ProviderResolver providerResolver = {});

    [[nodiscard]] Result<GenerationAuthorization> authorize(
        const GenerationRequest& request) const override;

private:
    const AuthenticationService& auth_;
    ProviderResolver             providerResolver_;
};

/// IGenerationRunner that drives a real GenerativeClient submit -> poll -> fetch
/// to completion. Between non-terminal polls it invokes `pacer` (default: none)
/// so the caller can yield/sleep; `maxPollAttempts` bounds the loop as a safety
/// net (the client's own 300-second budget is the primary timeout, Req 6.8).
class GenerativeClientRunner : public IGenerationRunner {
public:
    explicit GenerativeClientRunner(GenerativeClient& client,
                                    std::function<void()> pacer = {},
                                    std::size_t maxPollAttempts = 100000);

    [[nodiscard]] Result<MediaAsset> generate(const GenerationRequest& request,
                                              std::string_view authToken) override;

private:
    GenerativeClient&     client_;
    std::function<void()> pacer_;
    std::size_t           maxPollAttempts_;
};

/// ITimelinePlacement backed by the real TimelineEngine (Req 6.2). Converts the
/// user-specified frame position to a timeline Duration using the project frame
/// rate, validates it lies within [0, current timeline duration], and applies an
/// atomic add-clip command. Placement that would violate a timeline invariant
/// (e.g. overlapping an existing clip) is rejected and leaves the timeline
/// unchanged (guaranteed by TimelineEngine::apply).
class TimelineEnginePlacer : public ITimelinePlacement {
public:
    explicit TimelineEnginePlacer(TimelineEngine& engine);

    [[nodiscard]] Result<GeneratedMediaPlacement> place(
        const TimelinePlacementRequest& request) override;

    [[nodiscard]] bool trackExists(const Uuid& trackId) const override;

private:
    TimelineEngine& engine_;
};

} // namespace palmier::services

#endif // PALMIER_SERVICES_GENERATIVEMEDIACOORDINATOR_HPP
