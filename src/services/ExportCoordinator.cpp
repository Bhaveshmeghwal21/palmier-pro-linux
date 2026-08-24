// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/ExportCoordinator.cpp — admission, isolation, reporting and cleanup
// for one export (task 9.4; Requirements 6.5, 6.10, 6.11, 7.1–7.7, 7.9, 7.10,
// 7.11, 8.11).
//
// The file is organized exactly as the four responsibilities in the header:
//
//   * validate() / validateTimeline() — the pure admission checks, run before
//     anything is created, so a rejection cannot have left a file behind.
//   * begin() — the one-export-at-a-time gate, the value-copy snapshot, and the
//     thread launch.
//   * runExport() — the worker: an export-local GPU context, compositor,
//     clip-frame provider and encoder; encoder choice delegated to
//     media::EncoderSelector; the scope guard that owns the output path.
//   * offerProgress() / finishExport() / pump() — the marshalling boundary.
//
// GuardedBackend, below, is the small piece of machinery that makes three
// separate requirements observable through one mechanism. It wraps whatever
// IEncodeBackend the configured factory produced and publishes, through a shared
// control block that outlives it:
//
//   * how many video frames and audio blocks actually reached the encoder — which
//     is what distinguishes "hardware failed before the first frame" (a software
//     fallback, Requirement 8.3) from "hardware failed mid-export" (a hard
//     failure, Requirement 8.11);
//   * which stage failed, so an audio encode/mux failure is reported as such
//     (Requirement 6.10);
//   * a still-open backend, so the scope guard can call finish() best-effort to
//     release the output file handle before removing the file (Requirement 7.5).

#include "services/ExportCoordinator.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

#include <unistd.h> // access(), W_OK — the parent-directory writability check

#include "core/TimelineEngine.hpp" // timelineDuration()
#include "media/AudioGraph.hpp"
#include "media/DecoderClipFrameProvider.hpp"
#include "services/CaptionExport.hpp"

