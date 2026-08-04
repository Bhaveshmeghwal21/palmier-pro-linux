// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/ExportCoordinator.hpp — the Export_Coordinator (task 9.4; design.md
// "Components and interfaces → Media, playback, audio, export"; Requirements
// 6.5, 6.10, 6.11, 7.1–7.7, 7.9, 7.10, 7.11, 8.11).
//
// This is the one component that turns "the user (or the agent) asked for an
// export" into a running encode, and it is the component every one of the
// export requirements is written against. It owns four things and nothing else:
//
//   1. **Admission.** `validate()` is a PURE STATIC function over the request. It
//      is called before anything is created, which is what makes "create no file
//      at the requested path" literally true for every rejection (Requirements
//      7.6, 7.9, 7.11) rather than a cleanup promise. `begin()` additionally
//      rejects an empty timeline (Requirement 7.6) and a second concurrent
//      request (Requirement 7.10) before it starts a thread.
//   2. **Isolation.** One `std::thread` per export, working from a VALUE-COPY
//      `Project` snapshot taken on the calling thread, with an export-local
//      `gpu::GpuContext`, `gpu::Compositor`, clip-frame provider (export-local
//      decoders), audio mix and `media::MediaEncoder`. The worker holds NO
//      reference to the ProjectSession, the live TimelineEngine, the live
//      compositor or the live decoders. Requirement 7.2's "leaves the project
//      state unchanged" is therefore structural: there is no path from the
//      worker to the session to change. This is also why an edit made while an
//      export runs cannot alter the exported result.
//   3. **Reporting.** Progress is produced on the worker and MARSHALLED to the
//      owning (main) thread: the worker only appends to a mutex-guarded queue and
//      wakes the owner through a notifier, and `pump()` delivers the queued
//      reports and the final outcome on the calling thread (Requirement 7.3).
//      That is what keeps the Editor_Shell free to process window events — the
//      export never calls into the UI from the worker, and the main thread is
//      never blocked waiting for it. Percentages are clamped to be
//      non-decreasing and are emitted at intervals of at most one second,
//      measured on an INJECTED steady clock, so the cadence is asserted exactly
//      rather than slept for.
//   4. **Cleanup.** A scope guard on the worker owns the output path. On ANY
//      non-success exit — a validation-independent failure, a cancellation, an
//      audio encode/mux failure or a mid-export hardware encode failure — it
//      calls `MediaEncoder::finish()` best-effort (releasing the file handle) and
//      then removes the output path, so no file remains where the export was
//      requested (Requirements 6.10, 7.5, 7.7, 8.11). Success is the only path
//      that commits the guard.
//
// Encoder choice is NOT reimplemented here: `media::EncoderSelector` (task 9.1)
// owns the request/compiled-in/capability gate, the bounded capability probe and
// the retry-hardware-once-then-fall-back rule, and its single `EncoderSelection`
// value is what the outcome reports (Requirements 8.2, 8.3, 8.4, 8.8).
//
// ## Cancellation is deterministic, not timing-dependent
//
// `cancel()` sets an atomic flag. The render loop consults it through the cancel
// predicate on `media::ExportRequest` at the top of every frame, so the export
// stops at a frame boundary — the next one after the flag is observed — and the
// guard then removes the partial file. Nothing polls, nothing sleeps and no
// deadline decides the outcome. A test makes the cancellation point exact by
// calling `cancel()` from inside its own encode backend on a chosen frame: the
// flag is then set before the loop's next check, so the export always stops at
// that frame. Every wait this class exposes is BOUNDED
// (`waitForCompletion(timeout)`), so a coordinator that fails to finish makes a
// test fail rather than hang.
//
// ## Testability
//
// Every collaborator an export needs is injectable through `Options`: the steady
// clock, the encode backend factory, the audio range renderer, the clip-frame
// provider, the decode backend factory, the GPU context factory and the encoder
// selector's own options. A test therefore drives a complete export — including
// the mid-export failure, hardware-failure, audio-failure and cancellation paths
// — with no GPU, no vendor SDK, no FFmpeg, no media fixture and no sleeping,
// while still writing and removing REAL files at a real path, which is the only
// way "no file remains" can be checked.

