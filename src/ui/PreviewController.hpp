// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/PreviewController.hpp — the Qt-free preview/player playback engine
// (task 19.3; Requirements 2.8, 10.7 — extended by task 7.5 of
// end-to-end-editor-integration with the complete transport: Requirements 5.2,
// 5.3, 5.4, 5.5, 5.6, 5.7, 5.8, 5.9, 5.10).
//
// This is the headless heart of the editor's Preview / Player view (design.md
// "Preview / Player View"). It owns the playhead clock, drives the Compositor to
// produce a composited frame at the playhead at a MINIMUM of 24 frames per second
// (Requirement 2.8), and selects the GPU-active compositing path when the
// GpuContext advertises it, degrading to the CPU/software path otherwise or on a
// GPU render failure (Requirement 10.7 / the layer's "never fail for no GPU"
// contract).
//
// Deliberately Qt-free so it is unit-testable with no display, no Qt, no Vulkan,
// and no GPU: the wall clock is injected (PlaybackClock), the project comes from
// an injectable source, and the actual composite call goes through a RenderFn
// seam (defaulting to Compositor::renderAt — the vendor-neutral software
// reference, which becomes the GPU compute path in a Vulkan build). The thin
// QWidget/QML preview surface (built only behind PALMIER_HAVE_QT) owns one of
// these and simply paints the frames it produces.
//
// Playback model
// --------------
//   * The controller presents frames on a fixed cadence: the preview frame rate
//     is the project's timelineFps, floored at 24 fps so the "minimum 24 fps"
//     guarantee holds even for slower timelines (23.976 -> 24, 12 -> 24, 30 -> 30).
//   * While Playing, each presented frame advances the playhead by one preview
//     frame interval. pump() renders every frame whose scheduled wall-clock
//     deadline has passed (per the injected clock), so a caller can drive it from
//     a Qt timer, an export loop, or a test's manual clock and always get the
//     right cadence.
//   * Playback auto-stops when the playhead reaches a positive timeline duration;
//     an empty/zero-length timeline stays put and keeps presenting the cleared
//     canvas (a still preview) without auto-stopping.
//
// Transport model completed by task 7.5 (Requirements 5.2-5.10)
// ------------------------------------------------------------
// The pieces below are what stage 7 of end-to-end-editor-integration adds; they
// are stated here because each one is an externally observable contract:
//
//   * **Frame identity.** Frame k of a run is presented at
//     `previewFrameRate().durationForFrames(k)` — the SAME index-to-position
//     arithmetic `media::ExportEngine` uses for its render loop, so playback and
//     export agree frame for frame (Requirement 5.7). Positions are computed from
//     the index rather than accumulated, because `frameDuration()` truncates for
//     rational rates and accumulation would drift.
//   * **Playhead meaning.** While playing, `playhead()` is the position of the
//     LAST PRESENTED frame, which is what Requirement 5.4 requires pause to
//     retain. `frameIndex()` is the index of the NEXT frame due. At an
//     end-of-timeline halt the playhead rests on the timeline duration
//     (Requirement 5.10).
//   * **Pacing and drops.** Frame k is due at `anchor + durationForFrames(k)`.
//     After a frame is composited, any following frame whose slot has ALREADY
//     fully elapsed (more than one interval late per the design's pacing rule) is
//     counted as **dropped** and skipped rather than presented late, so
//     `presentedFrameCount() + droppedFrameCount()` equals the export planner's
//     frame count for the same timeline and rate (Requirements 5.2, 5.7). A late
//     *caller* (one big clock jump between pumps) is a catch-up, not a drop: the
//     drop test is the time the composite itself consumed.
//   * **The audio clock is the master clock when there is one** (task 8.7;
//     design.md D7, Requirement 6.3). An OPTIONAL `AudioMasterClock` may be
//     installed. When it is installed and yields a position, `pump()` paces
//     against that position instead of the wall clock: a frame more than one
//     interval BEHIND it is counted dropped and skipped, a frame more than one
//     interval AHEAD of it waits, and anything in between is presented. With no
//     audio clock installed — or when it yields `nullopt` because the engine is
//     stopped or audio output was unavailable — pacing is exactly the wall-clock
//     behaviour described above, which is what keeps video running at the project
//     frame rate with no audio device (Requirement 6.7).
//   * **Playhead indicator.** Every presented frame notifies the
//     `PlayheadIndicatorSink` with that frame's position, as do seek, pause and
//     stop. Since the preview cadence is at least 24 fps, the indicator is
//     updated far more often than the 10 Hz floor, and the position it carries is
//     exactly the presented frame's (Requirement 5.3).
//   * **Decode failure.** A provider/composite error while playing pauses
//     playback, retains the last good frame, and records `playbackNotice()`,
//     which quotes the underlying error — and therefore names the asset, because
//     `media::DecoderClipFrameProvider` names it (Requirement 5.5).
//   * **Seek clamping.** `seek()` clamps to [0, timeline duration] and presents
//     the clamped position (Requirement 5.9). A zero-length timeline has no
//     meaningful upper bound, so only the lower clamp applies there and the still
//     preview keeps working.
//   * **Software-compositing fallback.** A GPU render failure degrades to the
//     software path for the REMAINDER OF THE SESSION (`degradedToCpu()`,
//     `activePath()`) and records `softwareCompositingNotice()` for the shell's
//     status bar (Requirement 5.6).

