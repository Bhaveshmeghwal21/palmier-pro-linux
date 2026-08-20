// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/GenerativeClient.hpp — the in-timeline generative AI client
// (design.md "Component 5: Generative AI Client & Auth"; Requirement 6).
//
// Palmier's generative features (image/video generation from a prompt) are a
// closed-source *hosted* capability: the Linux client does not run inference
// locally, it calls the same hosted backend the macOS edition uses, over TLS,
// authorized by the bearer token from an authenticated Session (see
// AuthenticationService::Session). This component owns the *client-side*
// submit -> poll -> fetch job lifecycle and the editor-side policy the
// requirements pin down, independent of any particular transport:
//
//   * 6.1 — submit a prompt with a selected SOTA model and obtain generated
//           media of the requested type (video or image) within a 300-second
//           budget (kGenerationBudgetSeconds).
//   * 6.3 — a video model produces a video asset.
//   * 6.4 — an image model produces an image asset.
//   * 6.6 — a provider-side failure surfaces a descriptive error and leaves
//           project state unchanged (this client performs no project mutation,
//           so a failed submit/poll/fetch is inherently non-mutating; placement
//           into the library/timeline is task 14.2).
//   * 6.8 — a job that does not complete within 300 seconds is cancelled and
//           reported as a timeout, again leaving project state unchanged.
//
// The hosted transport is abstracted behind IGenerativeBackend so the whole
// submit/poll/fetch/timeout/cancel mechanic is unit-testable with a mock and an
// injectable clock — no real network. The auth token is captured at submit()
// and reused for the job's subsequent poll()/fetchResult()/cancel() calls, which
// is why those take only a JobId (matching the design interface). This header
// depends only on the domain core (Result/Error, Duration, Uuid/MediaAssetRef),
// so it compiles and tests on any platform.
//
// Out of scope for this component (task 14.1): prompt length/emptiness
// validation, subscription/BYOK entitlement gating, and placement of the
// generated media into the library and onto the timeline — those are task 14.2 /
// 14.4. The token check here is a transport-level guard (a bearer is required to
// call the hosted backend), not the entitlement-gating UX.

#ifndef PALMIER_SERVICES_GENERATIVECLIENT_HPP
#define PALMIER_SERVICES_GENERATIVECLIENT_HPP

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "core/Duration.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/Resolution.hpp"
#include "core/Result.hpp"
#include "core/Uuid.hpp"

namespace palmier::services {

// ---------------------------------------------------------------------------
// Request / job value types
// ---------------------------------------------------------------------------

/// The kind of media a generation request targets. A request selects exactly
/// one; the returned MediaAsset must match it (Requirements 6.1, 6.3, 6.4, 12).
enum class GenerationMediaType {
    Video, ///< A supported video SOTA model (Requirement 6.3).
    Image, ///< A supported image SOTA model (Requirement 6.4).
    Audio, ///< A supported audio SOTA model (PR 395; Requirement 14).
};

/// The generation mode a request selects (PR 396; Requirement 14). Every
/// existing request is `Generate` (the default), so this is purely additive:
/// nothing that predates PR 396 sets it, and the coordinator treats an absent
/// mode identically to `Generate`.
enum class GenerationMode {
    Generate, ///< Produce media from `prompt` (and optionally `sourceAssetId`).
    Upscale,  ///< Upscale `sourceAssetId` to `targetResolution` (PR 396).
};

/// Stable lowercase name for a GenerationMode ("generate"/"upscale").
[[nodiscard]] std::string_view toStringView(GenerationMode mode) noexcept;

/// Parse a lowercase mode name; std::nullopt for anything else, including case
/// variants (the tool schema's declared enum values are the only accepted
/// spellings, matching every other enum this project parses at the boundary).
[[nodiscard]] std::optional<GenerationMode> generationModeFromStringView(
    std::string_view text) noexcept;

/// Stable lowercase name for a GenerationMediaType.
[[nodiscard]] std::string_view toStringView(GenerationMediaType type) noexcept;

/// A generation request: the selected SOTA model, the media type it produces,
/// the user's prompt, and any model-specific parameters (e.g. resolution,
/// duration, seed). Prompt-length/emptiness validation is intentionally NOT
/// performed here — that is task 14.2 / 14.4.
///
/// `mode`, `sourceAssetId`, `targetResolution` and `requestedDuration` are all
/// additive (PR 396, PR 395): a plain prompt-to-video/image request leaves every
/// one of them at its default and behaves exactly as before their addition.
struct GenerationRequest {
    std::string                        model;     ///< SOTA model id (e.g. "veo", "gpt-image").
    GenerationMediaType                mediaType = GenerationMediaType::Video;
    GenerationMode                     mode = GenerationMode::Generate;
    std::string                        prompt;    ///< The user's generation prompt.
    std::map<std::string, std::string> params;    ///< Model-specific parameters.