namespace palmier::services {

namespace {

/// Lower-case an ASCII string, for case-insensitive container matching.
[[nodiscard]] std::string toLowerAscii(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

/// A stable, human-readable codec name for rejection messages.
[[nodiscard]] std::string codecLabel(gpu::CodecId codec) {
    switch (codec) {
        case gpu::CodecId::H264:    return "H.264";
        case gpu::CodecId::HEVC:    return "HEVC";
        case gpu::CodecId::VP9:     return "VP9";
        case gpu::CodecId::AV1:     return "AV1";
        case gpu::CodecId::MPEG2:   return "MPEG-2";
        case gpu::CodecId::Unknown: return "unknown";
    }
    return "unknown";
}

/// The media-layer container short-name for a service-level container value.
[[nodiscard]] std::string containerFormatFor(const std::string& container) {
    return toLowerAscii(container);
}

/// The stage of an export a failure is attributed to, for the error message
/// Requirements 6.10, 7.5 and 8.11 each require.
enum class FailingStage {
    None,
    Video,  ///< video encode / mux.
    Audio,  ///< audio encode / mux (Requirement 6.10).
    Finish, ///< flushing the streams or writing the trailer.
};

/// The control block a GuardedBackend publishes into. Outlives the backend, so the
/// scope guard can consult it after MediaEncoder has destroyed the backend.
struct EncodeObservation {
    std::mutex           mutex{};
    media::IEncodeBackend* live{nullptr}; ///< non-null while the backend exists.
    bool                 finished{false};
    std::size_t          videoFrames{0};
    std::size_t          audioBlocks{0};
    FailingStage         failingStage{FailingStage::None};

    /// Best-effort finish, used by the scope guard to release the output file
    /// handle before the file is removed (Requirement 7.5). A no-op once the
    /// backend has been finished or destroyed — which is the normal case, because
    /// ExportEngine::run finishes or destroys it before returning.
    void finishBestEffort() {
        std::lock_guard<std::mutex> lock(mutex);
        if (live == nullptr || finished) return;
        finished = true;
        (void)live->finish(); // deliberately ignored: cleanup must not mask the cause.
    }
};

/// Wraps the configured encode backend to publish per-stage counts and keep a
/// handle the scope guard can finish. Adds no encoding behaviour of its own.
class GuardedBackend final : public media::IEncodeBackend {
public:
    GuardedBackend(std::unique_ptr<media::IEncodeBackend> inner,
                   std::shared_ptr<EncodeObservation> observation)
        : inner_(std::move(inner)), observation_(std::move(observation)) {
        std::lock_guard<std::mutex> lock(observation_->mutex);
        observation_->live = inner_.get();
    }

    ~GuardedBackend() override {
        std::lock_guard<std::mutex> lock(observation_->mutex);
        observation_->live = nullptr;
    }

    GuardedBackend(const GuardedBackend&)            = delete;
    GuardedBackend& operator=(const GuardedBackend&) = delete;

    [[nodiscard]] Result<void> encode(const media::EncoderInputFrame& frame) override {
        Result<void> r = inner_->encode(frame);
        std::lock_guard<std::mutex> lock(observation_->mutex);
        if (r.isError()) {
            observation_->failingStage = FailingStage::Video;
        } else {
            ++observation_->videoFrames;
        }
        return r;
    }

    [[nodiscard]] Result<void> encodeAudio(const media::EncoderInputAudio& audio) override {
        Result<void> r = inner_->encodeAudio(audio);
        std::lock_guard<std::mutex> lock(observation_->mutex);
        if (r.isError()) {
            // Requirement 6.10: the reported stage must identify audio encoding
            // or muxing, not "the export".
            observation_->failingStage = FailingStage::Audio;
        } else {
            ++observation_->audioBlocks;
        }
        return r;
    }

    [[nodiscard]] Result<void> finish() override {
        {
            std::lock_guard<std::mutex> lock(observation_->mutex);
            if (observation_->finished) {
                // The scope guard already finished this backend best-effort.
                return ok();
            }
            observation_->finished = true;
        }
        Result<void> r = inner_->finish();
        if (r.isError()) {
            std::lock_guard<std::mutex> lock(observation_->mutex);
            observation_->failingStage = FailingStage::Finish;
        }
        return r;
    }

private:
    std::unique_ptr<media::IEncodeBackend> inner_;
    std::shared_ptr<EncodeObservation>     observation_;
};

/// The scope guard of design.md "Cleanup on failed export": it owns the output
/// path for the duration of the worker and, unless the export committed, calls
/// MediaEncoder::finish() best-effort and then removes the path — so no file
/// remains where the export was requested, on a failure, a cancellation or a
/// mid-export hardware failure (Requirements 6.10, 7.5, 7.7, 8.11).
class OutputGuard {
public:
    OutputGuard(std::filesystem::path path, std::shared_ptr<EncodeObservation> observation)
        : path_(std::move(path)), observation_(std::move(observation)) {}

    ~OutputGuard() {
        if (committed_) return;
        if (observation_) observation_->finishBestEffort();
        if (path_.empty()) return;
        std::error_code ec;
        std::filesystem::remove(path_, ec); // best-effort; must not mask the cause.
    }

    OutputGuard(const OutputGuard&)            = delete;
    OutputGuard& operator=(const OutputGuard&) = delete;

    /// The export succeeded: the output file is the deliverable and must survive.
    void commit() noexcept { committed_ = true; }

private:
    std::filesystem::path              path_;
    std::shared_ptr<EncodeObservation> observation_;
    bool                               committed_{false};
};

/// Prefix `message` with the stage that failed, so every failure error names the
/// failing stage (Requirements 6.10, 7.5, 8.11).
[[nodiscard]] Error stageError(const Error& cause, FailingStage stage, std::size_t videoFrames,
                               bool hardware) {
    std::string prefix;
    switch (stage) {
        case FailingStage::Audio:
            prefix = "audio encoding or muxing failed: ";
            break;
        case FailingStage::Finish:
            prefix = "finalizing the output file failed: ";
            break;
        case FailingStage::Video:
            prefix = hardware && videoFrames >= 1
                         ? "mid-export hardware encode failure: "
                         : "video encoding failed: ";
            break;
        case FailingStage::None:
            prefix = "export failed: ";
            break;
    }
    return makeError(cause.code(), prefix + cause.message());
}

} // namespace

// ---------------------------------------------------------------------------
// Supported containers and codecs
// ---------------------------------------------------------------------------

bool isSupportedExportContainer(const std::string& container) {
    const std::string c = toLowerAscii(container);
    return c == "mp4" || c == "mov" || c == "mkv" || c == "webm";
}

bool isSupportedExportCodec(gpu::CodecId codec) noexcept {
    // The three codecs the Encoder_Selector supports (Requirement 8.2).
    return codec == gpu::CodecId::H264 || codec == gpu::CodecId::HEVC ||
           codec == gpu::CodecId::VP9;
}

// ---------------------------------------------------------------------------
// Admission (Requirements 7.6, 7.9, 7.11)
// ---------------------------------------------------------------------------

Result<void> ExportCoordinator::validate(const ExportRequest2& request) {
    // --- Output path -------------------------------------------------------
    if (request.outputPath.empty()) {
        return err(invalidArgument("export requires an output path"));
    }
    const std::string pathText = request.outputPath.string();
    if (pathText.size() > kMaxExportPathLength) {
        return err(outOfRange("export output path is " + std::to_string(pathText.size()) +
                              " characters; the accepted range is 1 to " +
                              std::to_string(kMaxExportPathLength)));
    }

    std::filesystem::path parent = request.outputPath.parent_path();
    if (parent.empty()) parent = std::filesystem::path{"."};

    std::error_code ec;
    if (!std::filesystem::exists(parent, ec) || ec) {
        return err(notFound("the export output directory does not exist: " + parent.string()));
    }
    if (!std::filesystem::is_directory(parent, ec) || ec) {
        return err(invalidArgument("the export output path's parent is not a directory: " +
                                   parent.string()));
    }
    // Writability is asked of the operating system rather than inferred from the
    // permission bits, so the answer accounts for ownership, ACLs and read-only
    // mounts. It creates nothing.
    if (::access(parent.c_str(), W_OK) != 0) {
        return err(makeError(ErrorCode::PermissionDenied,
                             "the export output directory is not writable: " + parent.string()));
    }

    // --- Format ------------------------------------------------------------
    if (!isSupportedExportContainer(request.container)) {
        return err(unsupported("export container \"" + request.container +
                               "\" is not supported; the supported containers are "
                               "mp4, mov, mkv and webm"));
    }
    if (!isSupportedExportCodec(request.codec)) {
        return err(unsupported("export video codec " + codecLabel(request.codec) +
                               " is not supported; the supported codecs are H.264, HEVC and VP9"));
    }

    // --- Geometry, cadence, bit rate ---------------------------------------
    if (!request.resolution.isValid() || request.resolution.width < kMinExportWidth ||
        request.resolution.width > kMaxExportWidth || request.resolution.height < kMinExportHeight ||
        request.resolution.height > kMaxExportHeight) {
        return err(outOfRange(
            "export resolution " + std::to_string(request.resolution.width) + "x" +
            std::to_string(request.resolution.height) + " is out of range; the accepted range is " +
            std::to_string(kMinExportWidth) + "x" + std::to_string(kMinExportHeight) + " to " +
            std::to_string(kMaxExportWidth) + "x" + std::to_string(kMaxExportHeight)));
    }
    if (!request.frameRate.isValid()) {
        return err(invalidArgument("export frame rate must be a positive rational; the accepted "
                                   "range is 1 to 120 frames per second"));
    }
    const double fps = request.frameRate.toDouble();
    if (fps < kMinExportFps || fps > kMaxExportFps) {
        return err(outOfRange("export frame rate " + std::to_string(fps) +
                              " frames per second is out of range; the accepted range is 1 to 120"));
    }
    if (request.bitrateKbps < kMinExportBitrateKbps ||
        request.bitrateKbps > kMaxExportBitrateKbps) {
        return err(outOfRange("export video bit rate " + std::to_string(request.bitrateKbps) +
                              " kbps is out of range; the accepted range is " +
                              std::to_string(kMinExportBitrateKbps) + " to " +
                              std::to_string(kMaxExportBitrateKbps)));
    }

    // --- Overwrite protection (Requirement 7.11) ----------------------------
    // Checked LAST and, like every check above, without touching the file: an
    // existing destination is left byte-for-byte intact by a rejection.
    if (std::filesystem::exists(request.outputPath, ec) && !ec && !request.overwrite) {
        return err(makeError(ErrorCode::AlreadyExists,
                             "the export destination already exists: " + pathText +
                                 "; re-request with an explicit overwrite acknowledgement to "
                                 "replace it"));
    }

    return ok();
}

Result<void> ExportCoordinator::validateTimeline(const Project& project) {
    std::size_t segments = 0;
    for (const Track& track : project.tracks) {
        segments += track.clips.size();
    }
    if (segments == 0) {
        return err(failedPrecondition(
            "the timeline is empty: an export requires at least one media segment"));
    }
    return ok();
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

ExportCoordinator::ExportCoordinator(ProjectSession& session, gpu::GpuContext& liveContext,
                                     media::DecoderTeardownQueue& teardown, Options options)
    : session_(session),
      liveContext_(liveContext),
      teardown_(teardown),
      options_(std::move(options)) {
    if (!options_.clock) {
        options_.clock = media::systemSteadyClock();
    }
    // Requirement 7.3 fixes the ceiling at one second: a longer configured
    // interval is clamped down rather than honoured, so the guarantee cannot be
    // configured away. A shorter interval (including zero) is allowed — reporting
    // more often than required is still compliant.
    const auto ceiling = std::chrono::milliseconds{std::chrono::seconds{1}};
    if (options_.progressInterval > ceiling) {
        options_.progressInterval = ceiling;
    }
    if (options_.progressInterval < std::chrono::milliseconds::zero()) {
        options_.progressInterval = std::chrono::milliseconds::zero();
    }
}

ExportCoordinator::~ExportCoordinator() {
    cancel();
    joinWorker();
}

void ExportCoordinator::joinWorker() {
    if (worker_.joinable()) {
        worker_.join();
    }
}

void ExportCoordinator::setNotifier(Notifier notifier) {
    std::lock_guard<std::mutex> lock(mutex_);
    notifier_ = std::move(notifier);
}

// ---------------------------------------------------------------------------
// begin (Requirements 7.1, 7.2, 7.6, 7.9, 7.10, 7.11)
// ---------------------------------------------------------------------------

Result<void> ExportCoordinator::begin(const ExportRequest2& request, ProgressSink progress,
                                      CompletionSink completion) {
    // Requirement 7.10: a second request is refused WITHOUT touching the running
    // export — no flag is cleared, no queue is drained, no sink is replaced. The
    // check comes first for exactly that reason.
    if (running_.load()) {
        return err(failedPrecondition(
            "an export is already in progress; wait for it to finish or cancel it"));
    }

    // Requirements 7.9 / 7.11: reject before anything is created.
    if (Result<void> valid = validate(request); valid.isError()) {
        return valid;
    }

    // Requirement 7.6: the snapshot is taken here, on the calling thread, and the
    // empty-timeline check is made against that same value — so what is validated
    // and what is exported cannot differ.
    WorkerInput input;
    input.snapshot = session_.engine().snapshot(); // VALUE COPY (Requirement 7.2).
    input.request = request;
    input.revisionAtStart = session_.revision();

    if (Result<void> valid = validateTimeline(input.snapshot); valid.isError()) {
        return valid;
    }

    // A previous worker's thread object may still be joinable even though that
    // export finished; reap it before starting the next one.
    joinWorker();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        progressQueue_.clear();
        outcomeQueue_.reset();
        progressSink_ = std::move(progress);
        completionSink_ = std::move(completion);
    }
    deliveredProgress_.clear();
    lastOutcome_.reset();
    lastError_.reset();
    lastSelection_.reset();
    lastPercent_ = -1;
    lastEmitAt_.reset();
    cancelRequested_.store(false);
    running_.store(true);

    worker_ = std::thread([this, input = std::move(input)]() mutable {
        runExport(std::move(input));
    });
    return ok();
}

void ExportCoordinator::cancel() { cancelRequested_.store(true); }

// ---------------------------------------------------------------------------
// The worker (Requirements 7.2, 7.4, 7.5, 7.7, 6.5, 6.10, 6.11, 8.11)
// ---------------------------------------------------------------------------

void ExportCoordinator::runExport(WorkerInput input) {
    const std::filesystem::path outputPath = input.request.outputPath;
    auto observation = std::make_shared<EncodeObservation>();

    // The guard is installed BEFORE the encoder can create the file and is the
    // only thing that decides whether the output survives.
    OutputGuard guard(outputPath, observation);

    ExportOutcome outcome;
    outcome.outputPath = outputPath;
    outcome.projectRevisionAtStart = input.revisionAtStart;
    outcome.projectModified = false; // structural: the worker cannot reach the session.

    auto fail = [&](Error error) {
        QueuedOutcome queued;
        queued.succeeded = false;
        queued.error = std::move(error);
        queued.outcome = outcome;
        finishExport(std::move(queued));
    };

    // --- Encoder choice: media::EncoderSelector, not a second policy --------
    media::EncodeParameters parameters;
    parameters.resolution = input.request.resolution;
    parameters.frameRate = input.request.frameRate;
    parameters.bitrateBitsPerSecond = input.request.bitrateKbps * 1000;

    media::EncoderSelectionRequest selectionRequest;
    selectionRequest.codec = input.request.codec;
    selectionRequest.preferHardware = input.request.preferHardware;
    selectionRequest.parameters = parameters;
    // The live context's capabilities are read as a VALUE on this thread; the
    // context object itself is not used by the export.
    selectionRequest.caps = options_.caps.value_or(liveContext_.capabilities());
    selectionRequest.availability =
        options_.availability.value_or(gpu::BridgeAvailability::fromBuildConfig());

    media::EncoderSelector selector(options_.selector);
    Result<media::SelectionOutcome> selected = selector.select(selectionRequest);
    if (selected.isError()) {
        fail(std::move(selected).error());
        return;
    }
    const media::SelectionOutcome selection = std::move(selected).value();

    // --- Export-local GPU context (Requirement 7.2) ------------------------
    // A fresh context per export: the live one belongs to the interactive
    // preview, and an export must not be able to disturb it.
    Result<gpu::GpuContext> contextResult =
        options_.gpuContextFactory
            ? options_.gpuContextFactory()
            : (liveContext_.isSoftwareFallback()
                   ? Result<gpu::GpuContext>(gpu::GpuContext::softwareFallback())
                   : gpu::GpuContext::create(options_.policy));
    if (contextResult.isError()) {
        fail(makeError(contextResult.error().code(),
                       "preparing the export GPU context failed: " +
                           contextResult.error().message()));
        return;
    }
    gpu::GpuContext exportContext = std::move(contextResult).value();

    // --- Export-local compositor and decoders (Requirement 7.2) ------------
    gpu::Compositor compositor(exportContext);

    // Requirements 9.5, 10.3: install the SAME text/caption rasterizer the
    // live preview compositor uses, so a text clip or caption cue burns in
    // identically rather than merely being reachable through a second,
    // separately-behaving renderer. Absent one (no text/caption track exists
    // in this build's composition, or the caller genuinely configured none),
    // an export with a visible text clip or caption cue fails with
    // FailedPrecondition exactly like a visible video clip with no frame
    // provider would — the same "reachable when configured" contract every
    // other injectable seam in this options struct already follows.
    if (options_.textRasterizer) {
        compositor.setTextRasterizer(options_.textRasterizer);
    }

    // The export-local decoder set: its own DecoderClipFrameProvider, its own
    // decoder cache, retiring through the shared teardown queue (whose whole
    // purpose is to absorb slow decoder destruction off the calling thread).
    std::unique_ptr<media::DecoderClipFrameProvider> decoders;
    if (options_.frameProvider) {
        compositor.setFrameProvider(options_.frameProvider);
    } else {
        media::ClipFrameProviderOptions decoderOptions;
        decoderOptions.decoderCacheCapacity = options_.decoderCacheCapacity;
        decoders = std::make_unique<media::DecoderClipFrameProvider>(
            teardown_,
            options_.decodeFactory ? options_.decodeFactory : media::ffmpegDecodeBackendFactory(),
            decoderOptions);
        compositor.setFrameProvider(decoders->asProvider());
    }

    // --- The engine-level request ------------------------------------------
    media::ExportEngine engine(compositor, options_.audioRenderer);

    media::ExportRequest engineRequest;
    engineRequest.codec = input.request.codec;
    engineRequest.resolution = input.request.resolution;
    engineRequest.frameRate = input.request.frameRate;
    engineRequest.bitrateBitsPerSecond = parameters.bitrateBitsPerSecond;
    // The selector has already decided; the encoder is asked for hardware only
    // when the selection says hardware, so the two never disagree.
    engineRequest.preferHardware = selection.selection.isHardware();
    engineRequest.caps = selectionRequest.caps;
    engineRequest.availability = selectionRequest.availability;
    engineRequest.outputPath = outputPath;
    engineRequest.containerFormat = containerFormatFor(input.request.container);
    engineRequest.includeAudio = input.request.includeAudio;
    engineRequest.audio = options_.audio;
    // Every frame is reported to the coordinator; the coordinator, not the engine,
    // owns the ≤1 s cadence, because only the coordinator has the injected clock.
    engineRequest.progressInterval = std::chrono::milliseconds::zero();
    engineRequest.cancelled = [this]() { return cancelRequested_.load(); };

    const std::size_t plannedFrames =
        media::ExportEngine::plannedFrameCount(input.snapshot, input.request.frameRate);
    outcome.plannedFrames = plannedFrames;
    outcome.duration =
        input.request.frameRate.durationForFrames(static_cast<std::int64_t>(plannedFrames));

    // --- Wrap the encode backend so the stages are observable ---------------
    media::EncodeBackendFactory configured =
        options_.encodeFactory ? options_.encodeFactory : media::ffmpegEncodeBackendFactory();
    media::EncodeBackendFactory guarded =
        [configured, observation](const media::EncodeSpec& spec, const gpu::CodecRoute& route)
        -> Result<std::unique_ptr<media::IEncodeBackend>> {
        Result<std::unique_ptr<media::IEncodeBackend>> made = configured(spec, route);
        if (made.isError()) return made;
        std::unique_ptr<media::IEncodeBackend> inner = std::move(made).value();
        if (!inner) {
            return err<std::unique_ptr<media::IEncodeBackend>>(
                makeError(ErrorCode::Internal, "the encode backend factory returned null"));
        }
        return std::unique_ptr<media::IEncodeBackend>(
            std::make_unique<GuardedBackend>(std::move(inner), observation));
    };

    // --- Run ----------------------------------------------------------------
    offerProgress(0, 0, plannedFrames, Duration::zero(), /*force=*/true);

    Result<media::ExportResult> ran =
        engine.run(input.snapshot, engineRequest, guarded,
                   [this, plannedFrames](const media::ExportProgress& p) {
                       offerProgress(p.percent, p.framesRendered,
                                     p.totalFrames != 0 ? p.totalFrames : plannedFrames, p.position,
                                     /*force=*/false);
                   });

    std::size_t videoFrames = 0;
    FailingStage stage = FailingStage::None;
    {
        std::lock_guard<std::mutex> lock(observation->mutex);
        videoFrames = observation->videoFrames;
        stage = observation->failingStage;
    }

    if (ran.isError()) {
        Error cause = std::move(ran).error();
        outcome.framesEncoded = videoFrames;
        outcome.encoderName = selection.selection.encoderName();
        outcome.usedHardwareEncode = selection.selection.isHardware();
        outcome.usedSoftwareFallback = selection.selection.isSoftwareFallback();
        outcome.fallbackReason = selection.selection.fallbackReason();

        if (cause.code() == ErrorCode::Cancelled) {
            // Requirement 7.7: cancellation is an OUTCOME, not an error. The guard
            // has already removed the partial file by the time this is delivered.
            outcome.cancelled = true;
            QueuedOutcome queued;
            queued.succeeded = true;
            queued.outcome = outcome;
            queued.selection = selection;
            finishExport(std::move(queued));
            return;
        }

        // Requirements 6.10 / 7.5 / 8.11: name the failing stage. A hardware
        // failure AFTER at least one frame is a mid-export hardware encode
        // failure, which is a different fact from a hardware init failure (that
        // one has already become a software fallback inside the selector).
        QueuedOutcome queued;
        queued.succeeded = false;
        queued.error = stageError(cause, stage, videoFrames, selection.selection.isHardware());
        queued.outcome = outcome;
        queued.selection = selection;
        finishExport(std::move(queued));
        return;
    }

    const media::ExportResult result = std::move(ran).value();

    // Reconcile the selection with what the encoder actually bound to. The encoder
    // performs its own hardware-init retry; if it ended up on software after the
    // selector chose hardware, the outcome must report the software encoder and
    // the fallback, never the hardware name (Requirements 8.2, 8.3, 8.8).
    if (selection.selection.isHardware() && !result.usedHardwareEncode) {
        const media::EncoderSelection fallback = media::EncoderSelection::software(
            input.request.codec,
            "hardware encoder initialization failed; the software encoder was used",
            parameters);
        outcome.encoderName = fallback.encoderName();
        outcome.usedHardwareEncode = false;
        outcome.usedSoftwareFallback = true;
        outcome.fallbackReason = fallback.fallbackReason();
    } else {
        outcome.encoderName = selection.selection.encoderName();
        outcome.usedHardwareEncode = result.usedHardwareEncode;
        outcome.usedSoftwareFallback =
            selection.selection.isSoftwareFallback() || result.usedSoftwareFallback;
        outcome.fallbackReason = selection.selection.fallbackReason();
    }

    outcome.framesEncoded = result.framesRendered;
    outcome.plannedFrames = result.totalFrames;
    outcome.containsAudio = result.containsAudio;
    outcome.audioFrames = result.audioFrames;
    outcome.duration =
        input.request.frameRate.durationForFrames(static_cast<std::int64_t>(result.totalFrames));
    outcome.cancelled = false;

    // Requirement 7.4: the final report is 100%, and it is queued before the
    // completion so the owner sees the two in that order.
    offerProgress(100, result.framesRendered, result.totalFrames,
                  input.request.frameRate.durationForFrames(
                      static_cast<std::int64_t>(result.framesRendered > 0 ? result.framesRendered - 1
                                                                          : 0)),
                  /*force=*/true);

    guard.commit(); // the output file is the deliverable.

    // Requirement 10.3's sidecar export mode: unconditional whenever the
    // exported snapshot carries at least one non-muted caption cue, run AFTER
    // guard.commit() so a sidecar-write failure below can never cause the
    // (already-succeeded) video output to be removed — the two are reported
    // together but the video's own success is not retroactively undone by a
    // problem writing a second, optional file. Same base name as the video
    // output, ".srt" extension, sitting next to it — the burned-in layer
    // (Compositor::gatherVisibleCaptionCues) and this text both derive their
    // timing from the identical timelineStart/timelineEnd() fields, so the two
    // outputs cannot disagree about when a cue is on screen.
    if (projectHasCaptions(input.snapshot)) {
        std::filesystem::path sidecarPath = outputPath;
        sidecarPath.replace_extension(".srt");
        std::ofstream sidecar(sidecarPath, std::ios::binary | std::ios::trunc);
        if (sidecar) {
            const std::string srt = renderSrt(input.snapshot);
            sidecar.write(srt.data(), static_cast<std::streamsize>(srt.size()));
            sidecar.close();
            if (sidecar.good()) {
                outcome.captionsSidecarPath = sidecarPath;
            }
        }
        // A sidecar write failure is deliberately non-fatal to the export as a
        // whole (the video output already exists and is correct): outcome
        // .captionsSidecarPath simply stays empty, which a caller can observe
        // and treat as "no sidecar was written" without the export itself
        // having failed.
    }

    QueuedOutcome queued;
    queued.succeeded = true;
    queued.outcome = outcome;
    queued.selection = selection;
    finishExport(std::move(queued));
}

// ---------------------------------------------------------------------------
// Marshalling to the owning thread (Requirement 7.3)
// ---------------------------------------------------------------------------

void ExportCoordinator::offerProgress(int percent, std::size_t framesEncoded,
                                      std::size_t totalFrames, Duration position, bool force) {
    // Monotonic and bounded: a percentage never regresses and never leaves
    // [0, 100], whatever the engine reported.
    int clamped = std::clamp(percent, 0, 100);
    if (clamped < lastPercent_) clamped = lastPercent_;

    const auto now = options_.clock();
    // The interval is a CEILING on the gap between reports: reporting less often
    // than it is what Requirement 7.3 forbids, so a report that is not yet due is
    // simply dropped. Because the engine reports every frame, the next due frame
    // carries the newest percentage — nothing is lost by dropping this one.
    if (!force && lastEmitAt_.has_value() && (now - *lastEmitAt_) < options_.progressInterval) {
        return;
    }

    lastPercent_ = clamped;
    lastEmitAt_ = now;

    Notifier notifier;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        progressQueue_.push_back(
            ExportProgressReport{clamped, framesEncoded, totalFrames, position});
        notifier = notifier_;
    }
    cv_.notify_all();
    if (notifier) notifier();
}

void ExportCoordinator::finishExport(QueuedOutcome outcome) {
    Notifier notifier;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        outcomeQueue_ = std::move(outcome);
        notifier = notifier_;
    }
    // Cleared only after the outcome is queued, so an owner that observes
    // running() == false always finds the outcome available.
    running_.store(false);
    cv_.notify_all();
    if (notifier) notifier();
}

