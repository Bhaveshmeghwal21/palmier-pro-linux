// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/gpu_layer_placement_test.cpp — the per-frame placement of a layer on the canvas.
//
// Two behaviours the render path previously lacked, each of which had been *documented* as
// working while nothing implemented it:
//
//   * Transitions. Clip::transitionIn was stored, validated, persisted, round-tripped and
//     drawn in the timeline panel, but appeared in zero lines of the render path — so all
//     five kinds rendered as hard cuts. Measured before the fix: across a 1.0s crossfade
//     between an inverted clip and a normal one, mean luma went 159.9 -> 95.6 in a single
//     frame with no intermediate value anywhere. A `fade` on a lone clip, which needs no
//     neighbour at all and so cannot be dismissed as misuse, held a flat ~90 from its first
//     frame instead of rising from black.
//
//   * Fitting. blendOver aligned a source to the canvas's top-left and clipped it, so a
//     3840x2160 source on a 1920x1080 canvas showed only its top-left quarter. Both
//     blendOver and applyCropTransform carried a comment saying the *other* was responsible
//     for scaling, so neither did it.
//
// The arithmetic tests below pin the maths directly; the two renderAt tests are the ones that
// fail against the previous implementation.

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/Duration.hpp"
#include "core/Project.hpp"
#include "core/Result.hpp"
#include "core/Transition.hpp"
#include "core/Uuid.hpp"
#include "gpu/Compositor.hpp"
#include "gpu/GpuContext.hpp"
#include "gpu/LayerPlacement.hpp"

