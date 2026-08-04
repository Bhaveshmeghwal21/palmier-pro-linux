// SPDX-License-Identifier: GPL-3.0-or-later
//
// media/ExportEngine.hpp — render the timeline to an output file (task 10.1).
//
// The Export Engine is design.md "Component: Export_Engine": it renders the
// complete timeline into a single output media file at a selected format and
// resolution, driving the exact pipeline the design's "Example Usage" sketches —
// a Compositor produces one composited frame per timeline position and a
// MediaEncoder encodes/muxes them in presentation order:
//
//     for (Duration t = 0; t <= engine.duration(); t += frameStep) {
//         auto frame = comp.renderAt(project, t, target).value();
//         enc.submit(frame);
//     }
//     enc.finish();
//
// This file covers task 10.1 only — the render loop and progress reporting:
//
//   * Render the full timeline via Compositor + MediaEncoder into one output
//     file at the requested format/resolution (Requirement 11.1).
//   * Never modify the source timeline: run() takes the Project by const
//     reference and only reads it, so exporting is side-effect-free on the model
//     (Requirement 11.1 "without modifying the source timeline").
//   * Emit monotonically non-decreasing 0..100% progress, at least once per
//     second, through a caller-supplied callback (Requirement 11.2).
//   * Render frames in strictly increasing presentation time — frame i is
//     rendered at frameRate.durationForFrames(i), a strictly increasing sequence
//     — which is exactly the ordering property P6 (task 10.3) validates
//     (Requirement 10.3 references P6/frame ordering here).
//   * Prefer hardware encode when a compatible backend is active: the encoder is
//     built with preferHardware and the device caps, so the MediaEncoder's
//     HW-preferred / SW-fallback-on-init routing selects hardware where possible
//     (Requirements 10.3, 10.8).
//
// Validation of unsupported formats/resolutions and empty timelines, plus the
// mid-export failure cleanup (remove the partial output, preserve the timeline),
// are task 10.2 and layered on the same run() path; the P6 frame-ordering
// property test is task 10.3.
//
// Testability: run() composes a MediaEncoder, which is built behind the
// media::EncodeBackendFactory seam. run() therefore has an overload that accepts
// an injected factory so the whole render loop — frame stepping, progress
// cadence/monotonicity, presentation ordering, and HW-preferred routing — is
// exercisable with a mock encode backend on a machine with no FFmpeg, GPU, or
// vendor SDK (mirroring MediaEncoder's own tests). The Compositor is supplied by
// the caller (already wired to a ClipFrameProvider — the MediaDecoder in
// production, or synthetic frames in tests).

#ifndef PALMIER_MEDIA_EXPORTENGINE_HPP
#define PALMIER_MEDIA_EXPORTENGINE_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

#include <optional>

#include "core/Duration.hpp"
#include "core/FrameRate.hpp"
#include "core/Project.hpp"
#include "core/Resolution.hpp"
#include "core/Result.hpp"
#include "gpu/Compositor.hpp"
#include "gpu/GpuTypes.hpp"
#include "media/AudioGraph.hpp"
#include "media/MediaEncoder.hpp"