std::size_t ExportCoordinator::pump() {
    std::deque<ExportProgressReport>  progress;
    std::optional<QueuedOutcome>      outcome;
    ProgressSink                      progressSink;
    CompletionSink                    completionSink;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        progress.swap(progressQueue_);
        outcome = std::move(outcomeQueue_);
        outcomeQueue_.reset();
        progressSink = progressSink_;
        completionSink = completionSink_;
    }

    std::size_t delivered = 0;
    for (const ExportProgressReport& report : progress) {
        deliveredProgress_.push_back(report);
        if (progressSink) progressSink(report);
        ++delivered;
    }

    if (outcome.has_value()) {
        lastSelection_ = outcome->selection;
        if (outcome->succeeded) {
            lastOutcome_ = outcome->outcome;
            lastError_.reset();
            if (completionSink) {
                completionSink(Result<ExportOutcome>(outcome->outcome));
            }
        } else {
            lastOutcome_ = outcome->outcome;
            lastError_ = outcome->error;
            if (completionSink) {
                completionSink(err<ExportOutcome>(outcome->error));
            }
        }
        ++delivered;
    }
    return delivered;
}

bool ExportCoordinator::waitForCompletion(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    // Bounded by construction: a coordinator that never finishes returns false
    // rather than blocking a caller (or a test) forever.
    return cv_.wait_for(lock, timeout, [this]() {
        return outcomeQueue_.has_value() || !running_.load();
    });
}

