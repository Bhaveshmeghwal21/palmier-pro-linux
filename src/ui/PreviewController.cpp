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
#include <cstdint>
#include <utility>

#include "core/TimelineEngine.hpp" // timelineDuration()

namespace palmier::ui {
namespace {

/// Total timeline length of `project` — the SAME free function
/// `media::ExportEngine::plannedFrameCount` measures against, so playback and
/// export cannot disagree about where the timeline ends (Requirement 5.7).
[[nodiscard]] Duration timelineLength(const Project& project) {
    return palmier::timelineDuration(project);
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
// Pacing helpers.
// ---------------------------------------------------------------------------
void PreviewController::adoptRate(const Project& project) {
    previewRate_ = computePreviewRate(project);
    interval_ = previewRate_.frameDuration();
}

Duration PreviewController::positionFor(std::int64_t index) const {
    return previewRate_.durationForFrames(index);
}

Duration PreviewController::deadlineFor(std::int64_t index) const {
    // The deadline of frame `index` is the anchor plus the exact span from the
    // anchored frame to it — index arithmetic, never accumulation, so a rational
    // rate (24000/1001 and friends) cannot drift over a long run.
    return anchor_ + previewRate_.durationForFrames(index - baseIndex_);
}

void PreviewController::anchorPacing() {
    baseIndex_ = frameIndex_;
    anchor_ = clock_->now();
}

void PreviewController::notifyIndicator(Duration position) const {
    if (indicator_) indicator_(position);
}

// ---------------------------------------------------------------------------
// Transport (Requirements 5.4, 5.8, 5.9, 5.10).
// ---------------------------------------------------------------------------
void PreviewController::play() {
    if (state_ == PlaybackState::Playing) return;

    const Project project = projectSource_ ? projectSource_() : Project{};
    adoptRate(project);

    if (state_ == PlaybackState::Stopped) {
        // A fresh run: the next frame due is the one covering the playhead, and
        // the accounting of Requirements 5.2/5.7 starts over.
        frameIndex_ = previewRate_.framesForDuration(playhead_);
        presented_ = 0;
        dropped_ = 0;
        endOfTimeline_ = false;
        playbackNotice_.clear();
    }

    state_ = PlaybackState::Playing;
    // The frame at frameIndex_ is due immediately; the rest are paced from here.
    anchorPacing();
}

void PreviewController::pause() {
    if (state_ != PlaybackState::Playing) return;

    // Requirement 5.4: advance stops inside this call, the playhead is retained at
    // the position of the last presented frame, and that frame is presented again
    // so the surface unambiguously shows the paused position.
    state_ = PlaybackState::Paused;
    if (lastFrame_.has_value()) {
        playhead_ = lastFrame_->presentationTime;
    }
    const Project project = projectSource_ ? projectSource_() : Project{};
    presentAt(project, playhead_);
}

void PreviewController::stop() {
    // Requirement 5.8: advance stops inside this call, the playhead goes to
    // timeline position zero, and the frame for zero is presented.
    state_ = PlaybackState::Stopped;
    playhead_ = Duration::zero();
    frameIndex_ = 0;
    endOfTimeline_ = false;
    const Project project = projectSource_ ? projectSource_() : Project{};
    adoptRate(project);
    anchorPacing();
    presentAt(project, Duration::zero());
}

void PreviewController::seek(Duration position) {
    const Project project = projectSource_ ? projectSource_() : Project{};
    adoptRate(project);

    // Requirement 5.9: clamp to [0, timeline duration]. A zero-length timeline is
    // the still-preview case and has no meaningful upper bound, so only the lower
    // clamp applies there.
    const Duration total = timelineLength(project);
    Duration clamped = position.isNegative() ? Duration::zero() : position;
    if (total.isPositive() && clamped > total) {
        clamped = total;
    }

    playhead_ = clamped;
    frameIndex_ = previewRate_.framesForDuration(clamped);
    endOfTimeline_ = false;
    // Re-anchor pacing so a subsequent pump does not fire a burst of catch-up
    // frames for the time spent scrubbing.
    anchorPacing();

    presentAt(project, clamped);
}

Duration PreviewController::timelineDuration() const {
    return timelineLength(projectSource_ ? projectSource_() : Project{});
}

// ---------------------------------------------------------------------------
// Rendering.
// ---------------------------------------------------------------------------
Result<PreviewFrameInfo> PreviewController::renderAt(const Project& project, Duration position) {
    const gpu::RenderTarget target =
        gpu::RenderTarget::forCanvas(project.canvas, options_.clearColor);

    RenderPath attempt = degradedToCpu_ ? RenderPath::CpuFallback : preferredPath_;

    Result<gpu::RenderedFrame> result = renderFn_(attempt, project, position, target);

    // GPU render failed: retry once on the software path and stay there for the
    // remainder of the session, with the status-bar notice recorded (mirrors the
    // GPU-op "retry once on CPU" degradation; Requirements 10.5/10.7, 5.6).
    if (result.isError() && attempt == RenderPath::GpuActive) {
        degradedToCpu_ = true;
        if (softwareCompositingNotice_.empty()) {
            softwareCompositingNotice_ =
                "Software compositing is in use: the GPU compositing path failed (" +
                result.error().message() +
                "); frames are composited on the CPU for the rest of this session.";
        }
        attempt = RenderPath::CpuFallback;
        result = renderFn_(attempt, project, position, target);
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

void PreviewController::presentAt(const Project& project, Duration position) {
    const Result<PreviewFrameInfo> frame = renderAt(project, position);
    if (frame.isOk()) {
        playhead_ = position;
        notifyIndicator(position);
    }
    // A failure outside the pacing loop is recorded in lastError_ by renderAt and
    // leaves the previously presented frame on the surface; it does not change the
    // transport state, which the caller has just set deliberately.
}

Result<PreviewFrameInfo> PreviewController::renderFrame() {
    const Project project = projectSource_ ? projectSource_() : Project{};
    return renderAt(project, playhead_);
}

std::size_t PreviewController::pump() {
    if (state_ != PlaybackState::Playing) return 0;

    const Project project = projectSource_ ? projectSource_() : Project{};
    const Duration total = timelineLength(project);
    const bool bounded = total.isPositive();

    if (!interval_.isPositive()) {
        // Degenerate rate: pacing is impossible (previewRate_ is always >= 24 fps,
        // so this is only a defensive guard).
        return 0;
    }

    std::size_t rendered = 0;
    std::size_t steps = 0;
    while (steps < options_.maxFramesPerPump && clock_->now() >= deadlineFor(frameIndex_)) {
        // End of a bounded timeline: halt within this call, keep the last
        // presented frame on the surface, rest the playhead on the duration, and
        // report Stopped (Requirement 5.10).
        if (bounded && positionFor(frameIndex_) >= total) {
            playhead_ = total;
            state_ = PlaybackState::Stopped;
            endOfTimeline_ = true;
            notifyIndicator(playhead_);
            break;
        }

        const Duration position = positionFor(frameIndex_);
        const Duration startedAt = clock_->now();

        Result<PreviewFrameInfo> frame = renderAt(project, position);
        if (frame.isError()) {
            // Requirement 5.5: a decode/composite failure stops advancing the
            // playhead immediately, retains the last successfully presented frame,
            // reports Paused, and records a notice quoting the error — which names
            // the asset, because the clip frame provider names it.
            state_ = PlaybackState::Paused;
            playbackNotice_ = "Playback paused: " + frame.error().message();
            break;
        }

        playhead_ = position;
        ++presented_;
        ++rendered;
        ++steps;
        notifyIndicator(position);
        ++frameIndex_;

        // Drop accounting (Requirements 5.2, 5.7). The composite of the frame just
        // presented occupied `cost` of wall clock. Every following frame whose slot
        // elapsed ENTIRELY inside that composite is counted dropped and skipped,
        // rather than presented late: a composite of cost c consumes the presenting
        // frame's own slot plus one further slot for each whole interval by which c
        // exceeds the interval.
        //
        // The measure is the composite's own cost, not the clock's absolute
        // lateness, because a late CALLER (one big clock jump between pumps, which
        // is exactly how a Qt timer behaves after the event loop stalls, and how the
        // cadence tests drive a whole second at once) is a catch-up the engine
        // presents in full — not a drop the engine caused.
        const Duration cost = clock_->now() - startedAt;
        std::int64_t skips = 0;
        while (cost > interval_ * (skips + 1)) ++skips;
        for (std::int64_t i = 0; i < skips && steps < options_.maxFramesPerPump; ++i) {
            if (bounded && positionFor(frameIndex_) >= total) {
                break; // let the halt branch above own the end-of-timeline case.
            }
            ++dropped_;
            ++steps;
            ++frameIndex_;
        }
    }
    return rendered;
}

} // namespace palmier::ui
