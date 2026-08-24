// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/gpu_compositor_test.cpp — unit tests for the compositing render graph
// (task 7.3, Requirements 10.2 and 10.7).
//
// The Compositor's compositing math is the vendor-neutral, host-memory software
// reference (the same path property P5 validates the GPU against), so these
// tests exercise the real logic directly with no Vulkan loader or GPU: they
// verify visible-clip gathering, z-ordering (painter's order), opacity-weighted
// alpha compositing, the per-clip effect hook, the cleared-canvas base case, and
// the documented error paths. Source frames are supplied through an injected
// ClipFrameProvider (the seam the future MediaDecoder fills).

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "core/Duration.hpp"
#include "core/Effect.hpp"
#include "core/Project.hpp"
#include "core/Result.hpp"
#include "core/Uuid.hpp"
#include "gpu/Compositor.hpp"
#include "gpu/GpuContext.hpp"

namespace palmier::gpu {
namespace {

// --- Fixture helpers --------------------------------------------------------

/// A clip covering the whole timeline region [0, 10s), with the given opacity
/// and effects. The asset ref is left nil — the compositor does not validate the
/// project (that is ProjectValidation's job); it only reads geometry/opacity.
Clip makeClip(double opacity = 1.0, std::vector<Effect> effects = {}) {
    Clip c;
    c.id = Uuid::generateV4();
    c.timelineStart = Duration::zero();
    c.sourceIn = Duration::zero();
    c.sourceOut = Duration::fromSeconds(10.0);
    c.opacity = opacity;
    c.effects = std::move(effects);
    return c;
}

Track makeVideoTrack(std::vector<Clip> clips, bool muted = false) {
    Track t;
    t.id = Uuid::generateV4();
    t.kind = TrackKind::Video;
    t.muted = muted;
    t.clips = std::move(clips);
    return t;
}

Track makeAudioTrack(std::vector<Clip> clips) {
    Track t;
    t.id = Uuid::generateV4();
    t.kind = TrackKind::Audio;
    t.clips = std::move(clips);
    return t;
}

Project makeProject(std::vector<Track> tracks) {
    Project p;
    p.id = Uuid::generateV4();
    p.name = "test";
    p.timelineFps = FrameRate::fps30();
    p.canvas = Resolution::hd1080();
    p.tracks = std::move(tracks);
    return p;
}

/// A provider that returns a solid-color frame of `w`x`h` for every clip.
ClipFrameProvider solidProvider(std::uint32_t w, std::uint32_t h, RgbaColor color) {
    return [w, h, color](const Clip&, Duration) -> Result<SourceFrame> {
        return SourceFrame::solid(w, h, color);
    };
}

/// Read the RGBA of the first pixel of a rendered frame.
RgbaColor firstPixel(const RenderedFrame& frame) {
    const auto* px = static_cast<const std::uint8_t*>(frame.hostData());
    return RgbaColor{px[0], px[1], px[2], px[3]};
}

constexpr Duration kAt = Duration::fromSeconds(1.0); // inside [0, 10s)

// --- Visible-clip gathering & z-order --------------------------------------

TEST(CompositorGather, PicksCoveringVideoClipsWithTrackIndexAsZ) {
    Project p = makeProject({
        makeVideoTrack({makeClip()}), // z = 0
        makeVideoTrack({makeClip()}), // z = 1
    });

    auto layers = Compositor::gatherVisibleClips(p, kAt);
    ASSERT_EQ(layers.size(), 2u);
    EXPECT_EQ(layers[0].z, 0u);
    EXPECT_EQ(layers[1].z, 1u);
    EXPECT_LT(layers[0].z, layers[1].z); // ascending (bottom track first)
}

TEST(CompositorGather, SkipsMutedAndAudioTracksAndNonCoveringClips) {
    // A clip that does NOT cover kAt (starts at 5s).
    Clip late = makeClip();
    late.timelineStart = Duration::fromSeconds(5.0);

    Project p = makeProject({
        makeVideoTrack({makeClip()}),          // z=0: covers -> included
        makeVideoTrack({makeClip()}, /*muted=*/true), // z=1: muted -> skipped
        makeAudioTrack({makeClip()}),          // z=2: audio -> skipped
        makeVideoTrack({late}),                // z=3: not covering kAt -> skipped
    });

    auto layers = Compositor::gatherVisibleClips(p, kAt);
    ASSERT_EQ(layers.size(), 1u);
    EXPECT_EQ(layers[0].z, 0u);
}

TEST(CompositorGather, EmptyWhenNoClipCoversPosition) {
    Project p = makeProject({makeVideoTrack({makeClip()})});
    // 20s is past the clip's [0,10s) span.
    auto layers = Compositor::gatherVisibleClips(p, Duration::fromSeconds(20.0));
    EXPECT_TRUE(layers.empty());
}

// --- Painter's order --------------------------------------------------------

TEST(CompositorRender, TopOpaqueLayerOccludesLowerLayers) {
    // Bottom track red, top track green, both fully opaque.
    auto ctx = GpuContext::softwareFallback();
    Compositor comp(ctx);

    const RgbaColor red{255, 0, 0, 255};
    const RgbaColor green{0, 255, 0, 255};

    Project p = makeProject({
        makeVideoTrack({makeClip()}), // z=0 bottom
        makeVideoTrack({makeClip()}), // z=1 top
    });

    // Provider returns green for track-1 clips and red for track-0 clips by
    // alternating on call order is fragile; instead colorize by z via a counter.
    // Simpler: give every clip green EXCEPT flag the bottom red using opacity of
    // frames — here we distinguish by returning red first (bottom drawn first).
    int call = 0;
    comp.setFrameProvider([&](const Clip&, Duration) -> Result<SourceFrame> {
        // gatherVisibleClips sorts ascending by z, and renderAt calls the
        // provider in that order: first the bottom (red), then the top (green).
        return SourceFrame::solid(4, 4, call++ == 0 ? red : green);
    });

    RenderTarget target(4, 4, RgbaColor::opaqueBlack());
    auto rf = comp.renderAt(p, kAt, target);
    ASSERT_TRUE(rf.isOk());
    EXPECT_EQ(rf.value().layerCount(), 2u);
    // Top opaque green fully occludes the bottom red.
    EXPECT_EQ(firstPixel(rf.value()), green);
}

// --- Opacity-weighted alpha compositing ------------------------------------

TEST(CompositorRender, OpacityBlendsTopOverBottom) {
    auto ctx = GpuContext::softwareFallback();
    Compositor comp(ctx);

    const RgbaColor bottomColor{0, 0, 200, 255}; // opaque blue base
    const RgbaColor topColor{200, 0, 0, 255};    // red, drawn at 50% opacity

    // Bottom fully opaque, top at 0.5 opacity.
    Project p = makeProject({
        makeVideoTrack({makeClip(/*opacity=*/1.0)}), // z=0 bottom
        makeVideoTrack({makeClip(/*opacity=*/0.5)}), // z=1 top
    });

    int call = 0;
    comp.setFrameProvider([&](const Clip&, Duration) -> Result<SourceFrame> {
        return SourceFrame::solid(2, 2, call++ == 0 ? bottomColor : topColor);
    });

    auto rf = comp.renderAt(p, kAt, RenderTarget(2, 2));
    ASSERT_TRUE(rf.isOk());
    const RgbaColor out = firstPixel(rf.value());

    // out = top*0.5 + bottom*0.5  (bottom is fully opaque after being drawn).
    // R: 200*0.5 + 0*0.5   = 100
    // B:   0*0.5 + 200*0.5 = 100
    EXPECT_NEAR(out.r, 100, 1);
    EXPECT_EQ(out.g, 0);
    EXPECT_NEAR(out.b, 100, 1);
    EXPECT_EQ(out.a, 255);
}

// --- Cleared-canvas base case (empty timeline position) --------------------

TEST(CompositorRender, NoVisibleClipsYieldsClearedCanvasWithoutProvider) {
    auto ctx = GpuContext::softwareFallback();
    Compositor comp(ctx); // no frame provider installed

    Project p = makeProject({makeVideoTrack({makeClip()})});
    const RgbaColor clear{10, 20, 30, 255};

    // Position past all clips: nothing visible, so the provider is never needed.
    auto rf = comp.renderAt(p, Duration::fromSeconds(50.0), RenderTarget(3, 3, clear));
    ASSERT_TRUE(rf.isOk());
    EXPECT_EQ(rf.value().layerCount(), 0u);
    EXPECT_EQ(firstPixel(rf.value()), clear);
}

// --- Per-clip effect hook ---------------------------------------------------

TEST(CompositorRender, AppliesPerClipBrightnessEffect) {
    auto ctx = GpuContext::softwareFallback();
    Compositor comp(ctx);

    // A single opaque gray clip with a +0.2 brightness effect (~ +51 per channel).
    Clip c = makeClip(1.0, {Effect::brightness(0.2)});
    Project p = makeProject({makeVideoTrack({c})});

    const RgbaColor gray{100, 100, 100, 255};
    comp.setFrameProvider(solidProvider(2, 2, gray));

    auto rf = comp.renderAt(p, kAt, RenderTarget(2, 2, RgbaColor::opaqueBlack()));
    ASSERT_TRUE(rf.isOk());
    const RgbaColor out = firstPixel(rf.value());
    EXPECT_NEAR(out.r, 151, 1); // 100 + 0.2*255
    EXPECT_NEAR(out.g, 151, 1);
    EXPECT_NEAR(out.b, 151, 1);
}

TEST(CompositorRender, InjectedSoftwareEffectFnOverridesDefault) {
    auto ctx = GpuContext::softwareFallback();
    Compositor comp(ctx);

    // Override the effect hook to blank every channel to zero regardless of kind.
    comp.setSoftwareEffectFn([](const Effect&, std::uint8_t* rgba,
                                std::uint32_t w, std::uint32_t h) noexcept {
        const std::size_t n = static_cast<std::size_t>(w) * h * 4u;
        for (std::size_t i = 0; i < n; ++i) rgba[i] = (i % 4 == 3) ? 255 : 0;
    });

    Clip c = makeClip(1.0, {Effect::brightness(0.5)});
    Project p = makeProject({makeVideoTrack({c})});
    comp.setFrameProvider(solidProvider(2, 2, RgbaColor{123, 45, 67, 255}));

    auto rf = comp.renderAt(p, kAt, RenderTarget(2, 2, RgbaColor::opaqueBlack()));
    ASSERT_TRUE(rf.isOk());
    EXPECT_EQ(firstPixel(rf.value()), (RgbaColor{0, 0, 0, 255}));
}

// --- Effect registration ----------------------------------------------------

TEST(CompositorRegister, RegisterEffectTracksKernels) {
    auto ctx = GpuContext::softwareFallback();
    Compositor comp(ctx);

    EXPECT_FALSE(comp.isEffectRegistered(EffectType::Blur));
    EXPECT_EQ(comp.registeredEffectCount(), 0u);

    SpirvModule mod;
    mod.code = {0x07230203u, 0x0u}; // non-empty stand-in bytecode
    comp.registerEffect(EffectType::Blur, mod);

    EXPECT_TRUE(comp.isEffectRegistered(EffectType::Blur));
    EXPECT_EQ(comp.registeredEffectCount(), 1u);

    // Re-registering the same id replaces rather than duplicates.
    comp.registerEffect(EffectType::Blur, mod);
    EXPECT_EQ(comp.registeredEffectCount(), 1u);
}

// --- Error paths ------------------------------------------------------------

TEST(CompositorRender, RejectsDegenerateTarget) {
    auto ctx = GpuContext::softwareFallback();
    Compositor comp(ctx);
    Project p = makeProject({makeVideoTrack({makeClip()})});

    auto rf = comp.renderAt(p, kAt, RenderTarget(0, 0));
    ASSERT_TRUE(rf.isError());
    EXPECT_EQ(rf.error().code(), ErrorCode::InvalidArgument);
}

TEST(CompositorRender, RejectsNonRgba8Target) {
    auto ctx = GpuContext::softwareFallback();
    Compositor comp(ctx);
    Project p = makeProject({makeVideoTrack({makeClip()})});

    RenderTarget target(4, 4);
    target.format = FrameFormat::RGBA16F;
    auto rf = comp.renderAt(p, kAt, target);
    ASSERT_TRUE(rf.isError());
    EXPECT_EQ(rf.error().code(), ErrorCode::Unsupported);
}

TEST(CompositorRender, FailsWhenVisibleClipsButNoProvider) {
    auto ctx = GpuContext::softwareFallback();
    Compositor comp(ctx); // no provider
    Project p = makeProject({makeVideoTrack({makeClip()})});

    auto rf = comp.renderAt(p, kAt, RenderTarget(4, 4));
    ASSERT_TRUE(rf.isError());
    EXPECT_EQ(rf.error().code(), ErrorCode::FailedPrecondition);
}

TEST(CompositorRender, PropagatesProviderErrorWithoutLeakingFrames) {
    auto ctx = GpuContext::softwareFallback();
    Compositor comp(ctx);
    Project p = makeProject({makeVideoTrack({makeClip()})});

    comp.setFrameProvider([](const Clip&, Duration) -> Result<SourceFrame> {
        return err<SourceFrame>(makeError(ErrorCode::Io, "decode failed"));
    });

    auto rf = comp.renderAt(p, kAt, RenderTarget(4, 4));
    ASSERT_TRUE(rf.isError());
    EXPECT_EQ(rf.error().code(), ErrorCode::Io);

    // The acquired output frame must have been returned to the pool (no leak).
    EXPECT_EQ(ctx.framePool().stats().inUseBytes, 0u);
    EXPECT_EQ(ctx.framePool().stats().framesInUse, 0u);
}

TEST(CompositorRender, RenderedFrameHoldsFrameUntilDestroyed) {
    auto ctx = GpuContext::softwareFallback();
    Compositor comp(ctx);
    Project p = makeProject({makeVideoTrack({makeClip()})});
    comp.setFrameProvider(solidProvider(4, 4, RgbaColor{1, 2, 3, 255}));

    {
        auto rf = comp.renderAt(p, kAt, RenderTarget(4, 4));
        ASSERT_TRUE(rf.isOk());
        EXPECT_GT(ctx.framePool().stats().inUseBytes, 0u); // lease held by RenderedFrame
    }
    // RenderedFrame destroyed -> frame returned to the pool.
    EXPECT_EQ(ctx.framePool().stats().inUseBytes, 0u);
}

// --- Captions (usable-editor task 13; Requirement 10.3's burn-in mode) -----

Track makeCaptionTrack(std::vector<Clip> clips, bool muted = false) {
    Track t;
    t.id = Uuid::generateV4();
    t.kind = TrackKind::Caption;
    t.muted = muted;
    t.clips = std::move(clips);
    return t;
}

/// A caption cue covering [0, 10s), mirroring makeClip's own geometry.
Clip makeCaptionCue(std::string text = "Hello") {
    Clip c;
    c.id = Uuid::generateV4();
    c.timelineStart = Duration::zero();
    c.sourceIn = Duration::zero();
    c.sourceOut = Duration::fromSeconds(10.0);
    c.captionText = std::move(text);
    return c;
}

TEST(CompositorGather, GatherVisibleCaptionCuesPicksCoveringCuesWithTrackIndexAsZ) {
    Project p = makeProject({
        makeVideoTrack({makeClip()}),           // z=0: video, not a caption
        makeCaptionTrack({makeCaptionCue()}),   // z=1: covers -> included
    });

    auto layers = Compositor::gatherVisibleCaptionCues(p, kAt);
    ASSERT_EQ(layers.size(), 1u);
    EXPECT_EQ(layers[0].z, 1u);
    EXPECT_TRUE(layers[0].clip->isCaptionCue());
}

TEST(CompositorGather, GatherVisibleCaptionCuesSkipsMutedTracksAndNonCoveringCues) {
    Clip late = makeCaptionCue();
    late.timelineStart = Duration::fromSeconds(5.0);

    Project p = makeProject({
        makeCaptionTrack({makeCaptionCue()}),         // z=0: covers -> included
        makeCaptionTrack({makeCaptionCue()}, true),   // z=1: muted -> skipped
        makeCaptionTrack({late}),                     // z=2: not covering kAt -> skipped
    });

    auto layers = Compositor::gatherVisibleCaptionCues(p, kAt);
    ASSERT_EQ(layers.size(), 1u);
    EXPECT_EQ(layers[0].z, 0u);
}

TEST(CompositorGather, GatherVisibleCaptionCuesEmptyWhenNoCueCoversPosition) {
    Project p = makeProject({makeCaptionTrack({makeCaptionCue()})});
    auto layers = Compositor::gatherVisibleCaptionCues(p, Duration::fromSeconds(20.0));
    EXPECT_TRUE(layers.empty());
}

TEST(CompositorRender, RendersACaptionCueThroughTheTextRasterizerSeam) {
    auto ctx = GpuContext::softwareFallback();
    Compositor comp(ctx);
    Project p = makeProject({makeCaptionTrack({makeCaptionCue("Burn me in")})});

    // A fake rasterizer that returns a distinctive color and records the
    // TextStyle it was asked to render — proving renderAt synthesized a style
    // from captionText rather than requiring one already on the clip.
    TextStyle observedStyle;
    bool rasterizerCalled = false;
    comp.setTextRasterizer(
        [&](const TextStyle& style, std::uint32_t w, std::uint32_t h) -> Result<SourceFrame> {
            observedStyle = style;
            rasterizerCalled = true;
            return SourceFrame::solid(w, h, RgbaColor{9, 9, 9, 255});
        });

    RenderTarget target(4, 4, RgbaColor::opaqueBlack());
    auto rf = comp.renderAt(p, kAt, target);
    ASSERT_TRUE(rf.isOk());
    EXPECT_TRUE(rasterizerCalled);
    EXPECT_EQ(observedStyle.content, "Burn me in");
    const RgbaColor expected{9, 9, 9, 255};
    EXPECT_EQ(firstPixel(rf.value()), expected);
}

TEST(CompositorRender, FailsWhenVisibleCaptionCueButNoTextRasterizer) {
    auto ctx = GpuContext::softwareFallback();
    Compositor comp(ctx); // no rasterizer installed
    Project p = makeProject({makeCaptionTrack({makeCaptionCue()})});

    auto rf = comp.renderAt(p, kAt, RenderTarget(4, 4));
    ASSERT_TRUE(rf.isError());
    EXPECT_EQ(rf.error().code(), ErrorCode::FailedPrecondition);
}

} // namespace
} // namespace palmier::gpu