std::size_t ExportCoordinator::awaitCompletion(std::chrono::milliseconds timeout) {
    if (!waitForCompletion(timeout)) {
        return 0;
    }
    joinWorker();
    return pump();
}

// ---------------------------------------------------------------------------
// The `timeline.export` tool-surface adapter (task 9.7; Requirements 3.1, 7.2)
// ---------------------------------------------------------------------------

namespace {

/// The tool argument names, spelled once.
constexpr const char* kArgOutputPath     = "outputPath";
constexpr const char* kArgFormat         = "format";
constexpr const char* kArgWidth          = "width";
constexpr const char* kArgHeight         = "height";
constexpr const char* kArgCodec          = "codec";
constexpr const char* kArgFps            = "fps";
constexpr const char* kArgBitrateKbps    = "bitrateKbps";
constexpr const char* kArgIncludeAudio   = "includeAudio";
constexpr const char* kArgPreferHardware = "preferHardware";
constexpr const char* kArgOverwrite      = "overwrite";

/// The `codec` values the tool accepts, which are exactly the three codecs
/// `isSupportedExportCodec` admits (Requirement 8.2). Kept next to the parser so
/// the accepted spellings and the parse cannot drift; the published enum is
/// declared from the same three strings in `ToolRegistry.cpp` and a unit test
/// asserts the two agree.
[[nodiscard]] std::optional<gpu::CodecId> parseExportCodec(const std::string& text) {
    const std::string codec = toLowerAscii(text);
    if (codec == "h264") return gpu::CodecId::H264;
    if (codec == "hevc") return gpu::CodecId::HEVC;
    if (codec == "vp9")  return gpu::CodecId::VP9;
    return std::nullopt;
}

/// A frame rate for a frames-per-second number, mirroring the conversion
/// `project.create` applies to its own `fps` argument (exact integers stay exact,
/// the three broadcast pull-downs are recognised, anything else becomes a
/// thousandths rational) so the same number means the same cadence on both tools.
[[nodiscard]] FrameRate frameRateFromFps(double fps) {
    const double rounded = std::round(fps);
    if (std::abs(fps - rounded) < 1e-9 && rounded >= 1.0) {
        return FrameRate{static_cast<std::int64_t>(rounded), 1};
    }
    struct PullDown {
        double    value;
        FrameRate rate;
    };
    const PullDown pullDowns[] = {{24000.0 / 1001.0, FrameRate::fps23_976()},
                                  {30000.0 / 1001.0, FrameRate::fps29_97()},
                                  {60000.0 / 1001.0, FrameRate::fps59_94()}};
    for (const PullDown& candidate : pullDowns) {
        if (std::abs(fps - candidate.value) < 0.0005) return candidate.rate;
    }
    return FrameRate{static_cast<std::int64_t>(std::llround(fps * 1000.0)), 1000};
}

/// Read an optional integer-valued argument, refusing a present-but-not-numeric
/// one rather than silently substituting a default.
[[nodiscard]] Result<std::int64_t> optionalInt(const Json& input, const char* name,
                                               std::int64_t fallback) {
    const Json* value = input.find(name);
    if (value == nullptr || value->isNull()) return Result<std::int64_t>(fallback);
    if (!value->isNumber()) {
        return err<std::int64_t>(invalidArgument(std::string("timeline.export: '") + name +
                                                "' must be a number"));
    }
    return Result<std::int64_t>(value->isInt() ? value->asInt()
                                               : static_cast<std::int64_t>(value->asDouble()));
}

/// As above for a boolean argument.
[[nodiscard]] Result<bool> optionalBool(const Json& input, const char* name, bool fallback) {
    const Json* value = input.find(name);
    if (value == nullptr || value->isNull()) return Result<bool>(fallback);
    if (!value->isBool()) {
        return err<bool>(invalidArgument(std::string("timeline.export: '") + name +
                                         "' must be a boolean"));
    }
    return Result<bool>(value->asBool());
}

} // namespace

