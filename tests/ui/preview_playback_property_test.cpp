// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/ui/preview_playback_property_test.cpp — the playback transport and
// pacing properties (task 7.6 of the end-to-end-editor-integration spec;
// Requirements 5.2, 5.3, 5.4, 5.5, 5.7, 5.8, 5.9, 5.10).
//
// Six properties live here, all of them about ui::PreviewController — the
// Playback_Engine transport the composition root constructs (task 7.5):
//
//   * Property 21 — presentation rate stays within bounds and drops stay under 5%
//   * Property 22 — playhead indicator cadence
//   * Property 23 — a decode failure pauses and retains the last good frame
//   * Property 24 — playback frame accounting matches the export planner
//   * Property 79 — each transport command reaches its specified resting state
//   * Property 25 — seek clamps to the timeline bounds
//
// Everything runs on an INJECTED PlaybackClock. Nothing in this file sleeps.
// ----------------------------------------------------------------------------
// The whole subject here is timing: Requirement 5.2's rate window, Requirement
// 5.3's 10 Hz indicator cadence and 100 ms freshness, and the 100 ms / 500 ms
// bounds of Requirements 5.4/5.5/5.8/5.9/5.10. A test that drove those with
// std::this_thread::sleep_for would be both slow (a single 2-second timeline
// would cost 2 seconds of wall clock, times 100 generated cases, times six
// properties) and flaky (a loaded CI host misses a 100 ms bound for reasons that
// have nothing to do with the code). So:
//
//   * `ManualClock` is the only source of time. The test advances it explicitly.
//   * Composite LATENCY is injected by advancing that same clock from inside the
//     render seam, which is exactly what a slow GPU/decoder looks like to the
//     transport: the clock has moved on by the time the frame comes back. That is
//     how the drop accounting of Requirements 5.2/5.7 is exercised.
//   * Every "within N milliseconds" clause is asserted as "no further advance
//     after the command, with the clock moved on by more than N milliseconds and
//     the pump driven again" — a stronger statement than a wall-clock measurement,
//     and a deterministic one.
//
// Two documented readings of the requirement text
// ----------------------------------------------------------------------------
//  1. **"The project frame rate" in Requirement 5.2 means the EFFECTIVE preview
//     rate.** The preview cadence is the project rate floored at 24 fps (the
//     pre-existing minimum-24-fps preview contract, asserted by
//     tests/ui/preview_controller_test.cpp). For a 12 fps project the engine
//     therefore presents 24 frames per second, which is inside
//     [min(rate, 24), rate + 1] only when `rate` is read as the effective preview
//     rate (24) rather than the project's declared 12. Both clauses of the
//     requirement are then satisfiable simultaneously, and the floor is what the
//     user actually sees, so that is the reading used here — via
//     `PreviewController::previewFrameRate()`.
//  2. **The rate window is net of the 5 % drop allowance.** The two clauses of
//     Requirement 5.2 are one sentence: at least `min(fps, 24)` frames per
//     rolling second, AND no more than 5 % of planned frames dropped. At exactly
//     24 fps those cannot both be tight — a single dropped frame puts some window
//     at 23. The lower bound asserted here is therefore
//     `min(effectiveFps, 24) * (1 - 0.05)`, i.e. the floor as relaxed by the
//     allowance the same sentence grants, and the 5 % drop bound is asserted
//     separately and exactly.
//
// What the generators deliberately constrain
// ----------------------------------------------------------------------------
// Requirement 5.2 is a promise about a host that can keep up: no engine can hold
// a 5 % drop bound if every composite takes ten frame intervals. The latency
// generator therefore models a host that composites inside one frame interval,
// with a PERIODIC stall of two intervals every K >= 40 frames. That bounds the
// drop ratio by 1/40 = 2.5 % by construction and bounds the drops inside any one
// rolling second, so the property is deterministic rather than "usually true" —
// a random spike distribution would occasionally cluster two stalls in one second
// and make the test flaky without saying anything about the engine.
//
// No Qt, no Vulkan, no GPU, no FFmpeg, no media files and no temporary files are
// involved: the compositor runs the vendor-neutral software reference on
// gpu::GpuContext::softwareFallback(), and clip pixels come from a synthetic
// provider. PreviewController itself is Qt-free, which is what lets this suite
// build and run in both the UI and the headless tree.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include "core/Clip.hpp"
#include "core/Duration.hpp"
#include "core/Error.hpp"
#include "core/FrameRate.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/Project.hpp"
#include "core/Resolution.hpp"
#include "core/Result.hpp"
#include "core/Track.hpp"
#include "core/Uuid.hpp"
#include "gpu/Compositor.hpp"
#include "gpu/GpuContext.hpp"
#include "media/ExportEngine.hpp"
#include "ui/PreviewController.hpp"