#ifndef PALMIER_UI_PREVIEWCONTROLLER_HPP
#define PALMIER_UI_PREVIEWCONTROLLER_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "core/Duration.hpp"
#include "core/Error.hpp"
#include "core/FrameRate.hpp"
#include "core/Project.hpp"
#include "core/Result.hpp"
#include "gpu/Compositor.hpp"
#include "gpu/GpuContext.hpp"

namespace palmier::ui {

/// Which compositing path a frame was rendered on.
///   * GpuActive    — the GPU compute path (selected when the GpuContext holds a
///                    real device whose capabilities advertise compute).
///   * CpuFallback  — the vendor-neutral software path (no compatible GPU, or a
///                    degrade after a GPU render failure).
enum class RenderPath {
    GpuActive,
    CpuFallback,
};

[[nodiscard]] constexpr const char* toString(RenderPath path) noexcept {
    return path == RenderPath::GpuActive ? "GpuActive" : "CpuFallback";
}

/// The transport state of the player.
enum class PlaybackState {
    Stopped, ///< Not playing; playhead at its reset (0) or a sought position.
    Playing, ///< Advancing the playhead with wall-clock time.
    Paused,  ///< Not playing; playhead held at its current position.
};

/// Lightweight, copyable description of the frame most recently presented. The
/// pixels themselves are handed to the (optional) FrameSink; this record carries
/// only the metadata the UI/tests reason about.
struct PreviewFrameInfo {
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::size_t   layerCount{0};                    ///< Visible clip layers composited (0 = cleared canvas).
    Duration      presentationTime{Duration::zero()}; ///< Playhead position of this frame.
    RenderPath    path{RenderPath::CpuFallback};      ///< Path this frame was rendered on.
};

/// Tunables for the controller.
struct PreviewOptions {
    /// Force a specific preview frame rate instead of deriving it from the
    /// project's timelineFps. Invalid (default) means "derive from the project".
    /// Either way the effective rate is floored at 24 fps (Requirement 2.8).
    FrameRate targetFpsOverride{};

    /// Canvas clear color used when a position has no visible clips.
    gpu::RgbaColor clearColor{gpu::RgbaColor::opaqueBlack()};