Result<ExportRequest2> exportRequestFromToolArguments(const Json& input, const Project& project) {
    const Json* outputPath = input.find(kArgOutputPath);
    if (outputPath == nullptr || !outputPath->isString() || outputPath->asString().empty()) {
        return err<ExportRequest2>(
            invalidArgument("timeline.export: 'outputPath' is required and must be a non-empty "
                            "string"));
    }
    const Json* format = input.find(kArgFormat);
    if (format == nullptr || !format->isString() || format->asString().empty()) {
        return err<ExportRequest2>(
            invalidArgument("timeline.export: 'format' is required and must be a non-empty "
                            "string naming the output container (mp4, mov, mkv or webm)"));
    }

    ExportRequest2 request;
    request.outputPath = std::filesystem::path(outputPath->asString());
    request.container = toLowerAscii(format->asString());

    // The codec: the requested one, or the container's natural pairing. A webm
    // carries VP9; everything else defaults to H.264. An unsupported container is
    // NOT diagnosed here — validate() owns that message and names the whole
    // supported set (Requirement 7.9).
    if (const Json* codec = input.find(kArgCodec); codec != nullptr && !codec->isNull()) {
        if (!codec->isString()) {
            return err<ExportRequest2>(
                invalidArgument("timeline.export: 'codec' must be a string"));
        }
        const std::optional<gpu::CodecId> parsed = parseExportCodec(codec->asString());
        if (!parsed.has_value()) {
            return err<ExportRequest2>(unsupported(
                "timeline.export: video codec \"" + codec->asString() +
                "\" is not supported; the supported codecs are h264, hevc and vp9"));
        }
        request.codec = *parsed;
    } else {
        request.codec = request.container == "webm" ? gpu::CodecId::VP9 : gpu::CodecId::H264;
    }

    // Geometry and cadence default to the PROJECT's own canvas and timeline frame
    // rate, so an agent that names only a path and a container exports the project
    // as authored rather than at an invented size.
    Result<std::int64_t> width =
        optionalInt(input, kArgWidth, static_cast<std::int64_t>(project.canvas.width));
    if (width.isError()) return err<ExportRequest2>(std::move(width).error());
    Result<std::int64_t> height =
        optionalInt(input, kArgHeight, static_cast<std::int64_t>(project.canvas.height));
    if (height.isError()) return err<ExportRequest2>(std::move(height).error());

    // Clamp only the CAST, never the value: a negative or absurd number must reach
    // validate() so it is rejected by name with its accepted range, not folded into
    // a plausible-looking resolution by an unsigned conversion.
    const std::int64_t clampedWidth = std::clamp<std::int64_t>(width.value(), 0, 1'000'000);
    const std::int64_t clampedHeight = std::clamp<std::int64_t>(height.value(), 0, 1'000'000);
    request.resolution = Resolution{static_cast<std::uint32_t>(clampedWidth),
                                    static_cast<std::uint32_t>(clampedHeight)};

    if (const Json* fps = input.find(kArgFps); fps != nullptr && !fps->isNull()) {
        if (!fps->isNumber()) {
            return err<ExportRequest2>(
                invalidArgument("timeline.export: 'fps' must be a number"));
        }
        const double requested = fps->asDouble();
        // Out-of-range values are validate()'s to reject; only a rate the rational
        // conversion cannot express at all is refused here.
        if (!(requested > 0.0) || requested > 1'000'000.0) {
            return err<ExportRequest2>(outOfRange(
                "timeline.export: 'fps' must be a positive frame rate; the accepted range is " +
                std::to_string(static_cast<int>(kMinExportFps)) + " to " +
                std::to_string(static_cast<int>(kMaxExportFps)) + " frames per second"));
        }
        request.frameRate = frameRateFromFps(requested);
    } else {
        request.frameRate = project.timelineFps;
    }

    Result<std::int64_t> bitrate = optionalInt(input, kArgBitrateKbps, request.bitrateKbps);
    if (bitrate.isError()) return err<ExportRequest2>(std::move(bitrate).error());
    request.bitrateKbps = bitrate.value();

    Result<bool> includeAudio = optionalBool(input, kArgIncludeAudio, true);
    if (includeAudio.isError()) return err<ExportRequest2>(std::move(includeAudio).error());
    request.includeAudio = includeAudio.value();

    Result<bool> preferHardware = optionalBool(input, kArgPreferHardware, true);
    if (preferHardware.isError()) return err<ExportRequest2>(std::move(preferHardware).error());
    request.preferHardware = preferHardware.value();

    // Requirement 7.11: the acknowledgement is never implied. Absent the argument
    // an existing destination is preserved and the request rejected.
    Result<bool> overwrite = optionalBool(input, kArgOverwrite, false);
    if (overwrite.isError()) return err<ExportRequest2>(std::move(overwrite).error());
    request.overwrite = overwrite.value();

    return Result<ExportRequest2>(std::move(request));
}