#ifndef PALMIER_SERVICES_EXPORTCOORDINATOR_HPP
#define PALMIER_SERVICES_EXPORTCOORDINATOR_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "core/Duration.hpp"
#include "core/FrameRate.hpp"
#include "core/Project.hpp"
#include "core/Resolution.hpp"
#include "core/Result.hpp"
#include "gpu/Compositor.hpp"
#include "gpu/GpuContext.hpp"
#include "gpu/GpuTypes.hpp"
#include "media/AudioSink.hpp"      // SteadyClock
#include "media/DecoderTeardownQueue.hpp"
#include "media/EncoderSelector.hpp"
#include "media/ExportEngine.hpp"
#include "media/MediaDecoder.hpp"   // DecodeBackendFactory
#include "media/MediaEncoder.hpp"
#include "services/ProjectSession.hpp"

namespace palmier::services {

// ---------------------------------------------------------------------------
// Accepted request ranges (Requirements 7.1, 7.9)
// ---------------------------------------------------------------------------
//
// These are the bounds `validate()` enforces and the bounds its error messages
// quote back, so the accepted range a caller is told about and the range that is
// checked are one definition.

inline constexpr std::size_t   kMaxExportPathLength = 4096;
inline constexpr std::uint32_t kMinExportWidth = 128;
inline constexpr std::uint32_t kMaxExportWidth = 3840;
inline constexpr std::uint32_t kMinExportHeight = 128;
inline constexpr std::uint32_t kMaxExportHeight = 2160;
inline constexpr double        kMinExportFps = 1.0;
inline constexpr double        kMaxExportFps = 120.0;
inline constexpr std::int64_t  kMinExportBitrateKbps = 100;
inline constexpr std::int64_t  kMaxExportBitrateKbps = 200'000;

/// The container short-names the Export_Coordinator accepts, in the order they
/// are quoted in a rejection message.
[[nodiscard]] bool isSupportedExportContainer(const std::string& container);

/// The video codecs the Encoder_Selector supports for export: H.264, HEVC, VP9
/// (Requirement 8.2).
[[nodiscard]] bool isSupportedExportCodec(gpu::CodecId codec) noexcept;

// ---------------------------------------------------------------------------
// ExportRequest2 — what the dialog or the `timeline.export` tool asked for
// ---------------------------------------------------------------------------

/// One export request. Named `ExportRequest2` because `media::ExportRequest` is
/// the engine-level request this is translated into; this is the SERVICE-level
/// request, expressed in the units the dialog and the tool schema use (kilobits
/// per second, a container short-name, an explicit overwrite acknowledgement).
struct ExportRequest2 {
    std::filesystem::path outputPath{};
    std::string           container{"mp4"};                ///< mp4 | mov | mkv | webm
    gpu::CodecId          codec{gpu::CodecId::H264};        ///< H264 | HEVC | VP9
    Resolution            resolution{Resolution::hd1080()}; ///< 128..3840 x 128..2160
    FrameRate             frameRate{FrameRate::fps30()};    ///< 1..120 fps
    std::int64_t          bitrateKbps{8'000};               ///< 100..200000
    bool                  includeAudio{true};
    bool                  preferHardware{true};
    /// The explicit acknowledgement Requirement 7.11 demands before an existing
    /// destination may be replaced. Absent it, an existing file is preserved
    /// byte-for-byte and the request is rejected.
    bool                  overwrite{false};
};

// ---------------------------------------------------------------------------
// ExportOutcome — what happened
// ---------------------------------------------------------------------------

/// The result of a completed (or cancelled) export. Everything the
/// `timeline.export` tool must return is here (Requirement 7.2).
struct ExportOutcome {
    std::filesystem::path outputPath{};
    std::size_t           framesEncoded{0};
    /// `media::ExportEngine::plannedFrameCount` for the exported timeline and
    /// frame rate — the count a successful export must match (Requirement 7.4).
    std::size_t           plannedFrames{0};
    std::string           encoderName{};
    bool                  usedHardwareEncode{false};
    bool                  usedSoftwareFallback{false};
    std::string           fallbackReason{};
    bool                  containsAudio{false};
    /// Audio FRAMES (one sample per channel) written, at the audio stream's
    /// sample rate. Zero for a video-only export.
    std::uint64_t         audioFrames{0};
    /// The exported timeline duration: `plannedFrames` frame intervals.
    Duration              duration{Duration::zero()};
    bool                  cancelled{false};
    /// The project as the export left it. FALSE for every outcome: the worker
    /// runs on a value-copy snapshot and holds no session reference, so there is
    /// no path by which an export could modify the project (Requirements 7.1,
    /// 7.5, 7.7, 8.11). Reported so a caller can assert it rather than assume it.
    bool                  projectModified{false};
    /// The session revision when the snapshot was taken, so a caller can attribute
    /// any later revision change to its own edits rather than to the export.
    std::uint64_t         projectRevisionAtStart{0};
};

// ---------------------------------------------------------------------------
// ExportProgressReport — one marshalled progress notification
// ---------------------------------------------------------------------------

/// A progress report as delivered to the owning thread. `percent` is
/// non-decreasing across the reports of one export and lies in [0, 100]
/// (Requirement 7.3).
struct ExportProgressReport {
    int           percent{0};
    std::size_t   framesEncoded{0};
    std::size_t   totalFrames{0};
    Duration      position{Duration::zero()};
};

// ---------------------------------------------------------------------------
// ExportCoordinator
// ---------------------------------------------------------------------------

/// Injectable collaborators. Every one of them has a production default, and
/// every one of them is what lets an export be driven end to end on a host with
/// no GPU, no vendor SDK and no FFmpeg.
struct ExportCoordinatorOptions {
    /// The maximum interval between marshalled progress reports. Requirement 7.3
    /// fixes the ceiling at one second; a value above that is clamped down to it,
    /// so the guarantee cannot be configured away.
    std::chrono::milliseconds progressInterval{std::chrono::seconds{1}};