    /// Safety cap on frames rendered in a single pump() call, so a large clock
    /// jump (e.g. a stall) cannot spin unbounded.
    std::size_t maxFramesPerPump{240};
};

/// Monotonic wall clock the controller paces playback against. Injectable so
/// tests can drive an exact, deterministic cadence.
class PlaybackClock {
public:
    virtual ~PlaybackClock() = default;
    /// A monotonically non-decreasing time point expressed as a Duration since
    /// an arbitrary fixed epoch.
    [[nodiscard]] virtual Duration now() const = 0;
};

/// Production clock backed by std::chrono::steady_clock.
class SteadyPlaybackClock final : public PlaybackClock {
public:
    [[nodiscard]] Duration now() const override;
};

/// Supplies the current project to composite. A value snapshot keeps the
/// controller decoupled from the TimelineEngine (which offers exactly such a
/// snapshot()).
using PreviewProjectSource = std::function<Project()>;

/// The composite seam. Given the path to render on, the project, the playhead
/// position, and the output target, produce the composited frame. Defaults to
/// Compositor::renderAt; overridable for a real GPU dispatch or for tests.
using PreviewRenderFn =
    std::function<Result<gpu::RenderedFrame>(RenderPath, const Project&, Duration,
                                             const gpu::RenderTarget&)>;

/// Optional consumer of each presented frame (e.g. the Qt surface uploads the
/// pixels to a texture). Receives the frame and the path it was rendered on.
using PreviewFrameSink = std::function<void(const gpu::RenderedFrame&, RenderPath)>;

/// The OPTIONAL audio master clock (task 8.7; Requirements 6.3, 6.7).
///
/// design.md D7 "Decision — A/V sync" makes the audio sink the clock: video slews
/// to audio, audio is never resampled to chase video. The composition root binds
/// this to `media::AudioEngine::presentationPosition()`; `pump()` reads it once
/// per call and paces against it instead of the wall clock.
///
/// It is a `std::function` returning an `optional` for two reasons, and both are
/// load-bearing:
///
///   * **Optional by absence.** A controller with no audio clock installed — the
///     default, and what every transport/pacing test uses — paces exactly as it
///     did before task 8.7, off the injected `PlaybackClock`. Adding audio to the
///     application therefore cannot change the meaning of the stage-7 pacing
///     contract for callers that have no audio engine.
///   * **Optional by value.** Returning `nullopt` means "the audio clock is not
///     currently authoritative": the engine is stopped, or no output device could
///     be opened and audio is suppressed. `pump()` then falls back to wall-clock
///     pacing for that call, which is what keeps video running at the project
///     frame rate when audio is unavailable (Requirement 6.7).
///
/// It is a plain callable rather than a reference to the engine so that
/// `PreviewController` keeps depending on nothing from `media` — and stays
/// Qt-free and testable in both build trees.
using AudioMasterClock = std::function<std::optional<Duration>()>;

/// Optional consumer of playhead-indicator updates (Requirement 5.3): the
/// timeline panel's playhead marker. Called with the position of the frame just
/// presented — so the displayed position is never ahead of, or behind, the frame
/// actually on the preview surface. The Qt timeline panel installs this in task
/// 11.3; tests record the calls against the injected clock.
using PlayheadIndicatorSink = std::function<void(Duration)>;

class PreviewController {
public:
    /// Construct against a Compositor and the GpuContext whose capabilities
    /// decide the preferred render path, with an injected wall clock.
    PreviewController(gpu::Compositor& compositor, const gpu::GpuContext& context,
                      PreviewProjectSource projectSource, const PlaybackClock& clock,
                      PreviewOptions options = {});

    /// As above, using an internal steady clock (the production convenience).
    PreviewController(gpu::Compositor& compositor, const gpu::GpuContext& context,
                      PreviewProjectSource projectSource, PreviewOptions options = {});

    PreviewController(const PreviewController&) = delete;
    PreviewController& operator=(const PreviewController&) = delete;

    // --- Render-path selection (Requirement 10.7) --------------------------

    /// The path preferred from the GpuContext's capabilities, ignoring any
    /// runtime degrade. GpuActive iff the context is not the software fallback
    /// and its capabilities advertise compute.
    [[nodiscard]] RenderPath preferredPath() const noexcept { return preferredPath_; }

    /// The path currently in effect: CpuFallback once a GPU render has failed and
    /// the controller has degraded, otherwise the preferred path.
    [[nodiscard]] RenderPath activePath() const noexcept {
        return degradedToCpu_ ? RenderPath::CpuFallback : preferredPath_;
    }

    /// True iff the active path is the GPU compute path.
    [[nodiscard]] bool isGpuActive() const noexcept {
        return activePath() == RenderPath::GpuActive;
    }

    /// True iff the controller has degraded from GPU to CPU after a failure.
    /// Once true it stays true for the remainder of the session, which is what
    /// Requirement 5.6's "report the active compositing path as software through a
    /// public accessor for the remainder of the session" asks for.
    [[nodiscard]] bool degradedToCpu() const noexcept { return degradedToCpu_; }

    /// The status-bar notice for Requirement 5.6, or empty while the GPU path is
    /// still in use. Set exactly once, when a runtime GPU render failure degrades
    /// compositing to software, and never cleared for the rest of the session.
    [[nodiscard]] const std::string& softwareCompositingNotice() const noexcept {
        return softwareCompositingNotice_;
    }

    // --- Preview frame rate (Requirement 2.8) ------------------------------

    /// The effective preview frame rate for the current project (>= 24 fps).
    [[nodiscard]] FrameRate previewFrameRate() const;

    /// Duration of one preview frame at the effective rate.
    [[nodiscard]] Duration frameInterval() const { return previewFrameRate().frameDuration(); }

    /// The effective preview frame rate as a floating-point fps (>= 24.0).
    [[nodiscard]] double previewFps() const { return previewFrameRate().toDouble(); }