    /// The source clip being upscaled (`mode == Upscale`) or, for audio, the
    /// clip audio is being generated FROM rather than from `prompt` alone (PR
    /// 395's "either a source clip or a text prompt"). Nil when the request has
    /// no source, which is every request that predates PR 395/396.
    Uuid sourceAssetId;

    /// The requested output resolution for `mode == Upscale`. Invalid (zero) for
    /// every other request.
    Resolution targetResolution;

    /// The requested audio duration for `mediaType == Audio`. Zero for every
    /// non-audio request.
    Duration requestedDuration = Duration::zero();
};

/// Opaque identifier for a submitted generation job, assigned by the backend.
/// Ordered so it can key a std::map without a bespoke hash.
struct JobId {
    std::string value;

    [[nodiscard]] bool empty() const noexcept { return value.empty(); }

    [[nodiscard]] friend auto operator<=>(const JobId&, const JobId&) = default;
    [[nodiscard]] friend bool operator==(const JobId&, const JobId&) = default;
};

/// A handle to a submitted job, returned by submit(). Echoes the essentials of
/// the request so callers can track the job without retaining the request.
struct GenerationJob {
    JobId               id;
    std::string         model;
    GenerationMediaType mediaType = GenerationMediaType::Video;
};

/// The lifecycle phase of a generation job as reported by the backend.
enum class GenerationPhase {
    Pending,   ///< Accepted, not yet started.
    Running,   ///< In progress at the provider.
    Succeeded, ///< Completed; a result asset is available via fetchResult().
    Failed,    ///< Failed at the provider (see failureReason) — Requirement 6.6.
};

/// Stable lowercase name for a GenerationPhase.
[[nodiscard]] std::string_view toStringView(GenerationPhase phase) noexcept;

/// A snapshot of a job's progress, returned by poll(). For a Failed phase
/// `failureReason` carries the provider's descriptive reason (Requirement 6.6).
struct GenerationStatus {
    GenerationPhase phase = GenerationPhase::Pending;
    int             progressPercent = 0;   ///< 0-100, best-effort provider progress.
    std::string     failureReason;         ///< Non-empty only when phase == Failed.

    /// True once the job has reached a terminal phase (Succeeded or Failed).
    [[nodiscard]] bool isTerminal() const noexcept {
        return phase == GenerationPhase::Succeeded || phase == GenerationPhase::Failed;
    }
};

/// The generated media produced by a successful job, ready to be downloaded into
/// the media browser (design.md). Carries a project-scoped asset reference and
/// the media type it satisfies (which must match the request — 6.1/6.3/6.4).
/// Actually adding it to the library / placing it on the timeline is task 14.2.
struct MediaAsset {
    MediaAssetRef       ref;                                  ///< Downloaded asset identity + locator.
    GenerationMediaType mediaType = GenerationMediaType::Video;
};

// ---------------------------------------------------------------------------
// Hosted-backend transport seam
// ---------------------------------------------------------------------------

/// The hosted (closed-source) generative backend, abstracted so the client's
/// submit/poll/fetch/timeout/cancel policy is testable with a mock and no real
/// network. Every call carries the bearer `authToken` so the transport reflects
/// "over TLS using the auth token".
///
/// Contract the client relies on:
///   * submit  -> Ok(JobId) on acceptance, or an Error (e.g. Unauthenticated,
///                Timeout, Io) that the client forwards unchanged.
///   * poll    -> Ok(GenerationStatus) for the job (possibly a Failed status
///                carrying a provider reason), or an Error the client forwards.
///   * fetchResult -> Ok(MediaAsset) once the job has succeeded, else an Error.
///   * cancel  -> best-effort abort of an in-flight job (used on timeout, 6.8).
class IGenerativeBackend {
public:
    virtual ~IGenerativeBackend() = default;

    [[nodiscard]] virtual Result<JobId> submit(const GenerationRequest& request,
                                               std::string_view authToken) = 0;
    [[nodiscard]] virtual Result<GenerationStatus> poll(const JobId& id,
                                                        std::string_view authToken) = 0;
    [[nodiscard]] virtual Result<MediaAsset> fetchResult(const JobId& id,
                                                         std::string_view authToken) = 0;
    [[nodiscard]] virtual Result<void> cancel(const JobId& id,
                                              std::string_view authToken) = 0;
};

// ---------------------------------------------------------------------------
// GenerativeClient
// ---------------------------------------------------------------------------

/// Owns the client-side generation job lifecycle against the hosted backend.
/// The `backend` reference must outlive the client.
///
/// Thread-affinity: instances are not internally synchronized; callers sharing
/// one across threads must provide external synchronization.
class GenerativeClient {
public:
    /// The end-to-end generation budget: a job that has not completed within
    /// this many wall-clock seconds is cancelled and reported as a timeout
    /// (Requirements 6.1, 6.8).
    static constexpr std::int64_t kGenerationBudgetSeconds = 300;