Json exportOutcomeToJson(const ExportOutcome& outcome) {
    Json out = Json::object();
    out.set("outputPath", outcome.outputPath.string());
    out.set("framesEncoded", static_cast<std::int64_t>(outcome.framesEncoded));
    out.set("plannedFrames", static_cast<std::int64_t>(outcome.plannedFrames));
    out.set("encoderName", outcome.encoderName);
    // Requirement 7.2's boolean, plus the fallback flag and its reason: the two
    // cannot both be true (Requirement 8.8 makes that unconstructible), so a caller
    // reading both learns whether hardware ran, and if not, why.
    out.set("usedHardwareEncode", outcome.usedHardwareEncode);
    out.set("usedSoftwareFallback", outcome.usedSoftwareFallback);
    if (!outcome.fallbackReason.empty()) out.set("fallbackReason", outcome.fallbackReason);
    out.set("containsAudio", outcome.containsAudio);
    if (outcome.containsAudio) {
        out.set("audioFrames", static_cast<std::int64_t>(outcome.audioFrames));
    }
    out.set("durationNs", static_cast<std::int64_t>(outcome.duration.nanoseconds()));
    // Requirements 7.1/7.2: reported so a caller can ASSERT that the export left
    // the project alone rather than assume it. Always false — the worker runs on a
    // value-copy snapshot and holds no session reference.
    out.set("projectModified", outcome.projectModified);
    // Requirement 10.3: present iff a sidecar subtitle file was written
    // alongside the video output (i.e. the exported project had at least one
    // non-muted caption cue and the write succeeded); absent otherwise.
    if (!outcome.captionsSidecarPath.empty()) {
        out.set("captionsSidecarPath", outcome.captionsSidecarPath.string());
    }
    return out;
}