    // --- Transport ---------------------------------------------------------

    [[nodiscard]] PlaybackState state() const noexcept { return state_; }
    [[nodiscard]] bool isPlaying() const noexcept { return state_ == PlaybackState::Playing; }

    /// While playing (and after a pause) this is the position of the LAST
    /// PRESENTED frame; at an end-of-timeline halt it rests on the timeline
    /// duration; after stop() it is zero (Requirements 5.4, 5.8, 5.10).
    [[nodiscard]] Duration playhead() const noexcept { return playhead_; }

    /// Index of the NEXT frame due in the current run (frame k is presented at
    /// `previewFrameRate().durationForFrames(k)`).
    [[nodiscard]] std::int64_t frameIndex() const noexcept { return frameIndex_; }

    /// Begin playback from the current playhead. Anchors the pacing to the clock
    /// so the first frame is due immediately. No-op if already playing. Starting
    /// from the Stopped state begins a fresh run and resets the presented/dropped
    /// accounting; resuming from Paused continues the current run.
    void play();

    /// Stop advancing the playhead within this call, retain it at the position of
    /// the last presented frame, present that frame, and report Paused
    /// (Requirement 5.4). No-op unless playing.
    void pause();

    /// Stop advancing the playhead within this call, set it to timeline position
    /// zero, present the frame for zero, and report Stopped (Requirement 5.8).
    void stop();

    /// Move the playhead to `clamp(position, 0, timelineDuration())`, present the
    /// frame for the clamped position, and report the clamped position as the
    /// current playhead — whether playback was running or halted (Requirement
    /// 5.9). A zero-length timeline has no upper bound to clamp against (the still
    /// preview), so only the lower clamp applies there. Re-anchors pacing so play
    /// continues smoothly from the new position.
    void seek(Duration position);

    /// The timeline duration of the current project snapshot: the same value
    /// `timelineDuration()` reports to the export planner, and the upper bound
    /// `seek()` clamps against.
    [[nodiscard]] Duration timelineDuration() const;

    // --- Frame accounting (Requirements 5.2, 5.7) ---------------------------

    /// Frames presented by the pacing loop in the current run.
    [[nodiscard]] std::uint64_t presentedFrameCount() const noexcept { return presented_; }

    /// Frames whose presentation slot had already fully elapsed when the preceding
    /// composite finished, and which were therefore skipped rather than presented
    /// late. `presentedFrameCount() + droppedFrameCount()` equals
    /// `media::ExportEngine::plannedFrameCount(project, previewFrameRate())` after
    /// a run that played from zero to the end of a bounded timeline.
    [[nodiscard]] std::uint64_t droppedFrameCount() const noexcept { return dropped_; }

    /// True once a run has halted because the playhead reached the timeline
    /// duration (Requirement 5.10). Cleared when a new run starts.
    [[nodiscard]] bool reachedEndOfTimeline() const noexcept { return endOfTimeline_; }

    /// The user-facing notice for a decode/composite failure that paused playback
    /// (Requirement 5.5), quoting the underlying error — which names the asset,
    /// because the decoder-backed clip frame provider names it. Empty until such a
    /// failure occurs; cleared when a new run starts.
    [[nodiscard]] const std::string& playbackNotice() const noexcept { return playbackNotice_; }

    // --- Rendering ---------------------------------------------------------

    /// Render exactly one frame at the current playhead (e.g. for a paused
    /// scrub/refresh). Selects the active path and falls back to CPU on a GPU
    /// failure. Does not advance the playhead.
    [[nodiscard]] Result<PreviewFrameInfo> renderFrame();

    /// Advance playback per the injected clock, rendering every frame whose
    /// scheduled deadline has passed, and return how many frames were presented.
    /// Returns 0 when not playing. On a render error, records it, transitions to
    /// Paused, and returns the frames presented before the error.
    std::size_t pump();

    /// The most recently presented frame's metadata, if any.
    [[nodiscard]] const std::optional<PreviewFrameInfo>& lastFrame() const noexcept {
        return lastFrame_;
    }

    /// The most recent render error, if the last render failed.
    [[nodiscard]] const std::optional<Error>& lastError() const noexcept { return lastError_; }

    // --- Seams -------------------------------------------------------------

    /// Install a consumer for presented frames (the Qt surface uses this).
    void setFrameSink(PreviewFrameSink sink) { sink_ = std::move(sink); }

    /// Install the playhead-indicator consumer (the Qt timeline panel uses this).
    void setPlayheadIndicator(PlayheadIndicatorSink sink) { indicator_ = std::move(sink); }