namespace palmier::media {

// ---------------------------------------------------------------------------
// AudioRangeRenderer — where the export audio mix comes from
// ---------------------------------------------------------------------------

/// Mixes `[from, to)` of the project's audio into ONE interleaved-float buffer at
/// the Audio_Engine output format — the export audio seam (task 9.3;
/// Requirements 6.5, 6.11).
///
/// In production this is bound to `media::AudioEngine::renderRange` (task 8.4),
/// whose `mixWindow` builds an EXPORT-LOCAL `AudioGraph` per call and does all of
/// the resampling, per-clip gain, summing and [-1, 1] clamping. Nothing in this
/// file mixes audio itself: a second mixer would be a second definition of
/// "correct audio", and the range renderer exists precisely so there is only one.
///
/// The renderer is a seam rather than an `AudioEngine&` because the engine needs
/// a decoder factory, a sink and a teardown queue that an export driven by mock
/// frames has no use for, and because Requirement 6.11's "no clip on an unmuted
/// audio-bearing track" case must be reachable without any of them.
using AudioRangeRenderer =
    std::function<Result<AudioBuffer>(const Project&, Duration /*from*/, Duration /*to*/)>;

/// The fallback renderer used when an export asks for audio and no renderer was
/// injected: an export-local `AudioGraph` mixing ZERO sources into a full-length
/// buffer, i.e. exact silence for the requested range. This is the same call
/// `AudioEngine::mixWindow` makes for a range that no unmuted audio-bearing track
/// covers, so an audio-less timeline still yields one silent stream spanning the
/// whole duration (Requirement 6.11).
[[nodiscard]] AudioRangeRenderer silentAudioRangeRenderer(AudioFormat format);

// ---------------------------------------------------------------------------
// ExportCancelPredicate — how a running export is stopped
// ---------------------------------------------------------------------------

/// Consulted at the top of every frame: true stops the export at that frame
/// boundary with ErrorCode::Cancelled, after which the partial output file is
/// removed exactly as for any other mid-export failure (Requirement 7.7).
///
/// A predicate rather than a signal or a deadline: the caller
/// (`services::ExportCoordinator`) owns an atomic flag, so the frame at which an
/// export stops is decided by a value the caller can set at a known moment
/// instead of by how long anything took. May be empty (never cancelled).
using ExportCancelPredicate = std::function<bool()>;

// ---------------------------------------------------------------------------
// ExportRequest — what to export and how
// ---------------------------------------------------------------------------

/// Everything ExportEngine::run needs to render and encode the timeline.
///
///   * codec — the output video codec (gpu::CodecId).
///   * resolution — the output canvas/frame size; every frame is composited and
///     encoded at this size. Defaults to the project's canvas when left invalid
///     (zero) via run() (see below).
///   * frameRate — the output frame rate; when left invalid run() uses the
///     project's timelineFps.
///   * bitrateBitsPerSecond — target average bit rate (0 = backend default).
///   * preferHardware — request hardware encode when a compatible backend is
///     active (Requirements 10.3/10.8); falls back to software transparently.
///   * caps — capabilities of the selected device the encoder routes against.
///   * availability — which vendor HW backends are compiled into this build.
///   * outputPath / containerFormat — where the muxed output is written and the
///     container short-name (e.g. "mp4").
///   * clearColor — the canvas the compositor clears to before drawing clips
///     (opaque black by default, the conventional export background).
///   * progressInterval — the minimum cadence at which progress is emitted. The
///     default (1s) meets Requirement 11.2's "at least once per second"; tests
///     may pass a shorter interval (e.g. 0) to observe every frame.
///   * includeAudio / audio — when `includeAudio` is set the output carries
///     exactly one audio stream mixed from the same clip set, and `audio`
///     describes it (default: the Audio_Engine output format, AAC). Defaults to
///     false so an existing video-only caller is unaffected.
struct ExportRequest {
    gpu::CodecId            codec{gpu::CodecId::H264};
    Resolution              resolution{};
    FrameRate               frameRate{};
    std::int64_t            bitrateBitsPerSecond{0};
    bool                    preferHardware{true};
    gpu::GpuCaps            caps{gpu::GpuCaps::software()};
    gpu::BridgeAvailability availability{gpu::BridgeAvailability::fromBuildConfig()};
    std::filesystem::path   outputPath{};
    std::string             containerFormat{};
    gpu::RgbaColor          clearColor{gpu::RgbaColor::opaqueBlack()};
    std::chrono::milliseconds progressInterval{std::chrono::seconds{1}};
    bool                    includeAudio{false};
    AudioEncodeSpec         audio{};
    /// Consulted before each frame is rendered; true stops the export at that
    /// frame boundary and removes the partial output (Requirement 7.7).
    ExportCancelPredicate   cancelled{};
};

// ---------------------------------------------------------------------------
// ExportProgress — one progress notification
// ---------------------------------------------------------------------------

/// A single progress report delivered to the caller during run(). `percent` is a
/// monotonically non-decreasing value in [0, 100] (Requirement 11.2).
struct ExportProgress {
    int         percent{0};             ///< 0..100, monotonically non-decreasing.
    std::size_t framesRendered{0};      ///< frames composited + submitted so far.
    std::size_t totalFrames{0};         ///< total frames the export will render.
    Duration    position{Duration::zero()}; ///< timeline position of the last frame.
};

/// Progress sink invoked during run(). Called at least once per second while an
/// export is in progress, once with percent==0 at the start, and once with
/// percent==100 on successful completion. May be null (progress ignored).
using ExportProgressCallback = std::function<void(const ExportProgress&)>;

// ---------------------------------------------------------------------------
// ExportResult — the outcome of a successful export
// ---------------------------------------------------------------------------

/// The result of a completed export.
struct ExportResult {
    std::size_t           framesRendered{0}; ///< total frames rendered + encoded.
    std::size_t           totalFrames{0};    ///< the planned frame count.
    std::filesystem::path outputPath{};      ///< where the output was written.
    bool                  usedHardwareEncode{false}; ///< hardware encoder was used.
    bool                  usedSoftwareFallback{false}; ///< HW init failed -> SW.
    /// The output carries one audio stream (Requirement 6.5).
    bool                  containsAudio{false};
    /// Audio blocks submitted — one per rendered video frame interval.
    std::size_t           audioBlocks{0};
    /// Audio FRAMES (one sample per channel) submitted across the whole export.
    /// At the audio stream's sample rate this is the mixed audio duration, which
    /// spans the timeline duration and exceeds it by less than one video frame
    /// interval (Requirements 6.8, 6.11, 7.4).
    std::uint64_t         audioFrames{0};
};

// ---------------------------------------------------------------------------
// ExportEngine
// ---------------------------------------------------------------------------

/// Renders the complete timeline into a single output file, driving the
/// Compositor (one frame per timeline position, in strictly increasing
/// presentation time) and a MediaEncoder (HW-preferred, SW-fallback), while
/// reporting monotonic 0..100% progress (design "Component: Export_Engine";
/// Requirements 11.1, 11.2, 10.3, 10.8).
class ExportEngine {
public:
    /// Construct against a Compositor. The compositor must already be wired to a
    /// ClipFrameProvider (the MediaDecoder in production) so it can fetch source
    /// pixels for visible clips; a timeline position with no visible clips is
    /// rendered as the cleared canvas and needs no provider.
    ///
    /// This overload has no audio source: an export that asks for audio through
    /// it gets one silent stream spanning the timeline duration, which is exactly
    /// Requirement 6.11's outcome.
    explicit ExportEngine(gpu::Compositor& compositor) noexcept;