namespace palmier::gpu {
namespace {

/// A clip spanning [start, start+length) with an optional incoming transition.
Clip makeClip(double startSeconds, double lengthSeconds,
              std::optional<Transition> transition = std::nullopt) {
    Clip c;
    c.id = Uuid::generateV4();
    c.timelineStart = Duration::fromSeconds(startSeconds);
    c.sourceIn = Duration::zero();
    c.sourceOut = Duration::fromSeconds(lengthSeconds);
    c.transitionIn = std::move(transition);
    return c;
}

Transition makeTransition(TransitionKind kind, double durationSeconds) {
    return Transition{Uuid::generateV4(), kind, Duration::fromSeconds(durationSeconds)};
}

Track makeVideoTrack(std::vector<Clip> clips) {
    Track t;
    t.id = Uuid::generateV4();
    t.kind = TrackKind::Video;
    t.clips = std::move(clips);
    return t;
}

Project makeProject(std::vector<Track> tracks) {
    Project p;
    p.id = Uuid::generateV4();
    p.name = "placement";
    p.timelineFps = FrameRate::fps30();
    p.canvas = Resolution::hd1080();
    p.tracks = std::move(tracks);
    return p;
}

RgbaColor pixelAt(const RenderedFrame& f, std::uint32_t x, std::uint32_t y) {
    const auto* p = static_cast<const std::uint8_t*>(f.hostData());
    const std::size_t i = (static_cast<std::size_t>(y) * f.width() + x) * 4u;
    return RgbaColor{p[i], p[i + 1], p[i + 2], p[i + 3]};
}

// --- fitToCanvas ------------------------------------------------------------

TEST(FitToCanvas, AMatchingSourceIsTheIdentity) {
    const FittedRect r = fitToCanvas(1920, 1080, 1920, 1080);
    EXPECT_EQ(r.x, 0);
    EXPECT_EQ(r.y, 0);
    EXPECT_EQ(r.width, 1920u);
    EXPECT_EQ(r.height, 1080u);
}

TEST(FitToCanvas, FourKFillsAnHdCanvasRatherThanCroppingToItsTopLeft) {
    const FittedRect r = fitToCanvas(3840, 2160, 1920, 1080);
    EXPECT_EQ(r.x, 0);
    EXPECT_EQ(r.y, 0);
    EXPECT_EQ(r.width, 1920u);
    EXPECT_EQ(r.height, 1080u);
}

TEST(FitToCanvas, ASquareSourceIsPillarboxedAndCentred) {
    const FittedRect r = fitToCanvas(1000, 1000, 1920, 1080);
    EXPECT_EQ(r.width, 1080u);
    EXPECT_EQ(r.height, 1080u);
    EXPECT_EQ(r.x, (1920 - 1080) / 2);
    EXPECT_EQ(r.y, 0);
}

TEST(FitToCanvas, AWiderSourceIsLetterboxedAndCentred) {
    const FittedRect r = fitToCanvas(1920, 1080, 1000, 1000);
    EXPECT_EQ(r.width, 1000u);
    EXPECT_EQ(r.height, 563u);
    EXPECT_EQ(r.x, 0);
    EXPECT_EQ(r.y, (1000 - 563) / 2);
}

TEST(FitToCanvas, ASmallerSourceIsScaledUpToFillTheCanvas) {
    const FittedRect r = fitToCanvas(640, 360, 1920, 1080);
    EXPECT_EQ(r.width, 1920u);
    EXPECT_EQ(r.height, 1080u);
}

TEST(FitToCanvas, AZeroDimensionYieldsAnEmptyRect) {
    EXPECT_TRUE(fitToCanvas(0, 1080, 1920, 1080).isEmpty());
    EXPECT_TRUE(fitToCanvas(1920, 1080, 1920, 0).isEmpty());
}

// --- transitionProgress -----------------------------------------------------

TEST(TransitionProgress, AClipWithoutATransitionHasNone) {
    const Clip c = makeClip(1.0, 2.0);
    EXPECT_FALSE(transitionProgress(c, Duration::fromSeconds(1.5)).has_value());
}

TEST(TransitionProgress, RisesFromZeroAtTheLeadingEdgeToOneAtTheRegionEnd) {
    const Clip c = makeClip(1.0, 2.0, makeTransition(TransitionKind::Crossfade, 1.0));

    const auto atEdge = transitionProgress(c, Duration::fromSeconds(1.0));
    ASSERT_TRUE(atEdge.has_value());
    EXPECT_NEAR(*atEdge, 0.0, 1e-9);

    const auto atMid = transitionProgress(c, Duration::fromSeconds(1.5));
    ASSERT_TRUE(atMid.has_value());
    EXPECT_NEAR(*atMid, 0.5, 1e-9);
}

TEST(TransitionProgress, IsAbsentOutsideTheRegion) {
    const Clip c = makeClip(1.0, 2.0, makeTransition(TransitionKind::Crossfade, 1.0));
    EXPECT_FALSE(transitionProgress(c, Duration::fromSeconds(0.5)).has_value());
    EXPECT_FALSE(transitionProgress(c, Duration::fromSeconds(2.0)).has_value());
    EXPECT_FALSE(transitionProgress(c, Duration::fromSeconds(2.5)).has_value());
}

TEST(TransitionProgress, AZeroLengthRegionBlendsNothing) {
    const Clip c = makeClip(1.0, 2.0, makeTransition(TransitionKind::Crossfade, 0.0));
    EXPECT_FALSE(transitionProgress(c, Duration::fromSeconds(1.0)).has_value());
}

// --- layerPlacements --------------------------------------------------------

TEST(LayerPlacements, OutsideATransitionEveryLayerCarriesOnlyItsOwnOpacity) {
    Clip a = makeClip(0.0, 2.0);
    a.opacity = 0.25;
    const std::vector<PlacedLayer> layers{{&a, 0}};

    const auto p = layerPlacements(layers, Duration::fromSeconds(1.0));
    ASSERT_EQ(p.size(), 1u);
    EXPECT_NEAR(p[0].opacity, 0.25, 1e-9);
    EXPECT_NEAR(p[0].reveal, 1.0, 1e-9);
    EXPECT_NEAR(p[0].slideX, 0.0, 1e-9);
}

TEST(LayerPlacements, ACrossfadeRampsTheIncomingLayerAcrossTheRegion) {
    const Clip outgoing = makeClip(0.0, 2.0);
    const Clip incoming = makeClip(1.0, 2.0, makeTransition(TransitionKind::Crossfade, 1.0));
    const std::vector<PlacedLayer> layers{{&outgoing, 0}, {&incoming, 0}};

    const auto mid = layerPlacements(layers, Duration::fromSeconds(1.5));
    ASSERT_EQ(mid.size(), 2u);
    EXPECT_NEAR(mid[0].opacity, 1.0, 1e-9);  // outgoing stays fully drawn beneath
    EXPECT_NEAR(mid[1].opacity, 0.5, 1e-9);  // incoming half revealed over it

    const auto quarter = layerPlacements(layers, Duration::fromSeconds(1.25));
    EXPECT_NEAR(quarter[1].opacity, 0.25, 1e-9);
}

TEST(LayerPlacements, ACrossfadeScalesByTheClipsOwnOpacity) {
    const Clip outgoing = makeClip(0.0, 2.0);
    Clip incoming = makeClip(1.0, 2.0, makeTransition(TransitionKind::Crossfade, 1.0));
    incoming.opacity = 0.5;
    const std::vector<PlacedLayer> layers{{&outgoing, 0}, {&incoming, 0}};

    const auto mid = layerPlacements(layers, Duration::fromSeconds(1.5));
    EXPECT_NEAR(mid[1].opacity, 0.25, 1e-9);
}

TEST(LayerPlacements, AFadeOnALoneClipRampsUpFromTheClearedCanvas) {
    // The arrangement that proved the defect: one clip, no neighbour, a fade at its leading
    // edge. Nothing about this needs an outgoing clip, so a flat full-coverage placement here
    // is unambiguously wrong.
    const Clip lone = makeClip(5.0, 2.0, makeTransition(TransitionKind::Fade, 1.0));
    const std::vector<PlacedLayer> layers{{&lone, 0}};

    EXPECT_NEAR(layerPlacements(layers, Duration::fromSeconds(5.0))[0].opacity, 0.0, 1e-9);
    EXPECT_NEAR(layerPlacements(layers, Duration::fromSeconds(5.25))[0].opacity, 0.25, 1e-9);
    EXPECT_NEAR(layerPlacements(layers, Duration::fromSeconds(5.75))[0].opacity, 0.75, 1e-9);
    // Past the region the clip is fully present again.
    EXPECT_NEAR(layerPlacements(layers, Duration::fromSeconds(6.5))[0].opacity, 1.0, 1e-9);
}

TEST(LayerPlacements, ADipToColorPassesThroughTheClearColourAtTheMidpoint) {
    const Clip outgoing = makeClip(0.0, 2.0);
    const Clip incoming = makeClip(1.0, 2.0, makeTransition(TransitionKind::DipToColor, 1.0));
    const std::vector<PlacedLayer> layers{{&outgoing, 0}, {&incoming, 0}};

    // First quarter: the outgoing clip is halfway out, the incoming not yet in.
    const auto early = layerPlacements(layers, Duration::fromSeconds(1.25));
    EXPECT_NEAR(early[0].opacity, 0.5, 1e-9);
    EXPECT_NEAR(early[1].opacity, 0.0, 1e-9);

    // Exactly halfway: neither contributes, so the frame is the clear colour.
    const auto mid = layerPlacements(layers, Duration::fromSeconds(1.5));
    EXPECT_NEAR(mid[0].opacity, 0.0, 1e-9);
    EXPECT_NEAR(mid[1].opacity, 0.0, 1e-9);

    // Last quarter: the incoming clip is halfway in and the outgoing is gone.
    const auto late = layerPlacements(layers, Duration::fromSeconds(1.75));
    EXPECT_NEAR(late[0].opacity, 0.0, 1e-9);
    EXPECT_NEAR(late[1].opacity, 0.5, 1e-9);
}

TEST(LayerPlacements, AWipeRevealsAFractionOfTheWidthRatherThanFading) {
    const Clip outgoing = makeClip(0.0, 2.0);
    const Clip incoming = makeClip(1.0, 2.0, makeTransition(TransitionKind::Wipe, 1.0));
    const std::vector<PlacedLayer> layers{{&outgoing, 0}, {&incoming, 0}};

    const auto mid = layerPlacements(layers, Duration::fromSeconds(1.5));
    EXPECT_NEAR(mid[1].reveal, 0.5, 1e-9);
    EXPECT_NEAR(mid[1].opacity, 1.0, 1e-9);  // a wipe has a hard edge, it does not fade
}

TEST(LayerPlacements, ASlideTravelsInFromTheRightEdge) {
    const Clip outgoing = makeClip(0.0, 2.0);
    const Clip incoming = makeClip(1.0, 2.0, makeTransition(TransitionKind::Slide, 1.0));
    const std::vector<PlacedLayer> layers{{&outgoing, 0}, {&incoming, 0}};

    EXPECT_NEAR(layerPlacements(layers, Duration::fromSeconds(1.0))[1].slideX, 1.0, 1e-9);
    EXPECT_NEAR(layerPlacements(layers, Duration::fromSeconds(1.5))[1].slideX, 0.5, 1e-9);
    EXPECT_NEAR(layerPlacements(layers, Duration::fromSeconds(1.99))[1].slideX, 0.01, 1e-3);
}

TEST(LayerPlacements, ATransitionOnADifferentTrackDoesNotConsumeTheLayerBelow) {
    // Same position, but the two layers are on different tracks, so the lower one is not this
    // transition's outgoing clip and a dip must leave it alone.
    const Clip below = makeClip(0.0, 4.0);
    const Clip above = makeClip(1.0, 2.0, makeTransition(TransitionKind::DipToColor, 1.0));
    const std::vector<PlacedLayer> layers{{&below, 0}, {&above, 1}};

    const auto early = layerPlacements(layers, Duration::fromSeconds(1.25));
    EXPECT_NEAR(early[0].opacity, 1.0, 1e-9);
    EXPECT_NEAR(early[1].opacity, 0.0, 1e-9);
}

// --- Through the real render path -------------------------------------------

TEST(CompositorTransition, ACrossfadeMidpointShowsBothShotsRatherThanAHardCut) {
    auto ctx = GpuContext::softwareFallback();
    Compositor comp(ctx);

    const RgbaColor outgoingColor{200, 0, 0, 255};
    const RgbaColor incomingColor{0, 0, 200, 255};

    // The arrangement the model documents: the incoming clip overlaps the outgoing by exactly
    // its transition duration.
    Project p = makeProject({makeVideoTrack(
        {makeClip(0.0, 2.0), makeClip(1.0, 2.0, makeTransition(TransitionKind::Crossfade, 1.0))})});

    int call = 0;
    comp.setFrameProvider([&](const Clip&, Duration) -> Result<SourceFrame> {
        // Painter's order: the outgoing clip is drawn first.
        return SourceFrame::solid(2, 2, call++ == 0 ? outgoingColor : incomingColor);
    });

    auto rf = comp.renderAt(p, Duration::fromSeconds(1.5), RenderTarget(2, 2));
    ASSERT_TRUE(rf.isOk());
    // Both clips cover the midpoint, so both are gathered.
    EXPECT_EQ(rf.value().layerCount(), 2u);

    const RgbaColor out = pixelAt(rf.value(), 0, 0);
    // Halfway through a crossfade: half of each. Neither shot's own colour is acceptable —
    // that is precisely the hard cut this test exists to reject.
    EXPECT_NEAR(out.r, 100, 2);
    EXPECT_NEAR(out.b, 100, 2);
    EXPECT_EQ(out.a, 255);
}

TEST(CompositorTransition, AFadeOnALoneClipRisesFromTheClearColour) {
    auto ctx = GpuContext::softwareFallback();
    Compositor comp(ctx);

    const RgbaColor clip{200, 200, 200, 255};
    const RgbaColor clear{0, 0, 0, 255};

    Project p = makeProject(
        {makeVideoTrack({makeClip(0.0, 2.0, makeTransition(TransitionKind::Fade, 1.0))})});
    comp.setFrameProvider(
        [&](const Clip&, Duration) -> Result<SourceFrame> { return SourceFrame::solid(2, 2, clip); });

    // A quarter of the way in, the clip contributes a quarter of its brightness.
    auto quarter = comp.renderAt(p, Duration::fromSeconds(0.25), RenderTarget(2, 2, clear));
    ASSERT_TRUE(quarter.isOk());
    EXPECT_NEAR(pixelAt(quarter.value(), 0, 0).r, 50, 2);

    // Past the region it is fully present.
    auto after = comp.renderAt(p, Duration::fromSeconds(1.5), RenderTarget(2, 2, clear));
    ASSERT_TRUE(after.isOk());
    EXPECT_NEAR(pixelAt(after.value(), 0, 0).r, 200, 2);
}

TEST(CompositorFit, AnOversizedSourceContributesItsWholeImageNotItsTopLeftCorner) {
    auto ctx = GpuContext::softwareFallback();
    Compositor comp(ctx);

    // A 4x4 source whose top-left 2x2 quadrant is red and the rest green. Fitted to a 2x2
    // canvas, each destination pixel averages one 2x2 quadrant, so only the top-left
    // destination pixel is red. The previous top-left blit copied the source's first 2x2
    // block, making the entire canvas red — which is what this test rejects.
    Project p = makeProject({makeVideoTrack({makeClip(0.0, 2.0)})});
    comp.setFrameProvider([&](const Clip&, Duration) -> Result<SourceFrame> {
        SourceFrame f;
        f.width = 4;
        f.height = 4;
        f.rgba.resize(4u * 4u * 4u);
        for (std::uint32_t y = 0; y < 4; ++y) {
            for (std::uint32_t x = 0; x < 4; ++x) {
                const bool topLeft = x < 2 && y < 2;
                const std::size_t i = (static_cast<std::size_t>(y) * 4u + x) * 4u;
                f.rgba[i + 0] = topLeft ? 200 : 0;
                f.rgba[i + 1] = topLeft ? 0 : 200;
                f.rgba[i + 2] = 0;
                f.rgba[i + 3] = 255;
            }
        }
        return f;
    });

    auto rf = comp.renderAt(p, Duration::fromSeconds(1.0), RenderTarget(2, 2));
    ASSERT_TRUE(rf.isOk());

    EXPECT_NEAR(pixelAt(rf.value(), 0, 0).r, 200, 2);  // the red quadrant
    EXPECT_NEAR(pixelAt(rf.value(), 1, 0).g, 200, 2);  // green, not red
    EXPECT_NEAR(pixelAt(rf.value(), 0, 1).g, 200, 2);
    EXPECT_NEAR(pixelAt(rf.value(), 1, 1).g, 200, 2);
}

} // namespace
} // namespace palmier::gpu
