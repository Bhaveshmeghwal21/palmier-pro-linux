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

#include "core/Duration.hpp"
#include "core/FrameRate.hpp"
#include "core/Project.hpp"
#include "core/Resolution.hpp"
#include "core/Result.hpp"
#include "gpu/Compositor.hpp"
#include "gpu/GpuTypes.hpp"
#include "media/MediaEncoder.hpp"

namespace palmier::media {

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
    explicit ExportEngine(gpu::Compositor& compositor) noexcept;

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

private:
    // Shared implementation of both run() overloads. `factory` selects the
    // encode backend (the default FFmpeg factory or an injected one).
    [[nodiscard]] Result<ExportResult> runImpl(const Project& project,
                                               const ExportRequest& request,
                                               const EncodeBackendFactory& factory,
                                               const ExportProgressCallback& progress);

    gpu::Compositor& compositor_;
};

} // namespace palmier::media

#endif // PALMIER_MEDIA_EXPORTENGINE_HPP
