// SPDX-License-Identifier: GPL-3.0-or-later
//
// media/ExportEngine.cpp — the timeline export render loop + progress reporting
// (task 10.1; Requirements 11.1, 11.2, 10.3, 10.8).
//
// runImpl() is the heart of the Export Engine: it resolves the output frame rate
// and canvas, plans the frame count, builds a MediaEncoder (HW-preferred with SW
// fallback), then renders each timeline position with the Compositor and submits
// the composited frame to the encoder in strictly increasing presentation time.
// Progress is emitted through the caller's callback: once at 0% up front, at
// least once per second while rendering, and once at 100% on success. The source
// Project is only read, so an export never mutates the timeline.
//
// Task 10.2 layers export validation and failure cleanup onto this same run()
// path (Requirements 11.3, 11.4, 11.5, 11.6):
//
//   * Pre-render validation (runs BEFORE the encoder is built or any frame is
//     rendered, so a rejected request never opens/writes the output file):
//       - Unsupported output format (an unencodable codec or an unknown
//         container short-name) is rejected with Unsupported (Requirement 11.4).
//       - An unsupported output resolution (zero, above the maximum, or with an
//         odd dimension the 4:2:0 encoders cannot represent) is rejected with
//         Unsupported (Requirement 11.4).
//       - An empty timeline (zero media segments across all tracks) is rejected
//         with FailedPrecondition (Requirement 11.5).
//   * Mid-export failure cleanup: if the encoder fails to initialize, or the
//     compositor/encoder fails part-way through the render loop, or finalizing
//     the mux fails, the incomplete/partial output file is removed and the
//     original failure reason is returned. The source Project is a const
//     reference throughout, so it is never mutated on any path (Requirement
//     11.3).
//   * Success notification: on success the ExportResult carries the output
//     location and the final progress callback fires at 100% (Requirement 11.6).
//
// Task 9.3 adds the audio stream to the same loop (Requirements 6.5, 6.11): when
// the request includes audio the encoder is built with an AudioEncodeSpec and each
// iteration renders the frame, submits it, and THEN submits that frame interval's
// audio — [frame i, frame i+1) — mixed by the export-local AudioGraph behind the
// injected AudioRangeRenderer (media::AudioEngine::renderRange in production).
// Because the per-frame intervals tile [0, plannedFrameCount * frameStep) with no
// gap and no overlap, the audio stream spans the whole timeline duration and
// exceeds it by less than one video frame interval. A timeline with no clip on an
// unmuted audio-bearing track still gets that full-length stream: the renderer
// returns silence for every interval, which is what both AudioEngine::mixWindow
// and silentAudioRangeRenderer produce from an AudioGraph with zero sources
// (Requirement 6.11).

#include "media/ExportEngine.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "core/TimelineEngine.hpp" // timelineDuration()
#include "media/AudioSink.hpp"    // durationToFrames()

