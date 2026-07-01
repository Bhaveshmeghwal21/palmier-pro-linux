// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/ui/preview_controller_test.cpp — unit tests for the Qt-free
// preview/player playback engine (task 19.3; Requirements 2.8, 10.7).
//
// The PreviewController owns the playhead clock, drives the Compositor at a
// minimum of 24 fps, and selects the GPU-active compositing path with a CPU
// fallback. These tests exercise all of that headlessly — no Qt, no Vulkan, no
// GPU — by injecting a deterministic manual clock, driving the real
// vendor-neutral software Compositor (through both the software-fallback and a
// synthetic GPU-active GpuContext built from an injected device enumerator), and
// using the RenderFn seam to simulate a GPU render failure so the CPU-fallback
// path can be asserted.

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "core/Duration.hpp"
#include "core/FrameRate.hpp"
#include "core/Project.hpp"
#include "core/Result.hpp"
#include "core/Uuid.hpp"
#include "gpu/Compositor.hpp"
#include "gpu/GpuContext.hpp"
#include "gpu/GpuTypes.hpp"
#include "ui/PreviewController.hpp"

namespace palmier::ui {
namespace {

// --- Test doubles -----------------------------------------------------------

/// A deterministic, manually-advanced monotonic clock.
class ManualClock final : public PlaybackClock {
public:
    [[nodiscard]] Duration now() const override { return now_; }
    void set(Duration t) noexcept { now_ = t; }
    void advance(Duration d) noexcept { now_ += d; }

private:
    Duration now_{Duration::zero()};
};

// --- Project / device builders ---------------------------------------------

Project makeProject(FrameRate fps, std::vector<Track> tracks = {}) {
    Project p;
    p.id = Uuid::generateV4();
    p.name = "preview-test";
    p.timelineFps = fps;
    p.canvas = Resolution(8, 8); // tiny canvas keeps frame memory trivial
    p.tracks = std::move(tracks);
    return p;
}

Clip makeClip(Duration start, Duration length) {
    Clip c;
    c.id = Uuid::generateV4();
    c.timelineStart = start;
    c.sourceIn = Duration::zero();
    c.sourceOut = length;
    c.opacity = 1.0;
    return c;
}

Track makeVideoTrack(std::vector<Clip> clips) {
    Track t;
    t.id = Uuid::generateV4();
    t.kind = TrackKind::Video;
    t.clips = std::move(clips);
    return t;
}

gpu::GpuDeviceInfo discreteComputeDevice() {
    gpu::GpuDeviceInfo d;
    d.index = 0;
    d.name = "NVIDIA RTX (synthetic)";
    d.vendor = gpu::GpuVendor::NVIDIA;
    d.type = gpu::GpuDeviceType::DiscreteGpu;
    d.caps.vendorId = gpu::GpuVendor::NVIDIA;
    d.caps.vendor = "NVIDIA";
    d.caps.supportsCompute = true;
    d.caps.hwDecode = true;
    d.caps.hwEncode = true;
    d.caps.vramBytes = 2ull * 1024 * 1024 * 1024;
    return d;
}

/// A GpuContext that reports a real, compute-capable device (GPU-active).
gpu::GpuContext makeGpuActiveContext() {
    const std::vector<gpu::GpuDeviceInfo> devices{discreteComputeDevice()};
    gpu::PhysicalDeviceEnumerator enumr = [devices]() { return devices; };
    auto ctx = gpu::GpuContext::createWith(gpu::GpuSelectionPolicy::automatic(), enumr,
                                           /*store=*/nullptr);
    return std::move(ctx).value();
}

gpu::ClipFrameProvider solidProvider(gpu::RgbaColor color) {
    return [color](const Clip&, Duration) -> Result<gpu::SourceFrame> {
        return gpu::SourceFrame::solid(8, 8, color);
    };
}

PreviewProjectSource fixedProject(Project project) {
    return [project = std::move(project)]() { return project; };
}

constexpr Duration kOneSecond = Duration::fromSeconds(1.0);

} // namespace

// --- Preview frame rate: floored at 24 fps (Requirement 2.8) ---------------

TEST(PreviewControllerRate, FloorsSlowTimelineTo24Fps) {
    auto ctx = gpu::GpuContext::softwareFallback();
    gpu::Compositor comp(ctx);
    ManualClock clock;
    PreviewController controller(comp, ctx, fixedProject(makeProject(FrameRate(12, 1))), clock);

    EXPECT_GE(controller.previewFps(), 24.0);
    EXPECT_EQ(controller.previewFrameRate(), FrameRate::fps24());
}

TEST(PreviewControllerRate, KeepsFastTimelineRate) {
    auto ctx = gpu::GpuContext::softwareFallback();
    gpu::Compositor comp(ctx);
    ManualClock clock;
    PreviewController controller(comp, ctx, fixedProject(makeProject(FrameRate::fps30())), clock);

    EXPECT_DOUBLE_EQ(controller.previewFps(), 30.0);
    EXPECT_EQ(controller.previewFrameRate(), FrameRate::fps30());
}

TEST(PreviewControllerRate, NtscBelow24IsFlooredTo24) {
    auto ctx = gpu::GpuContext::softwareFallback();
    gpu::Compositor comp(ctx);
    ManualClock clock;
    // 23.976 fps < 24 -> preview cadence floored to 24 (the minimum guarantee).
    PreviewController controller(comp, ctx, fixedProject(makeProject(FrameRate::fps23_976())),
                                 clock);
    EXPECT_GE(controller.previewFps(), 24.0);
    EXPECT_EQ(controller.previewFrameRate(), FrameRate::fps24());
}

TEST(PreviewControllerRate, OverrideTakesPrecedence) {
    auto ctx = gpu::GpuContext::softwareFallback();
    gpu::Compositor comp(ctx);
    ManualClock clock;
    PreviewOptions opts;
    opts.targetFpsOverride = FrameRate::fps60();
    PreviewController controller(comp, ctx, fixedProject(makeProject(FrameRate::fps30())), clock,
                                 opts);
    EXPECT_DOUBLE_EQ(controller.previewFps(), 60.0);
}

// --- Render-path selection (Requirement 10.7) ------------------------------

TEST(PreviewControllerPath, SoftwareContextSelectsCpuFallback) {
    auto ctx = gpu::GpuContext::softwareFallback();
    gpu::Compositor comp(ctx);
    ManualClock clock;
    PreviewController controller(comp, ctx, fixedProject(makeProject(FrameRate::fps30())), clock);

    EXPECT_EQ(controller.preferredPath(), RenderPath::CpuFallback);
    EXPECT_FALSE(controller.isGpuActive());
}

TEST(PreviewControllerPath, ComputeCapableContextSelectsGpuActive) {
    auto ctx = makeGpuActiveContext();
    ASSERT_FALSE(ctx.isSoftwareFallback());
    ASSERT_TRUE(ctx.capabilities().supportsCompute);

    gpu::Compositor comp(ctx);
    ManualClock clock;
    PreviewController controller(comp, ctx, fixedProject(makeProject(FrameRate::fps30())), clock);

    EXPECT_EQ(controller.preferredPath(), RenderPath::GpuActive);
    EXPECT_TRUE(controller.isGpuActive());

    // A rendered frame reports the GPU-active path.
    auto frame = controller.renderFrame();
    ASSERT_TRUE(frame.isOk());
    EXPECT_EQ(frame.value().path, RenderPath::GpuActive);
}

TEST(PreviewControllerPath, DegradesToCpuWhenGpuRenderFails) {
    auto ctx = makeGpuActiveContext();
    gpu::Compositor comp(ctx);
    ManualClock clock;
    PreviewController controller(comp, ctx, fixedProject(makeProject(FrameRate::fps30())), clock);
    ASSERT_EQ(controller.preferredPath(), RenderPath::GpuActive);

    // The GPU attempt fails; the CPU attempt (delegating to the real software
    // compositor) succeeds. The controller must retry on CPU and stay there.
    controller.setRenderFn([&comp](RenderPath path, const Project& p, Duration d,
                                   const gpu::RenderTarget& t) -> Result<gpu::RenderedFrame> {
        if (path == RenderPath::GpuActive) {
            return err<gpu::RenderedFrame>(makeError(ErrorCode::Internal, "synthetic GPU failure"));
        }
        return comp.renderAt(p, d, t);
    });

    auto frame = controller.renderFrame();
    ASSERT_TRUE(frame.isOk());
    EXPECT_EQ(frame.value().path, RenderPath::CpuFallback);
    EXPECT_TRUE(controller.degradedToCpu());
    EXPECT_EQ(controller.activePath(), RenderPath::CpuFallback);
    EXPECT_FALSE(controller.isGpuActive());
}

// --- Cadence: >= 24 fps over a wall-second (Requirement 2.8) ----------------

TEST(PreviewControllerPlayback, RendersAtLeast24FramesPerWallSecond) {
    auto ctx = gpu::GpuContext::softwareFallback();
    gpu::Compositor comp(ctx);
    ManualClock clock;
    // Empty timeline (no clips) -> renderAt composites the cleared canvas with no
    // frame provider needed, so we measure the pure presentation cadence.
    PreviewController controller(comp, ctx, fixedProject(makeProject(FrameRate::fps30())), clock);

    controller.play();
    ASSERT_TRUE(controller.isPlaying());

    clock.advance(kOneSecond);
    const std::size_t rendered = controller.pump();

    EXPECT_GE(rendered, 24u); // the minimum-24-fps guarantee
    ASSERT_TRUE(controller.lastFrame().has_value());
    EXPECT_EQ(controller.lastFrame()->layerCount, 0u); // cleared canvas
    // The playhead advanced by roughly one wall-second of frames.
    EXPECT_GE(controller.playhead().seconds(), 0.9);
}

TEST(PreviewControllerPlayback, PumpReturnsZeroWhenNotPlaying) {
    auto ctx = gpu::GpuContext::softwareFallback();
    gpu::Compositor comp(ctx);
    ManualClock clock;
    PreviewController controller(comp, ctx, fixedProject(makeProject(FrameRate::fps30())), clock);

    clock.advance(kOneSecond);
    EXPECT_EQ(controller.pump(), 0u);
    EXPECT_EQ(controller.state(), PlaybackState::Stopped);
}

// --- Actual compositing through the pump (with a visible clip) --------------

TEST(PreviewControllerPlayback, PumpCompositesVisibleClipThroughSink) {
    auto ctx = gpu::GpuContext::softwareFallback();
    gpu::Compositor comp(ctx);
    comp.setFrameProvider(solidProvider(gpu::RgbaColor{10, 20, 30, 255}));

    // A clip covering [0, 2s) so the whole first wall-second is inside it.
    Project project =
        makeProject(FrameRate::fps30(),
                    {makeVideoTrack({makeClip(Duration::zero(), Duration::fromSeconds(2.0))})});

    ManualClock clock;
    PreviewController controller(comp, ctx, fixedProject(project), clock);

    std::size_t sinkCalls = 0;
    controller.setFrameSink([&sinkCalls](const gpu::RenderedFrame& frame, RenderPath) {
        EXPECT_EQ(frame.layerCount(), 1u); // the one visible clip
        ++sinkCalls;
    });

    controller.play();
    clock.advance(Duration::fromMilliseconds(500)); // half a second at 30 fps -> ~15 frames
    const std::size_t rendered = controller.pump();

    EXPECT_GT(rendered, 0u);
    EXPECT_EQ(sinkCalls, rendered);
    ASSERT_TRUE(controller.lastFrame().has_value());
    EXPECT_EQ(controller.lastFrame()->layerCount, 1u);
    EXPECT_EQ(controller.lastFrame()->width, 8u);
    EXPECT_EQ(controller.lastFrame()->height, 8u);
}

// --- Transport ordering -----------------------------------------------------

TEST(PreviewControllerTransport, PauseHoldsPlayheadStopResets) {
    auto ctx = gpu::GpuContext::softwareFallback();
    gpu::Compositor comp(ctx);
    ManualClock clock;
    PreviewController controller(comp, ctx, fixedProject(makeProject(FrameRate::fps30())), clock);

    controller.play();
    clock.advance(Duration::fromMilliseconds(200));
    controller.pump();
    const Duration held = controller.playhead();
    EXPECT_GT(held.seconds(), 0.0);

    controller.pause();
    EXPECT_EQ(controller.state(), PlaybackState::Paused);

    // While paused, further clock time and pumps do not advance the playhead.
    clock.advance(kOneSecond);
    EXPECT_EQ(controller.pump(), 0u);
    EXPECT_EQ(controller.playhead(), held);

    controller.stop();
    EXPECT_EQ(controller.state(), PlaybackState::Stopped);
    EXPECT_EQ(controller.playhead(), Duration::zero());
}

TEST(PreviewControllerTransport, SeekClampsNegativeToZero) {
    auto ctx = gpu::GpuContext::softwareFallback();
    gpu::Compositor comp(ctx);
    ManualClock clock;
    PreviewController controller(comp, ctx, fixedProject(makeProject(FrameRate::fps30())), clock);

    controller.seek(Duration::fromSeconds(3.0));
    EXPECT_DOUBLE_EQ(controller.playhead().seconds(), 3.0);

    controller.seek(Duration::fromSeconds(-1.0));
    EXPECT_EQ(controller.playhead(), Duration::zero());
}

TEST(PreviewControllerTransport, AutoStopsAtEndOfBoundedTimeline) {
    auto ctx = gpu::GpuContext::softwareFallback();
    gpu::Compositor comp(ctx);
    comp.setFrameProvider(solidProvider(gpu::RgbaColor{1, 2, 3, 255}));

    // A short 100 ms clip; playback should auto-stop at 100 ms.
    const Duration length = Duration::fromMilliseconds(100);
    Project project =
        makeProject(FrameRate::fps30(), {makeVideoTrack({makeClip(Duration::zero(), length)})});

    ManualClock clock;
    PreviewController controller(comp, ctx, fixedProject(project), clock);

    controller.play();
    clock.advance(kOneSecond); // far past the clip end
    controller.pump();

    EXPECT_EQ(controller.state(), PlaybackState::Stopped);
    EXPECT_EQ(controller.playhead(), length); // clamped to the timeline end
}

} // namespace palmier::ui