Tool::Handler makeExportToolHandler(ExportCoordinator& coordinator, ProjectSession& session,
                                    ExportToolOptions options) {
    return [&coordinator, &session, options](const Json& input) -> Result<Json> {
        Result<ExportRequest2> request =
            exportRequestFromToolArguments(input, session.engine().snapshot());
        if (request.isError()) return err<Json>(std::move(request).error());

        // The SAME entry point the export dialog uses (Requirement 7.2): admission,
        // the value-copy snapshot, the worker and the cleanup guard all belong to the
        // coordinator. A rejection here — an invalid parameter, an empty timeline, an
        // unacknowledged existing destination, an export already running — is returned
        // verbatim, having created no file.
        const ExportRequest2 parsed = std::move(request).value();
        if (Result<void> started = coordinator.begin(parsed); started.isError()) {
            return err<Json>(std::move(started).error());
        }

        // Bounded wait, pumped on this thread: the progress reports and the outcome
        // are delivered here, never from the worker.
        if (coordinator.awaitCompletion(options.budget) == 0) {
            // The export did not finish inside the budget. Cancelling stops it at its
            // next frame boundary and the coordinator's guard removes the partial
            // output, so the timeout leaves no file either.
            coordinator.cancel();
            (void)coordinator.awaitCompletion(options.budget);
            return err<Json>(makeError(
                ErrorCode::Timeout,
                "timeline.export did not complete within " +
                    std::to_string(options.budget.count()) +
                    " ms; the export was cancelled and the partial output removed"));
        }

        if (const std::optional<Error>& failure = coordinator.lastError(); failure.has_value()) {
            return err<Json>(*failure);
        }
        const std::optional<ExportOutcome>& outcome = coordinator.lastOutcome();
        if (!outcome.has_value()) {
            return err<Json>(makeError(ErrorCode::Internal,
                                       "timeline.export: the export outcome was never delivered"));
        }
        if (outcome->cancelled) {
            return err<Json>(makeError(ErrorCode::Cancelled,
                                       "timeline.export was cancelled; no file remains at " +
                                           outcome->outputPath.string()));
        }
        return Result<Json>(exportOutcomeToJson(*outcome));
    };
}

} // namespace palmier::services
