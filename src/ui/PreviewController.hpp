// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/PreviewController.hpp — the Qt-free preview/player playback engine
// (task 19.3; Requirements 2.8, 10.7).
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

#ifndef PALMIER_UI_PREVIEWCONTROLLER_HPP
#define PALMIER_UI_PREVIEWCONTROLLER_HPP

#include <cstddef>
#include <functional>
#include <optional>

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
    [[nodiscard]] bool degradedToCpu() const noexcept { return degradedToCpu_; }

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
    [[nodiscard]] Duration playhead() const noexcept { return playhead_; }

    /// Begin playback from the current playhead. Anchors the pacing to the clock
    /// so the first frame is due immediately. No-op if already playing.
    void play();

    /// Stop advancing the playhead, holding it where it is.
    void pause();

    /// Stop and reset the playhead to the start (0).
    void stop();

    /// Move the playhead to `position` (clamped to >= 0). Does not change the
    /// playing state, but re-anchors pacing so play continues smoothly.
    void seek(Duration position);

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

    /// Override the composite seam (a real GPU dispatch, or a test double).
    /// Passing an empty function restores the default (Compositor::renderAt).
    void setRenderFn(PreviewRenderFn fn);

private:
    [[nodiscard]] RenderPath computePreferredPath(const gpu::GpuContext& context) const noexcept;
    [[nodiscard]] FrameRate computePreviewRate(const Project& project) const noexcept;

    // Render one frame at the current playhead against `project`, applying the
    // GPU->CPU fallback policy. Updates lastFrame_/lastError_/degradedToCpu_.
    [[nodiscard]] Result<PreviewFrameInfo> renderCurrent(const Project& project);

    gpu::Compositor&      compositor_;
    PreviewProjectSource  projectSource_;
    PreviewOptions        options_;
    SteadyPlaybackClock   ownedClock_{};   ///< Used when no external clock is supplied.
    const PlaybackClock*  clock_{nullptr};

    PreviewRenderFn  renderFn_{};
    PreviewFrameSink sink_{};

    RenderPath    preferredPath_{RenderPath::CpuFallback};
    bool          degradedToCpu_{false};

    PlaybackState state_{PlaybackState::Stopped};
    Duration      playhead_{Duration::zero()};

    // Pacing state (valid while Playing).
    FrameRate previewRate_{FrameRate::fps24()};
    Duration  interval_{FrameRate::fps24().frameDuration()};
    Duration  nextDeadline_{Duration::zero()};

    std::optional<PreviewFrameInfo> lastFrame_{};
    std::optional<Error>            lastError_{};
};

} // namespace palmier::ui

#endif // PALMIER_UI_PREVIEWCONTROLLER_HPP