    /// The clock the progress cadence is measured on. Empty → the system steady
    /// clock. A test injects a manual clock and advances it by hand, which is why
    /// no test of the ≤1 s rule sleeps.
    media::SteadyClock clock{};

    /// The encode backend. Empty → `media::ffmpegEncodeBackendFactory()`.
    media::EncodeBackendFactory encodeFactory{};

    /// The export audio mix. Empty → an export-local `AudioGraph` producing
    /// silence for the range, which is precisely Requirement 6.11's outcome; the
    /// composition root binds this to `media::AudioEngine::renderRange`.
    media::AudioRangeRenderer audioRenderer{};

    /// The source pixels for visible clips. Empty → an EXPORT-LOCAL
    /// `media::DecoderClipFrameProvider` built on the teardown queue and
    /// `decodeFactory`, so the export never shares a decoder with playback.
    gpu::ClipFrameProvider frameProvider{};

    /// The decode backend the export-local decoders open through. Empty → the
    /// FFmpeg factory.
    media::DecodeBackendFactory decodeFactory{};

    /// Builds the EXPORT-LOCAL GPU context on the worker. Empty → the software
    /// fallback when the live context is the software fallback, otherwise a fresh
    /// context created with `policy`.
    std::function<Result<gpu::GpuContext>()> gpuContextFactory{};

    /// The GPU selection policy the default context factory uses.
    gpu::GpuSelectionPolicy policy{gpu::GpuSelectionPolicy::automatic()};

    /// Passed straight to `media::EncoderSelector` (task 9.1). A test injects a
    /// virtual-time probe awaiter here.
    media::EncoderSelectorOptions selector{};

    /// The device capabilities encoder selection is made against. Unset → read
    /// from the live `gpu::GpuContext`.
    ///
    /// Supplied as a VALUE for the same reason `media::EncoderSelector` takes one:
    /// the hardware branches — including the mid-export hardware encode failure of
    /// Requirement 8.11 — must be exercisable on a host with neither a GPU nor a
    /// vendor SDK, which is this project's CI and every container it runs in.
    std::optional<gpu::GpuCaps> caps{};

    /// The compiled-in vendor hardware paths. Unset →
    /// `gpu::BridgeAvailability::fromBuildConfig()`.
    std::optional<gpu::BridgeAvailability> availability{};

    /// The audio stream configuration for an export that includes audio. Defaults
    /// to the Audio_Engine output format (48 kHz, 2 channels, AAC).
    media::AudioEncodeSpec audio{};