    /// Monotonic clock used for the timeout budget. Injectable so tests advance
    /// time deterministically; defaults to std::chrono::steady_clock::now.
    using Clock     = std::function<std::chrono::steady_clock::time_point()>;
    using TimePoint = std::chrono::steady_clock::time_point;

    /// `backend` must outlive this client. `budget` defaults to the 300-second
    /// requirement and is overridable for tests; a default-constructed `clock`
    /// uses std::chrono::steady_clock.
    explicit GenerativeClient(
        IGenerativeBackend& backend,
        std::chrono::milliseconds budget =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::seconds(kGenerationBudgetSeconds)),
        Clock clock = {});

    /// Submit a generation `request`, authorized by `authToken` (the bearer from
    /// an authenticated Session). On acceptance the backend job id is recorded
    /// (with the token and submission time, for timeout enforcement) and a
    /// GenerationJob handle is returned (Requirements 6.1, 6.3, 6.4).
    ///
    /// A blank `authToken` is rejected with Unauthenticated without contacting
    /// the backend (a bearer is required to call the hosted service). A backend
    /// error is forwarded unchanged and no job is recorded.
    [[nodiscard]] Result<GenerationJob> submit(const GenerationRequest& request,
                                               std::string_view authToken);

    /// Poll the status of a previously submitted job.
    ///
    ///   * Unknown id -> NotFound.
    ///   * If the 300-second budget has elapsed and the job is not yet terminal,
    ///     the job is cancelled at the backend (best-effort) and a Timeout error
    ///     is returned (Requirement 6.8).
    ///   * Otherwise the backend is polled with the stored token; a provider
    ///     Error is forwarded unchanged (Requirement 6.6) and the last known
    ///     status is cached (a Failed status carries the provider reason).
    [[nodiscard]] Result<GenerationStatus> poll(const JobId& id);

    /// Fetch the generated media for a job.
    ///
    ///   * Unknown id -> NotFound.
    ///   * Budget elapsed while non-terminal -> cancel + Timeout (Requirement 6.8).
    ///   * Job Failed at the provider -> an error carrying the failure reason,
    ///     leaving project state unchanged (Requirement 6.6).
    ///   * Job not yet Succeeded -> FailedPrecondition.
    ///   * Job Succeeded -> the backend's MediaAsset, whose media type matches the
    ///     request (Requirements 6.1, 6.3, 6.4).
    [[nodiscard]] Result<MediaAsset> fetchResult(const JobId& id);

    /// Explicitly cancel a job (best-effort). Unknown id -> NotFound. Cancelling
    /// an already-succeeded or already-cancelled job is a no-op success.
    [[nodiscard]] Result<void> cancel(const JobId& id);

    /// True iff `id` names a job this client is currently tracking.
    [[nodiscard]] bool tracks(const JobId& id) const;

    /// The end-to-end generation budget as a Duration (Requirements 6.1, 6.8).
    [[nodiscard]] static Duration generationBudget() {
        return Duration::fromSeconds(static_cast<double>(kGenerationBudgetSeconds));
    }

private:
    /// Per-job bookkeeping needed to poll/fetch/cancel and enforce the budget.
    struct JobRecord {
        std::string         token;        ///< Bearer captured at submit, reused for the job.
        TimePoint           submittedAt;  ///< For the 300-second budget.
        GenerationMediaType mediaType = GenerationMediaType::Video;
        std::string         model;
        bool                cancelled = false;
        GenerationStatus    lastStatus;   ///< Most recent known status (cached once terminal).
    };

    /// True iff the elapsed wall-clock since submission has reached the budget.
    [[nodiscard]] bool budgetExceeded(const JobRecord& record) const;

    /// The timeout failure for `id`: it identifies the job and states the elapsed
    /// limit — the CONFIGURED budget, not the default constant, so a client built
    /// with a shorter or longer budget reports the limit it actually applied
    /// (Requirements 6.8, 12.10).
    [[nodiscard]] Error timedOutError(const JobId& id) const;

    /// Cancel `record` at the backend (best-effort), mark it timed-out, and
    /// return the Timeout error (Requirement 6.8). Idempotent per record.
    [[nodiscard]] Error cancelForTimeout(JobRecord& record, const JobId& id);

    IGenerativeBackend&           backend_;
    std::chrono::milliseconds     budget_;
    Clock                         clock_;
    std::map<JobId, JobRecord>    jobs_;
};

} // namespace palmier::services

#endif // PALMIER_SERVICES_GENERATIVECLIENT_HPP
