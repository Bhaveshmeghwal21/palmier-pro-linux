// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/PreviewController.cpp — implementation of the Qt-free preview/player
// playback engine (task 19.3; Requirements 2.8, 10.7).
//
// See PreviewController.hpp for the design. The compositing math itself lives in
// the Compositor (the vendor-neutral software reference that becomes the GPU
// compute path under PALMIER_HAVE_VULKAN); this file owns the playhead clock,
// the >= 24 fps cadence, and the GPU-active / CPU-fallback path selection.

#include "ui/PreviewController.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace palmier::ui {
namespace {

/// Total timeline length of `project`: the maximum clip end across all tracks,
/// or Duration::zero() when there are no clips. (Kept local so the controller
/// does not depend on the full TimelineEngine translation unit.)
[[nodiscard]] Duration timelineLength(const Project& project) noexcept {
    Duration total = Duration::zero();
    for (const Track& track : project.tracks) {
        for (const Clip& clip : track.clips) {
            const Duration end = clip.timelineEnd();
            if (end > total) total = end;
        }
    }
    return total;
}

} // namespace

Duration SteadyPlaybackClock::now() const {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    return Duration::fromNanoseconds(static_cast<std::int64_t>(ns));
}

// ---------------------------------------------------------------------------
// Construction.
// ---------------------------------------------------------------------------
PreviewController::PreviewController(gpu::Compositor& compositor,
                                     const gpu::GpuContext& context,
                                     PreviewProjectSource projectSource,
                                     const PlaybackClock& clock, PreviewOptions options)
    : compositor_(compositor),
      projectSource_(std::move(projectSource)),
      options_(std::move(options)),
      clock_(&clock) {
    preferredPath_ = computePreferredPath(context);
    // Default composite seam: the Compositor's renderAt. In a Vulkan build this
    // is the GPU compute path when a device is active; here (and in tests) it is
    // the vendor-neutral software reference. The requested RenderPath is
    // informational for the default seam — the controller records which path it
    // asked for; a real GPU-vs-CPU dispatch can key off it.
    renderFn_ = [this](RenderPath, const Project& project, Duration position,
                       const gpu::RenderTarget& target) {
        return compositor_.renderAt(project, position, target);
    };
}

PreviewController::PreviewController(gpu::Compositor& compositor,
                                     const gpu::GpuContext& context,
                                     PreviewProjectSource projectSource,
                                     PreviewOptions options)
    : PreviewController(compositor, context, std::move(projectSource), ownedClock_,
                        std::move(options)) {
    // The delegating ctor captured `ownedClock_` by reference (it is a member,
    // already alive), so clock_ correctly points at our own steady clock.
    clock_ = &ownedClock_;
}

// ---------------------------------------------------------------------------
// Path selection (Requirement 10.7).
// ---------------------------------------------------------------------------
RenderPath PreviewController::computePreferredPath(const gpu::GpuContext& context) const noexcept {
    // GPU-active iff a real device is selected (not the software fallback) and it
    // advertises the compute capability the effect/composite kernels need.
    if (!context.isSoftwareFallback() && context.capabilities().supportsCompute) {
        return RenderPath::GpuActive;
    }
    return RenderPath::CpuFallback;
}

void PreviewController::setRenderFn(PreviewRenderFn fn) {
    if (fn) {
        renderFn_ = std::move(fn);
    } else {
        renderFn_ = [this](RenderPath, const Project& project, Duration position,
                           const gpu::RenderTarget& target) {
            return compositor_.renderAt(project, position, target);
        };
    }
}

// ---------------------------------------------------------------------------
// Preview frame rate (Requirement 2.8): project fps floored at 24.
// ---------------------------------------------------------------------------
FrameRate PreviewController::computePreviewRate(const Project& project) const noexcept {
    const FrameRate base =
        options_.targetFpsOverride.isValid() ? options_.targetFpsOverride : project.timelineFps;
    if (base.isValid() && base.toDouble() >= 24.0) {
        return base;
    }
    return FrameRate::fps24();
}