    /// As above, with the audio mix source injected. Production binds `audio` to
    /// `media::AudioEngine::renderRange` so the export mix and the playback mix
    /// come from the same `AudioGraph` code (task 9.3). An empty renderer behaves
    /// exactly like the single-argument constructor.
    ExportEngine(gpu::Compositor& compositor, AudioRangeRenderer audio);

    /// Render + encode the timeline using the default FFmpeg encode backend.
    ///   * `project` is only read — the source timeline is never modified.
    ///   * `progress` (optional) receives monotonic 0..100% updates.
    /// The request is validated before any rendering begins; on a mid-export
    /// failure the incomplete output file is removed. The source timeline is
    /// unchanged in every error case:
    ///   * InvalidArgument   — no output path, or an unusable frame rate/canvas.
    ///   * Unsupported       — an unencodable codec, an unknown container format,
    ///                         or an unsupported output resolution (Req 11.4).
    ///   * FailedPrecondition — an empty timeline with zero media segments (11.5).
    ///   * (encoder errors)  — from MediaEncoder::create/submit/finish; the
    ///                         partial output file is removed (Req 11.3).
    ///   * (compositor errors)— propagated from renderAt (e.g. a decode failure);
    ///                         the partial output file is removed (Req 11.3).
    /// On success the ExportResult reports the output location (Req 11.6).
    [[nodiscard]] Result<ExportResult> run(const Project& project,
                                           const ExportRequest& request,
                                           ExportProgressCallback progress = {});

    /// As above, with an injected encode-backend factory (the testing seam that
    /// lets the render loop run without FFmpeg/GPU via a mock backend).
    [[nodiscard]] Result<ExportResult> run(const Project& project,
                                           const ExportRequest& request,
                                           const EncodeBackendFactory& encodeFactory,
                                           ExportProgressCallback progress = {});

    /// The number of frames run() will render for `project` at `frameRate`: the
    /// count of whole frames that covers [0, timelineDuration], i.e.
    /// ceil(duration / frameStep), and at least 1. Exposed for callers that want
    /// to size a progress bar up front and for tests. Returns 0 for an invalid
    /// frame rate.
    [[nodiscard]] static std::size_t plannedFrameCount(const Project& project,
                                                       FrameRate frameRate) noexcept;

    /// Replace the audio mix source. Exposed so a caller that builds the engine
    /// before its audio engine exists (the composition root's construction order)
    /// can still bind the production renderer.
    void setAudioRangeRenderer(AudioRangeRenderer audio) { audio_ = std::move(audio); }

    /// True when an audio mix source is bound. When false an audio export still
    /// produces one silent stream (Requirement 6.11).
    [[nodiscard]] bool hasAudioRangeRenderer() const noexcept
    {
        return static_cast<bool>(audio_);
    }

private:
    // Shared implementation of both run() overloads. `factory` selects the
    // encode backend (the default FFmpeg factory or an injected one).
    [[nodiscard]] Result<ExportResult> runImpl(const Project& project,
                                               const ExportRequest& request,
                                               const EncodeBackendFactory& factory,
                                               const ExportProgressCallback& progress);

    gpu::Compositor&   compositor_;
    AudioRangeRenderer audio_{};
};

} // namespace palmier::media

#endif // PALMIER_MEDIA_EXPORTENGINE_HPP