namespace palmier::ui {
namespace {

// ---------------------------------------------------------------------------
// The injected clock — the only source of time in this file.
// ---------------------------------------------------------------------------

class ManualClock final : public PlaybackClock {
public:
    [[nodiscard]] Duration now() const override { return now_; }
    void advance(Duration d) noexcept { now_ += d; }

private:
    Duration now_{Duration::zero()};
};

// ---------------------------------------------------------------------------
// Recording sinks
// ---------------------------------------------------------------------------

/// One presented frame, stamped with the clock as it stood when the frame
/// reached the sink (i.e. after its composite latency).
struct PresentedFrame {
    Duration      wall{Duration::zero()};
    Duration      position{Duration::zero()};
    std::uint32_t width{0};
    std::uint32_t height{0};
};

/// One playhead-indicator update, stamped with the clock and paired with the
/// position of the frame presented at that moment (Requirement 5.3's "within
/// 100 milliseconds of the position of the frame presented at that moment").
struct IndicatorUpdate {
    Duration wall{Duration::zero()};
    Duration displayed{Duration::zero()};
    Duration presentedFramePosition{Duration::zero()};
    bool     hadPresentedFrame{false};
};

/// Everything one playback run records.
struct RunRecord {
    std::vector<PresentedFrame>  frames{};
    std::vector<IndicatorUpdate> indicator{};
    std::uint64_t                renderCalls{0};
};

// ---------------------------------------------------------------------------
// Project construction
// ---------------------------------------------------------------------------

constexpr Duration kOneSecond = Duration::fromSeconds(1.0);

/// A synthetic asset reference. `sourcePath` is what a decode failure names, so
/// Property 23 can assert the message identifies the asset.
[[nodiscard]] MediaAssetRef makeAssetRef(const std::string& path) {
    MediaAssetRef ref;
    ref.assetId = Uuid::generateV4();
    ref.sourcePath = path;
    return ref;
}

[[nodiscard]] Clip makeClip(const MediaAssetRef& asset, Duration start, Duration length) {
    Clip clip;
    clip.id = Uuid::generateV4();
    clip.assetRef = asset;
    clip.timelineStart = start;
    clip.sourceIn = Duration::zero();
    clip.sourceOut = length;
    clip.opacity = 1.0;
    return clip;
}

[[nodiscard]] Track makeVideoTrack(std::vector<Clip> clips) {
    Track track;
    track.id = Uuid::generateV4();
    track.kind = TrackKind::Video;
    track.clips = std::move(clips);
    return track;
}

/// A project whose timeline duration is EXACTLY `total`: track 0 carries one clip
/// spanning [0, total), and `extraTracks` further video tracks each carry one clip
/// inside that span (so multi-track overlap is exercised without moving the end).
[[nodiscard]] Project makeTimeline(FrameRate fps, Resolution canvas, Duration total,
                                   int extraTracks, const MediaAssetRef& asset) {
    Project project;
    project.id = Uuid::generateV4();
    project.name = "preview-playback-property";
    project.timelineFps = fps;
    project.canvas = canvas;

    if (total.isPositive()) {
        project.tracks.push_back(makeVideoTrack({makeClip(asset, Duration::zero(), total)}));
        for (int i = 0; i < extraTracks; ++i) {
            // Half-length clips starting at the timeline origin: they overlap the
            // base track (so more than one layer is composited) and cannot extend
            // the timeline past `total`.
            const Duration half = Duration::fromNanoseconds(total.ticks() / 2);
            if (half.isPositive()) {
                project.tracks.push_back(
                    makeVideoTrack({makeClip(asset, Duration::zero(), half)}));
            } else {
                project.tracks.push_back(makeVideoTrack({}));
            }
        }
    } else {
        // A zero-length timeline: tracks with no clips at all.
        for (int i = 0; i <= extraTracks; ++i) {
            project.tracks.push_back(makeVideoTrack({}));
        }
    }
    project.assets.push_back(asset);
    return project;
}

// --- Structural project equality (Requirement 5.5's "project unchanged") -----

[[nodiscard]] bool clipsEqual(const Clip& a, const Clip& b) {
    return a.id == b.id && a.assetRef.assetId == b.assetRef.assetId &&
           a.assetRef.sourcePath == b.assetRef.sourcePath &&
           a.timelineStart == b.timelineStart && a.sourceIn == b.sourceIn &&
           a.sourceOut == b.sourceOut && a.effects.size() == b.effects.size() &&
           a.gain == b.gain && a.opacity == b.opacity;
}

[[nodiscard]] bool projectsEqual(const Project& a, const Project& b) {
    if (a.id != b.id || a.name != b.name || a.timelineFps != b.timelineFps) return false;
    if (!(a.canvas == b.canvas) || a.colorSpace != b.colorSpace) return false;
    if (a.tracks.size() != b.tracks.size()) return false;
    for (std::size_t t = 0; t < a.tracks.size(); ++t) {
        const Track& lhs = a.tracks[t];
        const Track& rhs = b.tracks[t];
        if (lhs.id != rhs.id || lhs.kind != rhs.kind || lhs.muted != rhs.muted) return false;
        if (lhs.clips.size() != rhs.clips.size()) return false;
        for (std::size_t c = 0; c < lhs.clips.size(); ++c) {
            if (!clipsEqual(lhs.clips[c], rhs.clips[c])) return false;
        }
    }
    return a.assets.size() == b.assets.size();
}

// ---------------------------------------------------------------------------
// Generators
// ---------------------------------------------------------------------------

/// Frame rates across the documented 1-120 fps range, integer and NTSC-style
/// rational (whose nanosecond frame interval does not divide a second evenly, so
/// index-based position arithmetic is genuinely exercised).
[[nodiscard]] FrameRate genFrameRate() {
    const int pick = *rc::gen::inRange(0, 10);
    switch (pick) {
        case 0: return FrameRate(1, 1);
        case 1: return FrameRate(12, 1);
        case 2: return FrameRate::fps23_976();
        case 3: return FrameRate::fps24();
        case 4: return FrameRate::fps25();
        case 5: return FrameRate::fps29_97();
        case 6: return FrameRate::fps30();
        case 7: return FrameRate::fps50();
        case 8: return FrameRate::fps59_94();
        default: return FrameRate(120, 1);
    }
}

/// Small canvases keep 100+ generated cases fast; the compositor's behaviour at
/// large canvases is covered by the compositor's own suites.
[[nodiscard]] Resolution genCanvas() {
    const std::uint32_t w = static_cast<std::uint32_t>(*rc::gen::inRange(1, 9)) * 4u;
    const std::uint32_t h = static_cast<std::uint32_t>(*rc::gen::inRange(1, 9)) * 4u;
    return Resolution{w, h};
}

/// A timeline duration in [`minMs`, `maxMs`] milliseconds.
[[nodiscard]] Duration genDuration(int minMs, int maxMs) {
    return Duration::fromMilliseconds(*rc::gen::inRange(minMs, maxMs + 1));
}

// ---------------------------------------------------------------------------
// The driver
// ---------------------------------------------------------------------------

/// Per-frame composite latency: a host that keeps up, stalling periodically.
/// `baseTicks` is applied to every composite; every `stallPeriod`-th composite
/// additionally takes a full extra interval, which is what makes the transport
/// drop exactly one frame there.
struct LatencyProfile {
    Duration     base{Duration::zero()};
    Duration     stall{Duration::zero()};
    std::int64_t stallPeriod{0}; ///< 0 = never stall.

