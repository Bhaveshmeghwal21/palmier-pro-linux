// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/ExportCoordinator.cpp - the export worker: construction and teardown,
// starting and cancelling an export, and runExport, which drives decode, effects,
// encode and muxing for one request behind the guards that make a failure
// recoverable and a partial output impossible to mistake for a finished one.
//
// Validation, progress reporting and the agent-facing tool surface were moved into
// sibling units when this file exceeded 1000 lines; they share
// services/ExportCoordinatorInternal.hpp.

#include "services/ExportCoordinatorInternal.hpp"

namespace palmier::services::detail {

[[nodiscard]] std::string toLowerAscii(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

[[nodiscard]] std::string codecLabel(gpu::CodecId codec) {
    switch (codec) {
        case gpu::CodecId::H264:
            return "H.264";
        case gpu::CodecId::HEVC:
            return "HEVC";
        case gpu::CodecId::VP9:
            return "VP9";
        case gpu::CodecId::AV1:
            return "AV1";
        case gpu::CodecId::MPEG2:
            return "MPEG-2";
        case gpu::CodecId::Unknown:
            return "unknown";
    }
    return "unknown";
}

}  // namespace palmier::services::detail

namespace palmier::services {

using namespace palmier::services::detail;  // NOLINT(google-build-using-namespace)

namespace {

[[nodiscard]] std::string containerFormatFor(const std::string& container) {
    return toLowerAscii(container);
}

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
            prefix = hardware && videoFrames >= 1 ? "mid-export hardware encode failure: "
                                                  : "video encoding failed: ";
            break;
        case FailingStage::None:
            prefix = "export failed: ";
            break;
    }
    return makeError(cause.code(), prefix + cause.message());
}

}  // namespace

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
    input.snapshot = session_.engine().snapshot();  // VALUE COPY (Requirement 7.2).
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

    worker_ =
        std::thread([this, input = std::move(input)]() mutable { runExport(std::move(input)); });
    return ok();
}

void ExportCoordinator::cancel() { cancelRequested_.store(true); }

void ExportCoordinator::runExport(WorkerInput input) {
    const std::filesystem::path outputPath = input.request.outputPath;
    auto observation = std::make_shared<EncodeObservation>();

    // The guard is installed BEFORE the encoder can create the file and is the
    // only thing that decides whether the output survives.
    OutputGuard guard(outputPath, observation);

    ExportOutcome outcome;
    outcome.outputPath = outputPath;
    outcome.projectRevisionAtStart = input.revisionAtStart;
    outcome.projectModified = false;  // structural: the worker cannot reach the session.

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
        fail(makeError(contextResult.error().code(), "preparing the export GPU context failed: " +
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
        [configured, observation](
            const media::EncodeSpec& spec,
            const gpu::CodecRoute& route) -> Result<std::unique_ptr<media::IEncodeBackend>> {
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
            "hardware encoder initialization failed; the software encoder was used", parameters);
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
                  input.request.frameRate.durationForFrames(static_cast<std::int64_t>(
                      result.framesRendered > 0 ? result.framesRendered - 1 : 0)),
                  /*force=*/true);

    guard.commit();  // the output file is the deliverable.

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

}  // namespace palmier::services