namespace palmier::media {

namespace {

using Clock = std::chrono::steady_clock;

// --- Export format / resolution support policy (Requirement 11.4) ----------

/// The video codecs the Export Engine can encode to. CodecId::Unknown (no
/// encoder) and decode-only entries in the catalog (e.g. MPEG-2) are rejected as
/// unsupported output formats before any rendering begins.
[[nodiscard]] bool isSupportedExportCodec(gpu::CodecId codec) noexcept {
    switch (codec) {
        case gpu::CodecId::H264:
        case gpu::CodecId::HEVC:
        case gpu::CodecId::AV1:
        case gpu::CodecId::VP9:
            return true;
        case gpu::CodecId::MPEG2:
        case gpu::CodecId::Unknown:
            return false;
    }
    return false;
}

/// A stable, human-readable name for a codec, for error messages.
[[nodiscard]] std::string_view codecName(gpu::CodecId codec) noexcept {
    switch (codec) {
        case gpu::CodecId::H264:  return "H.264";
        case gpu::CodecId::HEVC:  return "HEVC";
        case gpu::CodecId::AV1:   return "AV1";
        case gpu::CodecId::VP9:   return "VP9";
        case gpu::CodecId::MPEG2: return "MPEG-2";
        case gpu::CodecId::Unknown: return "unknown";
    }
    return "unknown";
}

/// Lower-case an ASCII string (for case-insensitive container matching).
[[nodiscard]] std::string toLowerAscii(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

/// The container short-names the Export Engine can mux to. An empty string means
/// "let the backend pick a default from the codec/output path" and is accepted.
[[nodiscard]] bool isSupportedExportContainer(const std::string& containerFormat) {
    if (containerFormat.empty()) {
        return true; // backend default.
    }
    const std::string f = toLowerAscii(containerFormat);
    return f == "mp4" || f == "mov" || f == "m4v" || f == "mkv" || f == "webm";
}

/// The largest output dimension the Export Engine accepts on either axis.
constexpr std::uint32_t kMaxExportDimension = 8192;

/// A resolution is a *supported* export target when it is positive, within the
/// maximum on each axis, and even on each axis (the 4:2:0 chroma subsampling the
/// export codecs use requires even width/height). A zero resolution is handled
/// separately (it means "fall back to the project canvas") and is not routed
/// here.
[[nodiscard]] bool isSupportedExportResolution(Resolution resolution) noexcept {
    if (!resolution.isValid()) {
        return false;
    }
    if (resolution.width > kMaxExportDimension || resolution.height > kMaxExportDimension) {
        return false;
    }
    return (resolution.width % 2u) == 0u && (resolution.height % 2u) == 0u;
}

/// The number of media segments (clips) across every track. An export of a
/// timeline with zero segments is rejected as empty (Requirement 11.5).
[[nodiscard]] std::size_t mediaSegmentCount(const Project& project) noexcept {
    std::size_t count = 0;
    for (const Track& track : project.tracks) {
        count += track.clips.size();
    }
    return count;
}

/// Best-effort removal of a partial/incomplete output file after a mid-export
/// failure (Requirement 11.3). Never throws: a missing file or a remove error is
/// ignored, since cleanup must not mask the original export failure reason.
void removeIncompleteOutput(const std::filesystem::path& outputPath) noexcept {
    if (outputPath.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::remove(outputPath, ec); // best-effort; error deliberately ignored.
}

/// Resolve the effective output frame rate: the request's when valid, otherwise
/// the project's timeline frame rate.
[[nodiscard]] FrameRate effectiveFrameRate(const Project& project, const ExportRequest& request) {
    return request.frameRate.isValid() ? request.frameRate : project.timelineFps;
}

/// Resolve the effective output resolution: the request's when valid, otherwise
/// the project's canvas.
[[nodiscard]] Resolution effectiveResolution(const Project& project, const ExportRequest& request) {
    return request.resolution.isValid() ? request.resolution : project.canvas;
}

} // namespace

AudioRangeRenderer silentAudioRangeRenderer(AudioFormat format) {
    return [format](const Project&, Duration from, Duration to) -> Result<AudioBuffer> {
        if (to < from) {
            return err<AudioBuffer>(
                invalidArgument("the export audio range end must not precede its start"));
        }
        if (!format.isValid()) {
            return err<AudioBuffer>(
                invalidArgument("the export audio format must have a positive rate and channels"));
        }
        const std::size_t frames =
            static_cast<std::size_t>(durationToFrames(to - from, format.sampleRate));
        // An export-local graph mixing ZERO sources into `frames` frames: exactly
        // the silence AudioGraph already produces for an empty source set, and
        // exactly what AudioEngine::mixWindow returns for a range no unmuted
        // audio-bearing track covers (Requirement 6.11).
        AudioGraph graph{format};
        return graph.render({}, frames);
    };
}

ExportEngine::ExportEngine(gpu::Compositor& compositor) noexcept : compositor_(compositor) {}

ExportEngine::ExportEngine(gpu::Compositor& compositor, AudioRangeRenderer audio)
    : compositor_(compositor), audio_(std::move(audio)) {}

std::size_t ExportEngine::plannedFrameCount(const Project& project, FrameRate frameRate) noexcept {
    if (!frameRate.isValid()) {
        return 0;
    }
    const Duration total = timelineDuration(project);
    const Duration step = frameRate.frameDuration();
    if (step.isZero()) {
        return 0;
    }

    // Whole frames that fit in [0, total] (floored), promoted to a ceiling so a
    // partial trailing frame is still rendered, and at least one frame so a
    // single-position timeline still produces output.
    std::int64_t frames = frameRate.framesForDuration(total);
    if (frameRate.durationForFrames(frames) < total) {
        ++frames; // ceil: cover the partial last frame.
    }
    if (frames < 1) {
        frames = 1;
    }
    return static_cast<std::size_t>(frames);
}

Result<ExportResult> ExportEngine::run(const Project& project, const ExportRequest& request,
                                       ExportProgressCallback progress) {
    return runImpl(project, request, ffmpegEncodeBackendFactory(), progress);
}

Result<ExportResult> ExportEngine::run(const Project& project, const ExportRequest& request,
                                       const EncodeBackendFactory& encodeFactory,
                                       ExportProgressCallback progress) {
    return runImpl(project, request, encodeFactory, progress);
}

Result<ExportResult> ExportEngine::runImpl(const Project& project, const ExportRequest& request,
                                           const EncodeBackendFactory& factory,
                                           const ExportProgressCallback& progress) {
    // --- Resolve output parameters (never mutating `project`) --------------
    const FrameRate frameRate = effectiveFrameRate(project, request);
    if (!frameRate.isValid()) {
        return err<ExportResult>(invalidArgument("export requires a valid frame rate"));
    }
    const Resolution resolution = effectiveResolution(project, request);
    if (!resolution.isValid()) {
        return err<ExportResult>(invalidArgument("export requires a positive output resolution"));
    }
    if (request.outputPath.empty()) {
        return err<ExportResult>(invalidArgument("export requires an output path"));
    }

    // --- Pre-render validation (reject BEFORE building the encoder or --------
    // rendering, so a rejected request never opens or writes the output file).

    // Unsupported output format: an unencodable codec (Requirement 11.4).
    if (!isSupportedExportCodec(request.codec)) {
        return err<ExportResult>(unsupported(
            std::string("export output format is unsupported: codec ") +
            std::string(codecName(request.codec)) + " has no encoder"));
    }
    // Unsupported output format: an unknown container short-name (Requirement 11.4).
    if (!isSupportedExportContainer(request.containerFormat)) {
        return err<ExportResult>(unsupported(
            "export output format is unsupported: container \"" + request.containerFormat +
            "\" is not a supported format"));
    }
    // Unsupported output resolution (Requirement 11.4). The resolution is already
    // known to be positive here; reject sizes above the maximum or with an odd
    // dimension the 4:2:0 export codecs cannot represent.
    if (!isSupportedExportResolution(resolution)) {
        return err<ExportResult>(unsupported(
            "export output resolution is unsupported: " + std::to_string(resolution.width) + "x" +
            std::to_string(resolution.height)));
    }
    // Empty timeline: zero media segments across all tracks (Requirement 11.5).
    if (mediaSegmentCount(project) == 0) {
        return err<ExportResult>(failedPrecondition(
            "export cannot render an empty timeline containing zero media segments"));
    }

    const std::size_t totalFrames = plannedFrameCount(project, frameRate);
    if (totalFrames == 0) {
        // Should not happen once the frame rate is valid, but guard defensively.
        return err<ExportResult>(invalidArgument("export could not plan any frames to render"));
    }

    // --- Build the encoder (HW-preferred, SW-fallback-on-init) -------------
    EncodeSpec spec;
    spec.codec = request.codec;
    spec.bitrateBitsPerSecond = request.bitrateBitsPerSecond;
    spec.resolution = resolution;
    spec.frameRate = frameRate;
    spec.preferHardware = request.preferHardware; // prefer HW (Req 10.3/10.8).
    spec.caps = request.caps;
    spec.availability = request.availability;
    spec.outputPath = request.outputPath;
    spec.containerFormat = request.containerFormat;
    // One audio stream alongside the video stream when the request includes audio
    // (Requirement 6.5). The encoder validates the audio parameters before the
    // output file is opened, so a malformed audio request creates no file.
    if (request.includeAudio) {
        spec.audio = request.audio;
    }

    // The audio mix source: the injected renderer (AudioEngine::renderRange in
    // production) or, when none was bound, exact silence for the requested range
    // — which is the outcome Requirement 6.11 prescribes for a timeline with no
    // clip on an unmuted audio-bearing track.
    AudioRangeRenderer renderAudio;
    if (request.includeAudio) {
        renderAudio = audio_ ? audio_ : silentAudioRangeRenderer(request.audio.format());
    }

    Result<MediaEncoder> encoderResult = MediaEncoder::create(spec, factory);
    if (encoderResult.isError()) {
        // The encoder may have opened/created the output file during init; remove
        // any partial output and preserve the (const) timeline (Requirement 11.3).
        removeIncompleteOutput(request.outputPath);
        return err<ExportResult>(std::move(encoderResult).error());
    }
    MediaEncoder encoder = std::move(encoderResult).value();

    const gpu::RenderTarget target =
        gpu::RenderTarget{resolution.width, resolution.height, request.clearColor};

    // --- Progress plumbing (monotonic 0..100%, >= once/sec) ----------------
    int lastPercent = -1;
    auto emit = [&](std::size_t rendered, Duration position) {
        if (!progress) {
            return;
        }
        int percent = static_cast<int>((static_cast<std::uint64_t>(rendered) * 100u) / totalFrames);
        percent = std::clamp(percent, 0, 100);
        if (percent < lastPercent) {
            percent = lastPercent; // never regress (Requirement 11.2).
        }
        lastPercent = percent;
        progress(ExportProgress{percent, rendered, totalFrames, position});
    };

    // Initial 0% notification (start of the 0..100 range).
    emit(0, Duration::zero());
    Clock::time_point lastEmit = Clock::now();

    // --- Render loop -------------------------------------------------------
    std::size_t rendered = 0;
    Duration lastPosition = Duration::zero();
    for (std::size_t i = 0; i < totalFrames; ++i) {
        // Cancellation is checked at the frame boundary, before any work for this
        // frame: the export stops at a known frame rather than at whatever point a
        // timer happened to fire, and the partial output is dropped exactly as for
        // any other mid-export failure (Requirement 7.7).
        if (request.cancelled && request.cancelled()) {
            removeIncompleteOutput(request.outputPath);
            return err<ExportResult>(makeError(
                ErrorCode::Cancelled,
                "the export was cancelled after " + std::to_string(rendered) + " of " +
                    std::to_string(totalFrames) + " frames"));
        }

        // Frame i's presentation time. durationForFrames(i) is exact and, since
        // frameStep > 0, strictly increasing in i — the P6 ordering property
        // (Requirement 10.3) the encoder also enforces on submit().
        const Duration position = frameRate.durationForFrames(static_cast<std::int64_t>(i));

        Result<gpu::RenderedFrame> frame = compositor_.renderAt(project, position, target);
        if (frame.isError()) {
            // Mid-export failure: drop the partial output, leave the timeline
            // untouched, and report the reason (Requirement 11.3).
            removeIncompleteOutput(request.outputPath);
            return err<ExportResult>(std::move(frame).error());
        }

        Result<void> submitted = encoder.submit(frame.value());
        if (submitted.isError()) {
            removeIncompleteOutput(request.outputPath);
            return err<ExportResult>(std::move(submitted).error());
        }

        // Then this frame interval's audio, mixed by the export-local AudioGraph
        // behind the range renderer (Requirement 6.5). Interleaving per frame
        // rather than mixing the whole timeline up front keeps the export's peak
        // memory independent of the timeline length, and keeps the two streams'
        // presentation times marching together through the muxer.
        if (renderAudio) {
            const Duration intervalEnd =
                frameRate.durationForFrames(static_cast<std::int64_t>(i) + 1);
            Result<AudioBuffer> mixed = renderAudio(project, position, intervalEnd);
            if (mixed.isError()) {
                // A failure of the audio mix is a failure of the export: drop the
                // partial output and report the audio stage (Requirement 6.10).
                removeIncompleteOutput(request.outputPath);
                return err<ExportResult>(std::move(mixed).error());
            }
            Result<void> submittedAudio = encoder.submitAudio(mixed.value(), position);
            if (submittedAudio.isError()) {
                removeIncompleteOutput(request.outputPath);
                return err<ExportResult>(std::move(submittedAudio).error());
            }
        }

        ++rendered;
        lastPosition = position;

        // Report at least once per second (Requirement 11.2). The final 100%
        // report is emitted after finish() below, so intermediate cadence here
        // is purely time-driven.
        const Clock::time_point now = Clock::now();
        if (now - lastEmit >= request.progressInterval) {
            emit(rendered, position);
            lastEmit = now;
        }
    }

    Result<void> finished = encoder.finish();
    if (finished.isError()) {
        // Finalizing the mux failed: the output is incomplete/corrupt, so remove
        // it and report the failure reason (Requirement 11.3).
        removeIncompleteOutput(request.outputPath);
        return err<ExportResult>(std::move(finished).error());
    }

    // Final, guaranteed 100% notification on success. On success the ExportResult
    // carries the output location, notifying the caller where the file was
    // written (Requirement 11.6).
    emit(rendered, lastPosition);

    ExportResult result;
    result.framesRendered = rendered;
    result.totalFrames = totalFrames;
    result.outputPath = request.outputPath;
    result.usedHardwareEncode = encoder.isHardware();
    result.usedSoftwareFallback = encoder.usedSoftwareFallback();
    result.containsAudio = encoder.hasAudioStream();
    result.audioBlocks = encoder.submittedAudioBlockCount();
    result.audioFrames = encoder.submittedAudioFrameCount();
    return result;
}

} // namespace palmier::media