    /// Bytes an export-local decoder cache may hold — the export-local decoder
    /// budget, independent of playback's.
    std::size_t decoderCacheCapacity{4};
};

class ExportCoordinator {
public:
    using Options = ExportCoordinatorOptions;

    /// Receives marshalled progress on the owning thread (Requirement 7.3).
    using ProgressSink = std::function<void(const ExportProgressReport&)>;

    /// Receives the final outcome on the owning thread: the ExportOutcome on
    /// success or a cancellation, an Error otherwise.
    using CompletionSink = std::function<void(const Result<ExportOutcome>&)>;

    /// Called ON THE WORKER THREAD once something has been queued, so the owner
    /// can schedule `pump()` on its own thread (the Qt composition posts to the
    /// main thread). Must be thread-safe. May be empty, in which case the owner
    /// pumps on its own cadence.
    using Notifier = std::function<void()>;

    ExportCoordinator(ProjectSession& session, gpu::GpuContext& liveContext,
                      media::DecoderTeardownQueue& teardown, Options options = {});

    /// Requests cancellation and joins the worker. Nothing is delivered during
    /// destruction; the scope guard still removes any partial output.
    ~ExportCoordinator();

    ExportCoordinator(const ExportCoordinator&)            = delete;
    ExportCoordinator& operator=(const ExportCoordinator&) = delete;
    ExportCoordinator(ExportCoordinator&&)                 = delete;
    ExportCoordinator& operator=(ExportCoordinator&&)      = delete;

    // --- Admission ---------------------------------------------------------

    /// Validate `request` WITHOUT creating anything (Requirements 7.6, 7.9,
    /// 7.11). Pure in the sense that matters: it reads the request and the
    /// filesystem and writes nothing, so a rejection provably leaves no file at
    /// the requested path and an existing destination byte-for-byte intact.
    ///
    /// Rejections, each naming the offending parameter together with its accepted
    /// range or supported values:
    ///   * InvalidArgument — an empty output path.
    ///   * OutOfRange      — a path longer than 4096 characters; a resolution,
    ///                       frame rate or bit rate outside its range.
    ///   * NotFound        — the parent directory does not exist.
    ///   * PermissionDenied— the parent directory is not writable.
    ///   * Unsupported     — an unsupported container or video codec.
    ///   * AlreadyExists   — the destination exists and the request carries no
    ///                       overwrite acknowledgement (Requirement 7.11).
    [[nodiscard]] static Result<void> validate(const ExportRequest2& request);

    /// Reject a timeline with zero media segments (Requirement 7.6). Separate from
    /// `validate()` because it is a fact about the project rather than about the
    /// request; `begin()` calls both before anything is created.
    [[nodiscard]] static Result<void> validateTimeline(const Project& project);

    // --- Running an export -------------------------------------------------

    /// Start an export. Returns an error — having started no thread and created
    /// no file — when `validate()` or `validateTimeline()` rejects, or when an
    /// export is already running (Requirement 7.10: the running export and its
    /// progress reporting are untouched by the rejected request).
    ///
    /// On success exactly one worker thread is running. `progress` and
    /// `completion` are invoked from `pump()` on the thread that calls it, never
    /// from the worker.
    [[nodiscard]] Result<void> begin(const ExportRequest2& request, ProgressSink progress = {},
                                     CompletionSink completion = {});

    /// Request cancellation of the running export (Requirement 7.7). Returns
    /// immediately; the worker stops at its next frame boundary, the guard removes
    /// the partial output, and the outcome reports `cancelled`. Harmless when no
    /// export is running.
    void cancel();

    /// True while a worker is running (from a successful `begin()` until the
    /// worker has queued its outcome).
    [[nodiscard]] bool running() const noexcept { return running_.load(); }

    /// True once `cancel()` has been called for the current export.
    [[nodiscard]] bool cancelRequested() const noexcept { return cancelRequested_.load(); }

    // --- Marshalling to the owning thread ----------------------------------

    void setNotifier(Notifier notifier);

    /// Deliver every queued progress report, and the outcome if it has arrived, on
    /// the CALLING thread. Returns the number of notifications delivered.
    [[nodiscard]] std::size_t pump();