    [[nodiscard]] Duration forCall(std::uint64_t callIndex) const {
        Duration latency = base;
        if (stallPeriod > 0 && callIndex > 0 &&
            (callIndex % static_cast<std::uint64_t>(stallPeriod)) == 0) {
            latency += stall;
        }
        return latency;
    }
};

/// Wire the recording sinks and the latency-injecting render seam onto
/// `controller`. The render seam advances the SAME injected clock the controller
/// paces against, which is precisely how a slow composite presents itself.
void instrument(PreviewController& controller, gpu::Compositor& compositor, ManualClock& clock,
                RunRecord& record, LatencyProfile latency) {
    controller.setRenderFn([&compositor, &clock, &record, latency](
                               RenderPath, const Project& project, Duration position,
                               const gpu::RenderTarget& target) -> Result<gpu::RenderedFrame> {
        const std::uint64_t call = record.renderCalls++;
        clock.advance(latency.forCall(call));
        return compositor.renderAt(project, position, target);
    });

    controller.setFrameSink([&clock, &record](const gpu::RenderedFrame& frame, RenderPath) {
        record.frames.push_back(PresentedFrame{clock.now(), frame.presentationTime(),
                                               frame.width(), frame.height()});
    });

    controller.setPlayheadIndicator([&clock, &record](Duration displayed) {
        IndicatorUpdate update;
        update.wall = clock.now();
        update.displayed = displayed;
        if (!record.frames.empty()) {
            update.presentedFramePosition = record.frames.back().position;
            update.hadPresentedFrame = true;
        }
        record.indicator.push_back(update);
    });
}

/// A frame provider yielding a solid frame at the canvas size, so every position
/// covered by a clip composites successfully.
[[nodiscard]] gpu::ClipFrameProvider solidProvider(Resolution canvas) {
    return [canvas](const Clip&, Duration) -> Result<gpu::SourceFrame> {
        return gpu::SourceFrame::solid(canvas.width, canvas.height,
                                       gpu::RgbaColor{16, 32, 64, 255});
    };
}

/// The event loop that stands in for the Qt timer driving `pump()`.
///
/// The clock is advanced ONLY when a pump found nothing due — i.e. only for the
/// time the player spends idle waiting for the next frame's deadline. That is the
/// physical model: real time advances monotonically, and a composite CONSUMES part
/// of the interval it runs in rather than adding to it. (Advancing the clock
/// unconditionally per pump would double-count the composite latency, and no
/// engine could then hold Requirement 5.2's rate: the harness itself would be
/// stretching wall time by the composite cost of every frame.)
///
/// A composite that outruns its interval therefore eats the idle time and pushes
/// the following frame's slot into the past, which is exactly what the drop
/// accounting is defined against.
constexpr Duration idleQuantum(Duration interval) {
    return Duration::fromNanoseconds(interval.ticks() / 8);
}

/// Run the loop until playback halts. `maxIterations` bounds it so a regression
/// fails an assertion instead of spinning.
std::size_t driveUntilHalted(PreviewController& controller, ManualClock& clock, Duration interval,
                             std::size_t maxIterations) {
    const Duration quantum = idleQuantum(interval);
    std::size_t iterations = 0;
    while (controller.isPlaying() && iterations < maxIterations) {
        ++iterations;
        if (controller.pump() == 0) {
            clock.advance(quantum.isPositive() ? quantum : interval);
        }
    }
    return iterations;
}

/// Run the loop until `targetFrames` frames have been presented (or playback
/// halts), so a test can stop at a generated prefix of a run.
void drivePrefixFrames(PreviewController& controller, ManualClock& clock, Duration interval,
                       const RunRecord& record, std::size_t targetFrames,
                       std::size_t maxIterations) {
    const Duration quantum = idleQuantum(interval);
    std::size_t iterations = 0;
    while (controller.isPlaying() && record.frames.size() < targetFrames &&
           iterations < maxIterations) {
        ++iterations;
        if (controller.pump() == 0) {
            clock.advance(quantum.isPositive() ? quantum : interval);
        }
    }
}

// ---------------------------------------------------------------------------
// Rolling-window counting
// ---------------------------------------------------------------------------

/// Number of stamps in the half-open window [from, from + 1s). `stamps` must be
/// non-decreasing.
[[nodiscard]] std::size_t countInWindow(const std::vector<Duration>& stamps, Duration from) {
    const Duration to = from + kOneSecond;
    const auto begin = std::lower_bound(stamps.begin(), stamps.end(), from);
    const auto end = std::lower_bound(stamps.begin(), stamps.end(), to);
    return static_cast<std::size_t>(end - begin);
}

/// The end of the measurable period: one frame interval past the last stamp, i.e.
/// the instant at which the next frame would have been due. Windows are bounded by
/// this rather than by the total elapsed clock, because a window extending past
/// the end of playback is not a window "of playback": the run has halted, so it
/// necessarily contains fewer frames and says nothing about the presentation rate.
[[nodiscard]] Duration measurableEnd(const std::vector<Duration>& stamps, Duration step) {
    if (stamps.empty()) return Duration::zero();
    return stamps.back() + step;
}

/// Every rolling 1-second window that fits entirely inside [0, `end`], sampled on
/// a grid of `step` (the frame interval) plus one window anchored at each stamp.
/// Anchoring on both a uniform grid and the stamps themselves catches windows that
/// are tight from either end.
[[nodiscard]] std::vector<Duration> windowAnchors(const std::vector<Duration>& stamps,
                                                  Duration end, Duration step) {
    std::vector<Duration> anchors;
    if (end < kOneSecond) return anchors;
    const Duration last = end - kOneSecond;
    if (step.isPositive()) {
        for (Duration t = Duration::zero(); t <= last; t += step) {
            anchors.push_back(t);
        }
    }
    for (Duration stamp : stamps) {
        if (stamp <= last) anchors.push_back(stamp);
    }
    return anchors;
}

[[nodiscard]] std::vector<Duration> presentationStamps(const RunRecord& record) {
    std::vector<Duration> stamps;
    stamps.reserve(record.frames.size());
    for (const PresentedFrame& frame : record.frames) stamps.push_back(frame.wall);
    return stamps;
}

[[nodiscard]] std::vector<Duration> indicatorStamps(const RunRecord& record) {
    std::vector<Duration> stamps;
    stamps.reserve(record.indicator.size());
    for (const IndicatorUpdate& update : record.indicator) stamps.push_back(update.wall);
    return stamps;
}

// ---------------------------------------------------------------------------
// A composed playback fixture: software GPU context + compositor + controller.
// ---------------------------------------------------------------------------

/// Owns the whole playback stack for one generated case. Held by value in the
/// property bodies; the controller keeps references into it, so it must not move
/// after construction (hence the deleted move/copy).
class PlaybackFixture {
public:
    PlaybackFixture(const Project& project, Resolution canvas)
        : context_(gpu::GpuContext::softwareFallback()),
          compositor_(context_),
          project_(project),
          controller_(compositor_, context_, [this]() { return project_; }, clock_) {
        compositor_.setFrameProvider(solidProvider(canvas));
    }