FrameRate PreviewController::previewFrameRate() const {
    return computePreviewRate(projectSource_ ? projectSource_() : Project{});
}

// ---------------------------------------------------------------------------
// Transport.
// ---------------------------------------------------------------------------
void PreviewController::play() {
    if (state_ == PlaybackState::Playing) return;

    const Project project = projectSource_ ? projectSource_() : Project{};
    previewRate_ = computePreviewRate(project);
    interval_ = previewRate_.frameDuration();

    state_ = PlaybackState::Playing;
    // The first frame is due immediately; subsequent frames are paced by interval_.
    nextDeadline_ = clock_->now();
}

void PreviewController::pause() {
    if (state_ == PlaybackState::Playing) {
        state_ = PlaybackState::Paused;
    }
}

void PreviewController::stop() {
    state_ = PlaybackState::Stopped;
    playhead_ = Duration::zero();
}

void PreviewController::seek(Duration position) {
    playhead_ = position.isNegative() ? Duration::zero() : position;
    // Re-anchor pacing so a subsequent pump does not fire a burst of catch-up
    // frames for the time spent scrubbing.
    if (state_ == PlaybackState::Playing) {
        nextDeadline_ = clock_->now();
    }
}

// ---------------------------------------------------------------------------
// Rendering.
// ---------------------------------------------------------------------------
Result<PreviewFrameInfo> PreviewController::renderCurrent(const Project& project) {
    const gpu::RenderTarget target =
        gpu::RenderTarget::forCanvas(project.canvas, options_.clearColor);

    RenderPath attempt = degradedToCpu_ ? RenderPath::CpuFallback : preferredPath_;

    Result<gpu::RenderedFrame> result = renderFn_(attempt, project, playhead_, target);

    // GPU render failed: retry once on the software path and stay there
    // (mirrors the GPU-op "retry once on CPU" degradation, Requirement 10.5/10.7).
    if (result.isError() && attempt == RenderPath::GpuActive) {
        degradedToCpu_ = true;
        attempt = RenderPath::CpuFallback;
        result = renderFn_(attempt, project, playhead_, target);
    }

    if (result.isError()) {
        lastError_ = std::move(result).error();
        return err<PreviewFrameInfo>(*lastError_);
    }

    gpu::RenderedFrame frame = std::move(result).value();
    PreviewFrameInfo info{frame.width(), frame.height(), frame.layerCount(),
                          frame.presentationTime(), attempt};

    if (sink_) sink_(frame, attempt);

    lastFrame_ = info;
    lastError_.reset();
    return info;
}

Result<PreviewFrameInfo> PreviewController::renderFrame() {
    const Project project = projectSource_ ? projectSource_() : Project{};
    return renderCurrent(project);
}

std::size_t PreviewController::pump() {
    if (state_ != PlaybackState::Playing) return 0;

    const Project project = projectSource_ ? projectSource_() : Project{};
    const Duration total = timelineLength(project);
    const bool bounded = total.isPositive();

    std::size_t rendered = 0;
    while (rendered < options_.maxFramesPerPump && clock_->now() >= nextDeadline_) {
        Result<PreviewFrameInfo> frame = renderCurrent(project);
        if (frame.isError()) {
            // Surface the error and hold playback so the caller can react.
            state_ = PlaybackState::Paused;
            break;
        }
        ++rendered;

        // Advance the playhead by one preview frame.
        if (interval_.isPositive()) {
            playhead_ += interval_;
            nextDeadline_ += interval_;
        } else {
            // Degenerate rate: present a single frame and stop pacing to avoid
            // an unbounded loop (previewRate_ is always >= 24, so this is only a
            // defensive guard).
            break;
        }

        // Auto-stop at the end of a bounded timeline.
        if (bounded && playhead_ >= total) {
            playhead_ = total;
            state_ = PlaybackState::Stopped;
            break;
        }
    }
    return rendered;
}

} // namespace palmier::ui