    /// Wait — BOUNDED — until the worker has queued its outcome. Returns false on
    /// timeout, so a coordinator that fails to finish makes a caller (and a test)
    /// fail rather than hang. Returns true immediately when no export is running
    /// and none is pending delivery.
    [[nodiscard]] bool waitForCompletion(std::chrono::milliseconds timeout);

    /// `waitForCompletion(timeout)` followed by `pump()`; returns the number of
    /// notifications delivered, or 0 on timeout.
    [[nodiscard]] std::size_t awaitCompletion(std::chrono::milliseconds timeout);

    // --- Observability ------------------------------------------------------

    /// The outcome of the most recently DELIVERED export, or nullopt.
    [[nodiscard]] const std::optional<ExportOutcome>& lastOutcome() const noexcept {
        return lastOutcome_;
    }

    /// The error of the most recently DELIVERED failed export, or nullopt.
    [[nodiscard]] const std::optional<Error>& lastError() const noexcept { return lastError_; }

    /// Every progress report delivered for the most recent export, in order.
    [[nodiscard]] const std::vector<ExportProgressReport>& deliveredProgress() const noexcept {
        return deliveredProgress_;
    }

    /// The effective progress interval — the requested one, clamped to at most one
    /// second (Requirement 7.3).
    [[nodiscard]] std::chrono::milliseconds progressInterval() const noexcept {
        return options_.progressInterval;
    }

    /// The encoder selection made for the most recent export, or nullopt.
    [[nodiscard]] const std::optional<media::SelectionOutcome>& lastSelection() const noexcept {
        return lastSelection_;
    }

private:
    /// The queued outcome of one export, appended by the worker and applied by the
    /// owning thread.
    struct QueuedOutcome {
        bool                                   succeeded{false};
        ExportOutcome                          outcome{};
        Error                                  error{};
        /// The selection the worker made, carried across the thread boundary in
        /// the queue rather than published directly, so no observability field is
        /// written by the worker and read by the owner.
        std::optional<media::SelectionOutcome>  selection{};
    };

    /// Everything the worker needs, all of it OWNED BY THE WORKER: a value-copy
    /// project snapshot and copies of the request and the sinks' parameters. There
    /// is deliberately no ProjectSession, TimelineEngine, live Compositor or live
    /// decoder here — that absence is Requirement 7.2.
    struct WorkerInput {
        Project        snapshot{};
        ExportRequest2 request{};
        std::uint64_t  revisionAtStart{0};
    };

    /// The worker body. Runs on its own thread and touches only `input`, the
    /// options, the teardown queue and the queues below.
    void runExport(WorkerInput input);

    /// Queue a progress report if the cadence allows, clamping the percentage to
    /// be non-decreasing. Called on the worker.
    void offerProgress(int percent, std::size_t framesEncoded, std::size_t totalFrames,
                       Duration position, bool force);

    /// Queue the outcome, mark the export finished and wake the owner. Called on
    /// the worker as its last action.
    void finishExport(QueuedOutcome outcome);

    void joinWorker();

    ProjectSession&              session_;
    gpu::GpuContext&             liveContext_;
    media::DecoderTeardownQueue& teardown_;
    Options                      options_;

    std::atomic<bool> running_{false};
    std::atomic<bool> cancelRequested_{false};

    mutable std::mutex                mutex_;
    std::condition_variable           cv_;
    std::deque<ExportProgressReport>  progressQueue_{};
    std::optional<QueuedOutcome>      outcomeQueue_{};
    Notifier                          notifier_{};
    ProgressSink                      progressSink_{};
    CompletionSink                    completionSink_{};

    /// Progress cadence state, worker-only.
    int                                                  lastPercent_{-1};
    std::optional<std::chrono::steady_clock::time_point>  lastEmitAt_{};

    /// Delivered state, owning-thread-only.
    std::optional<ExportOutcome>            lastOutcome_{};
    std::optional<Error>                    lastError_{};
    std::optional<media::SelectionOutcome>  lastSelection_{};
    std::vector<ExportProgressReport>       deliveredProgress_{};

    std::thread worker_{};
};

} // namespace palmier::services

#endif // PALMIER_SERVICES_EXPORTCOORDINATOR_HPP