    PlaybackFixture(const PlaybackFixture&) = delete;
    PlaybackFixture& operator=(const PlaybackFixture&) = delete;

    [[nodiscard]] PreviewController& controller() noexcept { return controller_; }
    [[nodiscard]] gpu::Compositor& compositor() noexcept { return compositor_; }
    [[nodiscard]] ManualClock& clock() noexcept { return clock_; }
    [[nodiscard]] const Project& project() const noexcept { return project_; }

private:
    gpu::GpuContext   context_;
    gpu::Compositor   compositor_;
    Project           project_;
    ManualClock       clock_{};
    PreviewController controller_;
};

} // namespace

// ===========================================================================
// Property 21
// ===========================================================================

// Feature: end-to-end-editor-integration, Property 21: Presentation rate stays
// within bounds and drops stay under 5% — for any project frame rate in 1-120 fps
// and any timeline duration, playing under a controlled clock presents frames at
// the canvas resolution with each rolling 1-second window's presented count in
// [min(fps, 24), fps + 1], and the total dropped count no more than 5 percent of
// the planned frames.
// Validates: Requirements 5.2
RC_GTEST_PROP(PreviewPlaybackProperties, PresentationRateWithinBoundsAndDropsUnderFivePercent, ()) {
    const FrameRate fps = genFrameRate();
    const Resolution canvas = genCanvas();
    // At least 1.1 s so at least one complete rolling 1-second window exists (the
    // window clause is vacuous for a shorter run); capped so 100+ cases stay fast.
    const Duration total = genDuration(1100, 2400);
    const int extraTracks = *rc::gen::inRange(0, 3);

    const MediaAssetRef asset = makeAssetRef("/synthetic/rate-asset.mp4");
    const Project project = makeTimeline(fps, canvas, total, extraTracks, asset);

    PlaybackFixture fixture(project, canvas);
    PreviewController& controller = fixture.controller();

    const FrameRate effective = controller.previewFrameRate();
    const Duration interval = effective.frameDuration();
    RC_ASSERT(interval.isPositive());

    // A host that composites inside one frame interval, stalling for one extra
    // interval every `stallPeriod` frames (see the file header for why the stall
    // is periodic rather than random).
    LatencyProfile latency;
    latency.base = Duration::fromNanoseconds(
        (interval.ticks() * *rc::gen::inRange(0, 90)) / 100); // 0 .. 0.9 interval
    latency.stall = interval;
    // Every 30th-60th composite stalls for one extra interval, which drops exactly
    // one frame there: at most one drop inside any rolling second even at 24 fps,
    // and a drop ratio of at most 1/30 = 3.3 % over the run — both inside
    // Requirement 5.2's allowance by construction rather than by luck.
    latency.stallPeriod = *rc::gen::inRange(30, 61);

    RunRecord record;
    instrument(controller, fixture.compositor(), fixture.clock(), record, latency);

    const std::size_t planned = media::ExportEngine::plannedFrameCount(project, effective);
    RC_ASSERT(planned > 0);

    controller.play();
    driveUntilHalted(controller, fixture.clock(), interval, (planned + 200) * 16);
    RC_ASSERT(!controller.isPlaying());

    // Every presented frame is at the project canvas resolution.
    RC_ASSERT(!record.frames.empty());
    for (const PresentedFrame& frame : record.frames) {
        RC_ASSERT(frame.width == canvas.width);
        RC_ASSERT(frame.height == canvas.height);
    }

    // Rolling-window presentation rate. See the file header for the two documented
    // readings: `fps` is the effective preview rate, and the lower bound is the
    // requirement's floor net of the 5 % drop allowance the same sentence grants.
    const double effFps = effective.toDouble();
    const double lower = std::min(effFps, 24.0) * 0.95;
    const double upper = std::ceil(effFps) + 1.0;

    const std::vector<Duration> stamps = presentationStamps(record);
    const std::vector<Duration> anchors =
        windowAnchors(stamps, measurableEnd(stamps, interval), interval);
    RC_ASSERT(!anchors.empty());
    for (Duration anchor : anchors) {
        const double count = static_cast<double>(countInWindow(stamps, anchor));
        RC_ASSERT(count >= lower);
        RC_ASSERT(count <= upper);
    }

    // Drops over the whole run: no more than 5 percent of the planned frames.
    const double dropped = static_cast<double>(controller.droppedFrameCount());
    RC_ASSERT(dropped <= 0.05 * static_cast<double>(planned));
}

// ===========================================================================
// Property 22
// ===========================================================================

// Feature: end-to-end-editor-integration, Property 22: Playhead indicator cadence
// — for any timeline and any playback run, the playhead indicator is updated at
// least 10 times per elapsed second and every displayed position is within 100
// milliseconds of the position of the frame presented at that moment.
// Validates: Requirements 5.3
RC_GTEST_PROP(PreviewPlaybackProperties, PlayheadIndicatorCadenceAndFreshness, ()) {
    const FrameRate fps = genFrameRate();
    const Resolution canvas = genCanvas();
    const Duration total = genDuration(1100, 2400);
    const int extraTracks = *rc::gen::inRange(0, 3);

    const MediaAssetRef asset = makeAssetRef("/synthetic/indicator-asset.mp4");
    const Project project = makeTimeline(fps, canvas, total, extraTracks, asset);

    PlaybackFixture fixture(project, canvas);
    PreviewController& controller = fixture.controller();

    const FrameRate effective = controller.previewFrameRate();
    const Duration interval = effective.frameDuration();

    LatencyProfile latency;
    latency.base = Duration::fromNanoseconds(
        (interval.ticks() * *rc::gen::inRange(0, 90)) / 100);
    latency.stall = interval;
    // Every 30th-60th composite stalls for one extra interval, which drops exactly
    // one frame there: at most one drop inside any rolling second even at 24 fps,
    // and a drop ratio of at most 1/30 = 3.3 % over the run — both inside
    // Requirement 5.2's allowance by construction rather than by luck.
    latency.stallPeriod = *rc::gen::inRange(30, 61);

    RunRecord record;
    instrument(controller, fixture.compositor(), fixture.clock(), record, latency);

    const std::size_t planned = media::ExportEngine::plannedFrameCount(project, effective);
    controller.play();
    driveUntilHalted(controller, fixture.clock(), interval, (planned + 200) * 16);

    // At least 10 updates in every complete rolling 1-second window.
    const std::vector<Duration> stamps = indicatorStamps(record);
    const std::vector<Duration> anchors =
        windowAnchors(stamps, measurableEnd(stamps, interval), interval);
    RC_ASSERT(!anchors.empty());
    for (Duration anchor : anchors) {
        RC_ASSERT(countInWindow(stamps, anchor) >= 10u);
    }

    // Every displayed position is within 100 ms of the position of the frame
    // presented at that moment.
    const Duration freshness = Duration::fromMilliseconds(100);
    for (const IndicatorUpdate& update : record.indicator) {
        RC_ASSERT(update.hadPresentedFrame);
        RC_ASSERT((update.displayed - update.presentedFramePosition).abs() <= freshness);
    }

    // Requirement 5.3's other clause: each presented frame's timeline position is
    // strictly greater than the previous one.
    for (std::size_t i = 1; i < record.frames.size(); ++i) {
        RC_ASSERT(record.frames[i].position > record.frames[i - 1].position);
    }
}

// ===========================================================================
// Property 23
// ===========================================================================

// Feature: end-to-end-editor-integration, Property 23: A decode failure pauses
// and retains the last good frame — for any timeline and any frame index at which
// the provider fails, playback stops advancing within 100 milliseconds, the
// preview retains the last successfully presented frame, the state is reported
// paused, the project is byte-identical, and the error names the asset whose
// decode failed.
// Validates: Requirements 5.5
RC_GTEST_PROP(PreviewPlaybackProperties, DecodeFailurePausesAndRetainsLastGoodFrame, ()) {
    const FrameRate fps = genFrameRate();
    const Resolution canvas = genCanvas();
    const Duration total = genDuration(200, 1200);
    const int extraTracks = *rc::gen::inRange(0, 2);

    const std::string assetPath = "/synthetic/failing-asset-" +
                                  std::to_string(*rc::gen::inRange(1, 1000)) + ".mp4";
    const MediaAssetRef asset = makeAssetRef(assetPath);
    const Project project = makeTimeline(fps, canvas, total, extraTracks, asset);

    PlaybackFixture fixture(project, canvas);
    PreviewController& controller = fixture.controller();

    const FrameRate effective = controller.previewFrameRate();
    const Duration interval = effective.frameDuration();
    const std::size_t planned = media::ExportEngine::plannedFrameCount(project, effective);

    // The frame index whose decode fails, including the very first frame. The
    // provider is asked once per composited layer, so the failure is keyed on the
    // number of DISTINCT frames composited so far, tracked through the render seam.
    const std::size_t failAt = static_cast<std::size_t>(
        *rc::gen::inRange<std::size_t>(0, planned));

    const ErrorCode code = *rc::gen::element(ErrorCode::Io, ErrorCode::NotFound,
                                             ErrorCode::Unsupported,
                                             ErrorCode::FailedPrecondition);

    // A provider that fails for every position at or after the failing frame's
    // position, naming the asset exactly as media::DecoderClipFrameProvider does.
    const Duration failPosition = effective.durationForFrames(static_cast<std::int64_t>(failAt));
    fixture.compositor().setFrameProvider(
        [canvas, failPosition, code, assetPath](const Clip&,
                                                Duration position) -> Result<gpu::SourceFrame> {
            if (position >= failPosition) {
                return err<gpu::SourceFrame>(makeError(
                    code, "decode failed for asset " + assetPath + ": synthetic failure"));
            }
            return gpu::SourceFrame::solid(canvas.width, canvas.height,
                                           gpu::RgbaColor{8, 16, 24, 255});
        });

    RunRecord record;
    instrument(controller, fixture.compositor(), fixture.clock(), record, LatencyProfile{});

    const Project before = fixture.project();

    controller.play();
    driveUntilHalted(controller, fixture.clock(), interval, (planned + 200) * 16);

    // The state is reported paused — not stopped, not still playing.
    RC_ASSERT(controller.state() == PlaybackState::Paused);
    RC_ASSERT(!controller.isPlaying());

    // The preview retains the last successfully presented frame: exactly the
    // frames before the failing index were presented, and lastFrame() is the last
    // of them (or empty when the very first frame failed).
    RC_ASSERT(record.frames.size() == failAt);
    if (failAt == 0) {
        RC_ASSERT(!controller.lastFrame().has_value());
        RC_ASSERT(controller.playhead() == Duration::zero());
    } else {
        RC_ASSERT(controller.lastFrame().has_value());
        const Duration lastGood =
            effective.durationForFrames(static_cast<std::int64_t>(failAt) - 1);
        RC_ASSERT(controller.lastFrame()->presentationTime == lastGood);
        RC_ASSERT(controller.playhead() == lastGood);
    }

    // Playback stops advancing: with the clock moved on by far more than 100 ms
    // and the pump driven again, nothing moves and nothing new is presented.
    const Duration heldPlayhead = controller.playhead();
    const std::size_t presentedAtFailure = record.frames.size();
    for (int i = 0; i < 4; ++i) {
        fixture.clock().advance(Duration::fromMilliseconds(100));
        RC_ASSERT(controller.pump() == 0u);
    }
    RC_ASSERT(controller.playhead() == heldPlayhead);
    RC_ASSERT(record.frames.size() == presentedAtFailure);

    // The error names the asset whose decode failed.
    RC_ASSERT(controller.lastError().has_value());
    RC_ASSERT(controller.lastError()->message().find(assetPath) != std::string::npos);
    RC_ASSERT(controller.playbackNotice().find(assetPath) != std::string::npos);

    // The project is unchanged.
    RC_ASSERT(projectsEqual(fixture.project(), before));
}

// ===========================================================================
// Property 24
// ===========================================================================

// Feature: end-to-end-editor-integration, Property 24: Playback frame accounting
// matches the export planner — for all timelines, playing from position zero
// until playback halts yields a presented frame count plus a reported dropped
// frame count equal to `ExportEngine::plannedFrameCount` for the same timeline
// and frame rate, with strictly increasing presentation timestamps across
// presented frames; and for a timeline whose planned count is zero, no frames are
// presented and the state remains reported stopped.
// Validates: Requirements 5.7
RC_GTEST_PROP(PreviewPlaybackProperties, FrameAccountingMatchesTheExportPlanner, ()) {
    const FrameRate fps = genFrameRate();
    const Resolution canvas = genCanvas();
    // Includes single-tick and single-frame timelines (1 ms is below one frame
    // interval at every generated rate) as well as multi-frame ones.
    const Duration total = genDuration(1, 1500);
    const int extraTracks = *rc::gen::inRange(0, 8);

    const MediaAssetRef asset = makeAssetRef("/synthetic/accounting-asset.mp4");
    const Project project = makeTimeline(fps, canvas, total, extraTracks, asset);

    PlaybackFixture fixture(project, canvas);
    PreviewController& controller = fixture.controller();

    const FrameRate effective = controller.previewFrameRate();
    const Duration interval = effective.frameDuration();

    // Optional stalls, so the accounting identity is asserted both with and
    // without drops in the run.
    LatencyProfile latency;
    if (*rc::gen::arbitrary<bool>()) {
        latency.base = Duration::fromNanoseconds((interval.ticks() * 30) / 100);
        latency.stall = interval * 2;
        latency.stallPeriod = *rc::gen::inRange(3, 20);
    }

    RunRecord record;
    instrument(controller, fixture.compositor(), fixture.clock(), record, latency);

    const std::size_t planned = media::ExportEngine::plannedFrameCount(project, effective);

    // The zero-planned antecedent is UNREACHABLE for the preview transport, and
    // that is itself an invariant worth asserting: the effective preview rate is
    // always valid (>= 24 fps), and plannedFrameCount is only zero for an invalid
    // rate. The "no frames presented, state stopped" consequent is asserted for
    // the reachable analogue below (a halted transport).
    RC_ASSERT(effective.isValid());
    RC_ASSERT(planned > 0);

    // A halted transport presents nothing however far the clock moves.
    RC_ASSERT(controller.state() == PlaybackState::Stopped);
    fixture.clock().advance(kOneSecond);
    RC_ASSERT(controller.pump() == 0u);
    RC_ASSERT(record.frames.empty());

    controller.play();
    driveUntilHalted(controller, fixture.clock(), interval, (planned + 500) * 16);

    // Playback halted at the end of the timeline, on its own.
    RC_ASSERT(!controller.isPlaying());
    RC_ASSERT(controller.state() == PlaybackState::Stopped);
    RC_ASSERT(controller.reachedEndOfTimeline());

    // Presented + dropped == the export planner's frame count for the same
    // timeline and frame rate.
    RC_ASSERT(controller.presentedFrameCount() + controller.droppedFrameCount() ==
              static_cast<std::uint64_t>(planned));
    RC_ASSERT(record.frames.size() == controller.presentedFrameCount());

    // Strictly increasing presentation timestamps across the presented frames.
    for (std::size_t i = 1; i < record.frames.size(); ++i) {
        RC_ASSERT(record.frames[i].position > record.frames[i - 1].position);
    }
    // Every presented position lies inside the timeline, and playback rests on the
    // timeline duration.
    for (const PresentedFrame& frame : record.frames) {
        RC_ASSERT(!frame.position.isNegative());
        RC_ASSERT(frame.position < total || frame.position == Duration::zero());
    }
    RC_ASSERT(controller.playhead() == total);
}

// ===========================================================================
// Property 79
// ===========================================================================

// Feature: end-to-end-editor-integration, Property 79: Each transport command
// reaches its specified resting state — for any timeline and any prefix of
// playback, issuing pause, stop, or playing on to the timeline duration halts
// playhead advance within 100 ms and reaches that command's specified resting
// state: pause retains the last presented frame's position and reports paused;
// stop sets the playhead to zero and presents the frame for zero within 500 ms
// and reports stopped; reaching the duration retains the last presented frame and
// reports stopped.
// Validates: Requirements 5.4, 5.8, 5.10
RC_GTEST_PROP(PreviewPlaybackProperties, EachTransportCommandReachesItsRestingState, ()) {
    enum class Command { Pause, Stop, PlayToEnd };

    const FrameRate fps = genFrameRate();
    const Resolution canvas = genCanvas();
    const Duration total = genDuration(50, 1500);
    const int extraTracks = *rc::gen::inRange(0, 8);
    const Command command =
        *rc::gen::element(Command::Pause, Command::Stop, Command::PlayToEnd);

    const MediaAssetRef asset = makeAssetRef("/synthetic/transport-asset.mp4");
    const Project project = makeTimeline(fps, canvas, total, extraTracks, asset);

    PlaybackFixture fixture(project, canvas);
    PreviewController& controller = fixture.controller();

    const FrameRate effective = controller.previewFrameRate();
    const Duration interval = effective.frameDuration();
    const std::size_t planned = media::ExportEngine::plannedFrameCount(project, effective);

    RunRecord record;
    instrument(controller, fixture.compositor(), fixture.clock(), record, LatencyProfile{});

    // Any prefix of playback, including the empty prefix (the command is issued
    // before a single frame has been presented).
    const std::size_t prefix = *rc::gen::inRange<std::size_t>(0, planned + 1);

    controller.play();
    drivePrefixFrames(controller, fixture.clock(), interval, record, prefix,
                      (planned + 200) * 16);

    const std::size_t framesBeforeCommand = record.frames.size();
    const std::optional<Duration> lastPresented =
        framesBeforeCommand == 0 ? std::optional<Duration>{}
                                 : std::optional<Duration>{record.frames.back().position};

    switch (command) {
        case Command::Pause: {
            if (!controller.isPlaying()) {
                // The prefix already ran to the end of the timeline; that case is
                // the PlayToEnd branch, asserted below.
                RC_ASSERT(controller.state() == PlaybackState::Stopped);
                RC_ASSERT(controller.playhead() == total);
                break;
            }
            controller.pause();
            // Reports paused, and the playhead is the last presented frame's
            // position (Requirement 5.4).
            RC_ASSERT(controller.state() == PlaybackState::Paused);
            if (lastPresented.has_value()) {
                RC_ASSERT(controller.playhead() == *lastPresented);
                // That frame is presented again, so the surface shows the paused
                // position (the 500 ms bound is met inside the call itself).
                RC_ASSERT(controller.lastFrame().has_value());
                RC_ASSERT(controller.lastFrame()->presentationTime == *lastPresented);
                RC_ASSERT(record.frames.back().position == *lastPresented);
            }
            break;
        }
        case Command::Stop: {
            controller.stop();
            // Reports stopped, the playhead is timeline position zero, and the
            // frame for zero is presented (Requirement 5.8).
            RC_ASSERT(controller.state() == PlaybackState::Stopped);
            RC_ASSERT(controller.playhead() == Duration::zero());
            RC_ASSERT(controller.lastFrame().has_value());
            RC_ASSERT(controller.lastFrame()->presentationTime == Duration::zero());
            RC_ASSERT(!record.frames.empty());
            RC_ASSERT(record.frames.back().position == Duration::zero());
            break;
        }
        case Command::PlayToEnd: {
            driveUntilHalted(controller, fixture.clock(), interval, (planned + 500) * 16);
            // Reports stopped, the last presented frame is retained, and the
            // playhead rests on the timeline duration (Requirement 5.10).
            RC_ASSERT(controller.state() == PlaybackState::Stopped);
            RC_ASSERT(controller.reachedEndOfTimeline());
            RC_ASSERT(controller.playhead() == total);
            RC_ASSERT(controller.lastFrame().has_value());
            RC_ASSERT(!record.frames.empty());
            RC_ASSERT(controller.lastFrame()->presentationTime == record.frames.back().position);
            RC_ASSERT(controller.lastFrame()->presentationTime < total);
            break;
        }
    }

    // Whatever the command, the playhead stops advancing: with the clock moved on
    // by far more than 100 ms and the pump driven repeatedly, it does not move.
    const Duration resting = controller.playhead();
    for (int i = 0; i < 4; ++i) {
        fixture.clock().advance(Duration::fromMilliseconds(100));
        RC_ASSERT(controller.pump() == 0u);
        RC_ASSERT(controller.playhead() == resting);
    }
}

// ===========================================================================
// Property 25
// ===========================================================================

// Feature: end-to-end-editor-integration, Property 25: Seek clamps to the
// timeline bounds — for any timeline, any requested position (negative, interior,
// or beyond the end) and any playing state, seeking presents the frame for
// clamp(requested, 0, duration) within 500 milliseconds and reports that clamped
// position as the current playhead.
// Validates: Requirements 5.9
RC_GTEST_PROP(PreviewPlaybackProperties, SeekClampsToTheTimelineBounds, ()) {
    const FrameRate fps = genFrameRate();
    const Resolution canvas = genCanvas();
    const Duration total = genDuration(50, 1500);
    const int extraTracks = *rc::gen::inRange(0, 3);
    const bool playing = *rc::gen::arbitrary<bool>();

    const MediaAssetRef asset = makeAssetRef("/synthetic/seek-asset.mp4");
    const Project project = makeTimeline(fps, canvas, total, extraTracks, asset);

    PlaybackFixture fixture(project, canvas);
    PreviewController& controller = fixture.controller();
    const Duration interval = controller.previewFrameRate().frameDuration();

    RunRecord record;
    instrument(controller, fixture.compositor(), fixture.clock(), record, LatencyProfile{});

    // Requested positions spanning +/- 2x the duration: negative, interior, and
    // beyond the end, plus the exact boundaries.
    const std::int64_t span = total.ticks() * 2;
    const Duration requested = Duration::fromNanoseconds(
        *rc::gen::inRange<std::int64_t>(-span, span + 1));

    if (playing) {
        controller.play();
        // Present a frame or two first, so the seek interrupts a live run. A very
        // short timeline can reach its end inside that prefix, which is the
        // "halted when the seek was requested" half of the property.
        drivePrefixFrames(controller, fixture.clock(), interval, record, 2, 256);
    }

    const Duration expected = requested.isNegative()
                                  ? Duration::zero()
                                  : (requested > total ? total : requested);
    const PlaybackState stateBeforeSeek = controller.state();

    controller.seek(requested);

    // The clamped position is reported as the current playhead...
    RC_ASSERT(controller.playhead() == expected);
    // ...and the frame for it has been presented (inside the seek call, so the
    // 500 ms bound is met by construction).
    RC_ASSERT(controller.lastFrame().has_value());
    RC_ASSERT(controller.lastFrame()->presentationTime == expected);
    RC_ASSERT(!record.frames.empty());
    RC_ASSERT(record.frames.back().position == expected);

    // Seeking does not change the transport state — it clamps and presents
    // whether playback was running or halted when the seek was requested.
    RC_ASSERT(controller.state() == stateBeforeSeek);

    // The clamp is idempotent: seeking to the clamped position again is a no-op,
    // and seeking past both bounds lands on them exactly.
    controller.seek(expected);
    RC_ASSERT(controller.playhead() == expected);
    controller.seek(Duration::fromNanoseconds(-span - 1));
    RC_ASSERT(controller.playhead() == Duration::zero());
    controller.seek(total + kOneSecond);
    RC_ASSERT(controller.playhead() == total);
}

} // namespace palmier::ui