    /// Override the composite seam (a real GPU dispatch, or a test double).
    /// Passing an empty function restores the default (Compositor::renderAt).
    void setRenderFn(PreviewRenderFn fn);

    /// Install (or, with an empty function, remove) the audio master clock
    /// (task 8.7; Requirement 6.3). While installed AND yielding a position,
    /// `pump()` paces video against the audio position rather than the wall
    /// clock: a frame more than one interval behind it is counted dropped and
    /// skipped, a frame more than one interval ahead of it waits, and anything in
    /// between is presented. The composition root binds this to the one
    /// `media::AudioEngine`.
    void setAudioMasterClock(AudioMasterClock clock) { audioClock_ = std::move(clock); }

    /// True when an audio master clock has been installed. It may still yield
    /// `nullopt` per call, in which case that pump paces off the wall clock.
    [[nodiscard]] bool hasAudioMasterClock() const noexcept {
        return static_cast<bool>(audioClock_);
    }

    /// The audio position the most recent `pump()` paced against, or empty when
    /// that pump used the wall clock (no clock installed, or the clock yielded
    /// `nullopt`). Observability for the A/V skew bound of Requirement 6.3.
    [[nodiscard]] const std::optional<Duration>& lastAudioPosition() const noexcept {
        return lastAudioPosition_;
    }

private:
    [[nodiscard]] RenderPath computePreferredPath(const gpu::GpuContext& context) const noexcept;
    [[nodiscard]] FrameRate computePreviewRate(const Project& project) const noexcept;

    // Render one frame at `position` against `project`, applying the GPU->CPU
    // fallback policy. Updates lastFrame_/lastError_/degradedToCpu_ (and, on a
    // degrade, softwareCompositingNotice_).
    [[nodiscard]] Result<PreviewFrameInfo> renderAt(const Project& project, Duration position);

    // Present the frame for `position` outside the pacing loop (seek/pause/stop):
    // renders it, moves the playhead to it and notifies the indicator. Errors are
    // recorded in lastError_ without changing the transport state.
    void presentAt(const Project& project, Duration position);

    // Notify the playhead-indicator sink, if installed.
    void notifyIndicator(Duration position) const;

    // Wall-clock deadline of frame `index` in the current run.
    [[nodiscard]] Duration deadlineFor(std::int64_t index) const;

    // Timeline position of frame `index` in the current run.
    [[nodiscard]] Duration positionFor(std::int64_t index) const;

    // Re-anchor pacing so the frame at frameIndex_ is due immediately.
    void anchorPacing();

    // Adopt `project`'s preview rate into previewRate_/interval_.
    void adoptRate(const Project& project);

    gpu::Compositor&      compositor_;
    PreviewProjectSource  projectSource_;
    PreviewOptions        options_;
    SteadyPlaybackClock   ownedClock_{};   ///< Used when no external clock is supplied.
    const PlaybackClock*  clock_{nullptr};

    PreviewRenderFn      renderFn_{};
    PreviewFrameSink     sink_{};
    PlayheadIndicatorSink indicator_{};
    AudioMasterClock     audioClock_{};
    std::optional<Duration> lastAudioPosition_{};

    RenderPath    preferredPath_{RenderPath::CpuFallback};
    bool          degradedToCpu_{false};
    std::string   softwareCompositingNotice_{};

    PlaybackState state_{PlaybackState::Stopped};
    Duration      playhead_{Duration::zero()};

    // Pacing state. Frame `index` of the current run is presented at
    // positionFor(index) and is due at deadlineFor(index); both are computed from
    // the index (never accumulated) so a rational frame rate cannot drift.
    FrameRate    previewRate_{FrameRate::fps24()};
    Duration     interval_{FrameRate::fps24().frameDuration()};
    std::int64_t frameIndex_{0};  ///< Index of the next frame due.
    std::int64_t baseIndex_{0};   ///< Index the current pacing anchor refers to.
    Duration     anchor_{Duration::zero()}; ///< Wall clock of baseIndex_'s deadline.

    // Frame accounting (Requirements 5.2, 5.7) and the notices (5.5, 5.10).
    std::uint64_t presented_{0};
    std::uint64_t dropped_{0};
    bool          endOfTimeline_{false};
    std::string   playbackNotice_{};

    std::optional<PreviewFrameInfo> lastFrame_{};
    std::optional<Error>            lastError_{};
};

} // namespace palmier::ui

#endif // PALMIER_UI_PREVIEWCONTROLLER_HPP
