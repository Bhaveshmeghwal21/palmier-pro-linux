// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/gpu_effect_kernels_test.cpp — unit tests for the SPIR-V effect kernels
// and the software reference effects they mirror (task 7.4; Requirements 10.2,
// 10.7).
//
// Two layers are exercised here, both without a GPU or Vulkan loader:
//
//   1. The host-memory software effect reference (applyEffectSoftware +
//      applyTransitionSoftware in Compositor.cpp). This is the P5 parity
//      reference the GPU kernels must match, so it is tested directly on small
//      buffers with hand-computed expected values: Brightness, Contrast, Blur
//      (box average, edge-clamped), CropTransform (transparent outside the
//      rect), ColorGrade (gain/lift/saturation), and the two-input transition.
//
//   2. The effect-kernel registry (EffectKernels.hpp): the GLSL sources are
//      present/well-formed, the kernel<->EffectType mappings are correct, and —
//      where shaderc is compiled in — the kernels compile to valid SPIR-V and
//      register with a Compositor. Tests that need shaderc skip cleanly when it
//      is absent, so the suite passes in a minimal build.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "core/Effect.hpp"
#include "core/ToneCurve.hpp"
#include "core/Uuid.hpp"
#include "gpu/Compositor.hpp"
#include "gpu/EffectKernels.hpp"
#include "gpu/GpuContext.hpp"

namespace palmier::gpu {
namespace {

// --- helpers ----------------------------------------------------------------

/// Build an effect of `type` with the given named parameters.
Effect makeEffect(EffectType type, std::map<std::string, double> params) {
    return Effect{Uuid::generateV4(), type, std::move(params)};
}

/// A width*height RGBA8 buffer filled with one color.
std::vector<std::uint8_t> solid(std::uint32_t w, std::uint32_t h,
                                std::uint8_t r, std::uint8_t g,
                                std::uint8_t b, std::uint8_t a) {
    std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 4u);
    for (std::size_t i = 0; i < px.size(); i += 4) {
        px[i + 0] = r;
        px[i + 1] = g;
        px[i + 2] = b;
        px[i + 3] = a;
    }
    return px;
}

// ===========================================================================
// Software reference effects (the P5 reference the SPIR-V kernels mirror)
// ===========================================================================

TEST(SoftwareEffect, BrightnessShiftsRgbAndPreservesAlpha) {
    auto px = solid(2, 2, 100, 100, 100, 200);
    applyEffectSoftware(makeEffect(EffectType::Brightness, {{"amount", 0.2}}),
                        px.data(), 2, 2);
    EXPECT_NEAR(px[0], 151, 1); // 100 + 0.2*255
    EXPECT_NEAR(px[1], 151, 1);
    EXPECT_NEAR(px[2], 151, 1);
    EXPECT_EQ(px[3], 200); // alpha preserved
}

TEST(SoftwareEffect, BrightnessClampsAtChannelBounds) {
    auto px = solid(1, 1, 250, 5, 128, 255);
    applyEffectSoftware(makeEffect(EffectType::Brightness, {{"amount", 0.5}}),
                        px.data(), 1, 1); // +127.5
    EXPECT_EQ(px[0], 255); // 250 + 127.5 clamps high
    EXPECT_NEAR(px[1], 133, 1);
}

TEST(SoftwareEffect, ContrastScalesAboutMidGray) {
    auto px = solid(1, 1, 200, 128, 50, 255);
    applyEffectSoftware(makeEffect(EffectType::Contrast, {{"amount", 1.0}}),
                        px.data(), 1, 1); // factor 2 about 128
    EXPECT_EQ(px[0], 255);         // (200-128)*2+128 = 272 -> clamp 255
    EXPECT_EQ(px[1], 128);         // mid-gray unchanged
    EXPECT_NEAR(px[2], 0, 1);      // (50-128)*2+128 = -28 -> clamp 0
}

TEST(SoftwareEffect, BlurOfUniformImageIsUnchanged) {
    auto px = solid(4, 4, 80, 90, 100, 255);
    const auto before = px;
    applyEffectSoftware(makeEffect(EffectType::Blur, {{"radius", 2.0}}),
                        px.data(), 4, 4);
    EXPECT_EQ(px, before); // averaging a constant leaves it constant
}

TEST(SoftwareEffect, BlurRadiusZeroIsNoOp) {
    auto px = solid(3, 3, 10, 20, 30, 255);
    px[0] = 200; // perturb one pixel
    const auto before = px;
    applyEffectSoftware(makeEffect(EffectType::Blur, {{"radius", 0.0}}),
                        px.data(), 3, 3);
    EXPECT_EQ(px, before);
}

TEST(SoftwareEffect, BlurAveragesEdgeClampedWindow) {
    // 2x1 image: left R=0, right R=200. radius 1 -> each pixel averages both.
    std::vector<std::uint8_t> px = {0, 0, 0, 255, 200, 0, 0, 255};
    applyEffectSoftware(makeEffect(EffectType::Blur, {{"radius", 1.0}}),
                        px.data(), 2, 1);
    EXPECT_EQ(px[0], 100); // (0 + 200) / 2
    EXPECT_EQ(px[4], 100);
    EXPECT_EQ(px[3], 255); // alpha preserved
    EXPECT_EQ(px[7], 255);
}

TEST(SoftwareEffect, CropTransformClearsOutsideRectToTransparent) {
    // 4x4 opaque white; keep the right half (cropLeft = 0.5).
    auto px = solid(4, 4, 255, 255, 255, 255);
    applyEffectSoftware(
        makeEffect(EffectType::CropTransform, {{"cropLeft", 0.5}}), px.data(), 4, 4);

    // Column 0 (x=0) is outside -> transparent black; column 2 is inside -> kept.
    auto pixel = [&](std::uint32_t x, std::uint32_t y) {
        const std::size_t o = (static_cast<std::size_t>(y) * 4 + x) * 4u;
        return std::vector<std::uint8_t>{px[o], px[o + 1], px[o + 2], px[o + 3]};
    };
    EXPECT_EQ(pixel(0, 0), (std::vector<std::uint8_t>{0, 0, 0, 0}));
    EXPECT_EQ(pixel(1, 2), (std::vector<std::uint8_t>{0, 0, 0, 0}));
    EXPECT_EQ(pixel(2, 0), (std::vector<std::uint8_t>{255, 255, 255, 255}));
    EXPECT_EQ(pixel(3, 3), (std::vector<std::uint8_t>{255, 255, 255, 255}));
}

TEST(SoftwareEffect, CropTransformFullRectIsNoOp) {
    auto px = solid(3, 3, 12, 34, 56, 255);
    const auto before = px;
    applyEffectSoftware(makeEffect(EffectType::CropTransform,
                                   {{"cropLeft", 0.0}, {"cropTop", 0.0},
                                    {"cropRight", 1.0}, {"cropBottom", 1.0}}),
                        px.data(), 3, 3);
    EXPECT_EQ(px, before);
}

TEST(SoftwareEffect, ColorGradeAppliesPerChannelGain) {
    auto px = solid(1, 1, 50, 60, 70, 128);
    applyEffectSoftware(makeEffect(EffectType::ColorGrade, {{"gainR", 2.0}}),
                        px.data(), 1, 1);
    EXPECT_EQ(px[0], 100); // 50 * 2
    EXPECT_EQ(px[1], 60);  // unchanged
    EXPECT_EQ(px[2], 70);
    EXPECT_EQ(px[3], 128); // alpha preserved
}

TEST(SoftwareEffect, ColorGradeZeroSaturationYieldsGray) {
    auto px = solid(1, 1, 200, 100, 0, 255);
    applyEffectSoftware(makeEffect(EffectType::ColorGrade, {{"saturation", 0.0}}),
                        px.data(), 1, 1);
    // luma = 0.299*200 + 0.587*100 + 0.114*0 = 59.8 + 58.7 = 118.5 -> ~119
    EXPECT_NEAR(px[0], 119, 1);
    EXPECT_EQ(px[0], px[1]); // gray: all channels equal
    EXPECT_EQ(px[1], px[2]);
}

TEST(SoftwareEffect, ColorGradeLiftAddsOffset) {
    auto px = solid(1, 1, 10, 10, 10, 255);
    applyEffectSoftware(makeEffect(EffectType::ColorGrade, {{"lift", 0.1}}),
                        px.data(), 1, 1); // +25.5
    EXPECT_NEAR(px[0], 36, 1); // 10 + 25.5
}

// --- Lift / gamma / gain primary grade (monitoring-and-grading Requirement 4) ---

namespace {

/// A FROZEN copy of the colour-grade arithmetic exactly as it stood immediately
/// before per-channel lift and gamma were added, including its own copy of
/// Compositor's private toByte() rounding.
///
/// This exists so Requirement 4.2's "renders byte-identically" can be asserted
/// against something. The previous implementation is gone from the tree, so a test
/// comparing the new code to the old one has to carry the old one; comparing the new
/// code to itself would assert nothing at all. Do not "simplify" this to call
/// applyEffectSoftware — that is precisely the tautology it exists to avoid.
[[nodiscard]] std::uint8_t legacyToByte(double v) {
    if (v <= 0.0) return 0;
    if (v >= 255.0) return 255;
    return static_cast<std::uint8_t>(v + 0.5);
}

void legacyColorGrade(std::vector<std::uint8_t>& px, double gainR, double gainG, double gainB,
                      double lift, double saturation) {
    const double liftShift = lift * 255.0;
    if (gainR == 1.0 && gainG == 1.0 && gainB == 1.0 && liftShift == 0.0 && saturation == 1.0) {
        return;
    }
    for (std::size_t o = 0; o < px.size(); o += 4) {
        double r = static_cast<double>(px[o + 0]) * gainR + liftShift;
        double g = static_cast<double>(px[o + 1]) * gainG + liftShift;
        double b = static_cast<double>(px[o + 2]) * gainB + liftShift;
        const double luma = 0.299 * r + 0.587 * g + 0.114 * b;
        r = luma + (r - luma) * saturation;
        g = luma + (g - luma) * saturation;
        b = luma + (b - luma) * saturation;
        px[o + 0] = legacyToByte(r);
        px[o + 1] = legacyToByte(g);
        px[o + 2] = legacyToByte(b);
    }
}

/// An image covering every byte value on every channel, with a varying alpha, so a
/// comparison over it is a comparison over the whole domain rather than a sample.
[[nodiscard]] std::vector<std::uint8_t> fullRangeImage() {
    std::vector<std::uint8_t> px(256u * 4u);
    for (std::size_t i = 0; i < 256; ++i) {
        px[i * 4 + 0] = static_cast<std::uint8_t>(i);
        px[i * 4 + 1] = static_cast<std::uint8_t>(255 - i);
        px[i * 4 + 2] = static_cast<std::uint8_t>((i * 7) % 256);
        px[i * 4 + 3] = static_cast<std::uint8_t>((i * 3) % 256);
    }
    return px;
}

}  // namespace

// Requirement 4.2 and 4.3, the hard criterion: an effect carrying only the legacy
// parameters must render EXACTLY as it did before per-channel lift and gamma existed
// — not within a tolerance, byte for byte — with no migration and no user action.
//
// The legacy parameter set is what a project saved before this change actually
// contains: a scalar `lift`, no `liftR/G/B`, and no gamma at all.
TEST(ColorGradePrimaryGrade, ALegacyEffectRendersByteIdenticallyToThePreviousImplementation) {
    struct Case {
        double gainR, gainG, gainB, lift, saturation;
    };
    // Includes the all-default no-op, one-sided gains, a NEGATIVE lift (the case the
    // gamma step's clamp would silently change if it were not guarded), a lift that
    // pushes past white, and saturation both under and over 1.
    const std::vector<Case> cases = {
        {1.0, 1.0, 1.0, 0.0, 1.0},   {2.0, 1.0, 1.0, 0.0, 1.0},
        {1.0, 0.5, 1.5, 0.0, 1.0},   {1.0, 1.0, 1.0, -0.3, 1.0},
        {1.0, 1.0, 1.0, 0.4, 1.0},   {1.0, 1.0, 1.0, 0.0, 0.0},
        {1.0, 1.0, 1.0, 0.0, 1.8},   {1.7, 0.3, 1.1, -0.25, 0.4},
        {0.0, 0.0, 0.0, 0.6, 1.2},   {1.9, 1.9, 1.9, -0.45, 1.6},
    };

    for (const Case& c : cases) {
        std::vector<std::uint8_t> viaCurrent = fullRangeImage();
        std::vector<std::uint8_t> viaLegacy = viaCurrent;

        applyEffectSoftware(makeEffect(EffectType::ColorGrade,
                                       {{"gainR", c.gainR},
                                        {"gainG", c.gainG},
                                        {"gainB", c.gainB},
                                        {"lift", c.lift},
                                        {"saturation", c.saturation}}),
                            viaCurrent.data(), 256, 1);
        legacyColorGrade(viaLegacy, c.gainR, c.gainG, c.gainB, c.lift, c.saturation);

        ASSERT_EQ(viaCurrent.size(), viaLegacy.size());
        for (std::size_t i = 0; i < viaCurrent.size(); ++i) {
            ASSERT_EQ(viaCurrent[i], viaLegacy[i])
                << "byte " << i << " differs for gain(" << c.gainR << ',' << c.gainG << ','
                << c.gainB << ") lift " << c.lift << " saturation " << c.saturation;
        }
    }
}

// Requirement 4.2's mechanism, asserted directly rather than left implicit. A gamma
// of exactly 1 must SKIP the step, not compute pow(x, 1). The observable difference
// is a negative intermediate: the clamp inside the step would fold it up to 0 before
// the saturation mix reads it, changing the luma and therefore every channel.
TEST(ColorGradePrimaryGrade, AUnityGammaIsSkippedRatherThanComputed) {
    const std::map<std::string, double> legacy = {
        {"lift", -0.3}, {"saturation", 0.25}, {"gainR", 1.0}};
    std::map<std::string, double> explicitUnity = legacy;
    explicitUnity["gammaR"] = 1.0;
    explicitUnity["gammaG"] = 1.0;
    explicitUnity["gammaB"] = 1.0;

    std::vector<std::uint8_t> withoutGamma = fullRangeImage();
    std::vector<std::uint8_t> withUnityGamma = withoutGamma;
    applyEffectSoftware(makeEffect(EffectType::ColorGrade, legacy), withoutGamma.data(), 256, 1);
    applyEffectSoftware(makeEffect(EffectType::ColorGrade, explicitUnity),
                        withUnityGamma.data(), 256, 1);

    EXPECT_EQ(withoutGamma, withUnityGamma)
        << "spelling gamma out as 1.0 must be indistinguishable from omitting it";
}

// Requirement 4.1: each channel's lift is independent. Also pins Requirement 4.2's
// fallback from the other side — naming liftR alone must not disturb G and B, which
// keep taking their value from the legacy scalar.
TEST(ColorGradePrimaryGrade, PerChannelLiftIsIndependentAndFallsBackToTheLegacyScalar) {
    auto px = solid(1, 1, 10, 10, 10, 255);
    applyEffectSoftware(
        makeEffect(EffectType::ColorGrade, {{"liftR", 0.2}, {"lift", 0.1}, {"saturation", 1.0}}),
        px.data(), 1, 1);
    EXPECT_NEAR(px[0], 61, 1);  // 10 + 0.2*255 = 61
    EXPECT_NEAR(px[1], 36, 1);  // 10 + 0.1*255 = 35.5 -> the legacy scalar
    EXPECT_NEAR(px[2], 36, 1);
    EXPECT_EQ(px[3], 255);
}

// Requirement 4.1: gamma above 1 brightens midtones and below 1 darkens them, which
// is the direction a colourist's midtone control moves. Asserted on mid-gray, where
// a gamma change is largest — at 0 and 255 pow() is a fixed point and would show
// nothing whichever direction the convention ran.
TEST(ColorGradePrimaryGrade, GammaAboveOneBrightensMidtonesAndBelowOneDarkensThem) {
    const std::uint8_t mid = 128;

    auto brighter = solid(1, 1, mid, mid, mid, 255);
    applyEffectSoftware(makeEffect(EffectType::ColorGrade,
                                   {{"gammaR", 2.0}, {"gammaG", 2.0}, {"gammaB", 2.0}}),
                        brighter.data(), 1, 1);
    EXPECT_GT(brighter[0], mid) << "gamma 2.0 must brighten, i.e. pow(x, 1/gamma)";

    auto darker = solid(1, 1, mid, mid, mid, 255);
    applyEffectSoftware(makeEffect(EffectType::ColorGrade,
                                   {{"gammaR", 0.5}, {"gammaG", 0.5}, {"gammaB", 0.5}}),
                        darker.data(), 1, 1);
    EXPECT_LT(darker[0], mid) << "gamma 0.5 must darken";

    // pow(0.5, 1/2.0) = 0.7071 -> 180.3; pow(0.5, 2.0) = 0.25 -> 63.75. Computed from
    // the exact input (128/255 = 0.50196) rather than a rounded 0.5.
    const double x = 128.0 / 255.0;
    EXPECT_NEAR(brighter[0], std::pow(x, 0.5) * 255.0, 1.0);
    EXPECT_NEAR(darker[0], std::pow(x, 2.0) * 255.0, 1.0);

    // The endpoints are fixed points of pow, so gamma must leave them alone.
    auto ends = solid(2, 1, 0, 0, 0, 255);
    ends[4] = ends[5] = ends[6] = 255;
    applyEffectSoftware(makeEffect(EffectType::ColorGrade,
                                   {{"gammaR", 2.2}, {"gammaG", 2.2}, {"gammaB", 2.2}}),
                        ends.data(), 2, 1);
    EXPECT_EQ(ends[0], 0);
    EXPECT_EQ(ends[4], 255);
}

// Requirement 4.5: the order is gain, lift, gamma, saturation, and it is fixed
// because it is observable. Gain-then-lift and lift-then-gain differ whenever gain
// is not 1, so this proves the implemented order rather than restating it.
TEST(ColorGradePrimaryGrade, TheDocumentedOrderOfOperationsIsTheImplementedOne) {
    // gain 2 then lift 0.1: 50*2 = 100, +25.5 = 125.5 -> 126.
    // The other order would be (50 + 25.5)*2 = 151.
    auto px = solid(1, 1, 50, 50, 50, 255);
    applyEffectSoftware(makeEffect(EffectType::ColorGrade,
                                   {{"gainR", 2.0}, {"gainG", 2.0}, {"gainB", 2.0}, {"lift", 0.1}}),
                        px.data(), 1, 1);
    EXPECT_NEAR(px[0], 126, 1);
    EXPECT_LT(px[0], 140) << "lift must be applied AFTER gain, not before it";

    // gamma before saturation: at saturation 0 every channel collapses to the luma of
    // the POST-gamma values. Applying gamma after the collapse would give a different
    // gray, because gamma is non-linear and the channels differ before it.
    auto grey = solid(1, 1, 200, 100, 20, 255);
    applyEffectSoftware(makeEffect(EffectType::ColorGrade, {{"gammaR", 2.2},
                                                            {"gammaG", 2.2},
                                                            {"gammaB", 2.2},
                                                            {"saturation", 0.0}}),
                        grey.data(), 1, 1);
    const double gr = std::pow(200.0 / 255.0, 1.0 / 2.2) * 255.0;
    const double gg = std::pow(100.0 / 255.0, 1.0 / 2.2) * 255.0;
    const double gb = std::pow(20.0 / 255.0, 1.0 / 2.2) * 255.0;
    const double expected = 0.299 * gr + 0.587 * gg + 0.114 * gb;
    EXPECT_NEAR(grey[0], expected, 1.0);
    EXPECT_EQ(grey[0], grey[1]);
    EXPECT_EQ(grey[1], grey[2]);
}

// A gamma of zero is a division by zero waiting to happen, and a negative gamma is
// meaningless. Both must produce a rendered image rather than a NaN written into a
// frame, where toByte's comparisons would both be false and the cast would be
// undefined — and where it would then propagate through every later effect in the
// chain and into the encoder.
TEST(ColorGradePrimaryGrade, ADegenerateGammaIsClampedRatherThanDividingByZero) {
    for (const double gamma : {0.0, -1.0, -1000.0}) {
        // Pixel 0 is a mid tone; pixel 1 carries a pure-white green channel, which is
        // a fixed point of pow() for ANY exponent and so is the one value that must
        // survive whatever the clamp floor happens to be.
        auto px = solid(2, 1, 10, 128, 250, 255);
        px[4] = 0;
        px[5] = 255;
        px[6] = 77;
        auto again = px;

        const Effect e = makeEffect(
            EffectType::ColorGrade,
            {{"gammaR", gamma}, {"gammaG", gamma}, {"gammaB", gamma}});
        applyEffectSoftware(e, px.data(), 2, 1);
        applyEffectSoftware(e, again.data(), 2, 1);

        // Deterministic: a NaN is not equal to itself, so any NaN reaching the
        // arithmetic would make these two runs disagree even though nothing else
        // differs between them.
        EXPECT_EQ(px, again) << "gamma " << gamma << " must be deterministic";

        EXPECT_EQ(px[5], 255) << "pure white is a fixed point of pow for any exponent";
        EXPECT_EQ(px[3], 255) << "alpha must survive a degenerate gamma";
        EXPECT_EQ(px[7], 255);
    }
}

// --- Tone curve (monitoring-and-grading Requirement 5) ----------------------
//
// The interpolation and baking are covered exhaustively in core_tone_curve_test.cpp.
// These are about the EFFECT: that the type is wired through the software reference,
// that a curve with no points is a no-op, and that alpha is untouched.

namespace {

/// Parameters holding one channel's points, built through the production name builder.
std::map<std::string, double> curveParams(CurveChannel channel,
                                         const std::vector<CurvePoint>& points) {
    std::map<std::string, double> params;
    for (std::size_t i = 0; i < points.size(); ++i) {
        params.emplace(curvePointParameterName(channel, i, /*isY=*/false), points[i].x);
        params.emplace(curvePointParameterName(channel, i, /*isY=*/true), points[i].y);
    }
    return params;
}

}  // namespace

TEST(SoftwareToneCurve, AnEffectWithNoPointsLeavesEveryPixelUntouched) {
    const auto before = solid(4, 4, 10, 128, 250, 77);
    auto px = before;
    applyEffectSoftware(makeEffect(EffectType::ToneCurve, {}), px.data(), 4, 4);
    EXPECT_EQ(px, before) << "Requirement 5.5: an empty curve is the identity";
}

TEST(SoftwareToneCurve, AMasterCurveMapsEveryChannelThroughTheSameTransferFunction) {
    // A curve from (0,0) to (1,0.5) halves everything.
    auto px = solid(1, 1, 0, 128, 254, 200);
    applyEffectSoftware(
        makeEffect(EffectType::ToneCurve, curveParams(CurveChannel::Master, {{0.0, 0.0},
                                                                            {1.0, 0.5}})),
        px.data(), 1, 1);
    EXPECT_EQ(px[0], 0u);
    EXPECT_EQ(px[1], 64u);   // 128 -> 0.50196 -> 0.25098 -> 64.0
    EXPECT_EQ(px[2], 127u);  // 254 -> 0.99608 -> 0.49804 -> 127.0
    EXPECT_EQ(px[3], 200u) << "alpha is not a colour channel and must be untouched";
}

TEST(SoftwareToneCurve, APerChannelCurveTouchesOnlyItsOwnChannel) {
    auto px = solid(1, 1, 200, 200, 200, 255);
    applyEffectSoftware(
        makeEffect(EffectType::ToneCurve, curveParams(CurveChannel::Green, {{0.0, 1.0},
                                                                           {1.0, 0.0}})),
        px.data(), 1, 1);
    EXPECT_EQ(px[0], 200u);
    EXPECT_EQ(px[2], 200u);
    EXPECT_LT(px[1], 100u) << "an inverting green curve must actually invert green";
}

// The software reference and the table it is built from must agree for EVERY input
// byte, which is the claim that makes the GPU kernel a pure lookup and therefore
// exactly equal rather than merely within a tolerance (Requirement 5.3).
TEST(SoftwareToneCurve, TheRenderedResultIsExactlyTheBakedTableForEveryInputByte) {
    const std::map<std::string, double> params =
        curveParams(CurveChannel::Master, {{0.1, 0.05}, {0.45, 0.7}, {0.9, 0.95}});
    const ToneCurveTables tables = toneCurveTables(params);

    // One pixel per possible byte value, identical across R, G and B.
    std::vector<std::uint8_t> px(256u * 4u);
    for (std::size_t v = 0; v < 256; ++v) {
        px[v * 4 + 0] = px[v * 4 + 1] = px[v * 4 + 2] = static_cast<std::uint8_t>(v);
        px[v * 4 + 3] = 255;
    }
    applyEffectSoftware(makeEffect(EffectType::ToneCurve, params), px.data(), 256, 1);

    for (std::size_t v = 0; v < 256; ++v) {
        EXPECT_EQ(px[v * 4 + 0], tables.red[v]) << "input byte " << v;
        EXPECT_EQ(px[v * 4 + 1], tables.green[v]) << "input byte " << v;
        EXPECT_EQ(px[v * 4 + 2], tables.blue[v]) << "input byte " << v;
    }
}

TEST(SoftwareToneCurve, TheEffectTypeIsWiredToItsOwnKernelAndName) {
    // The mapping half of "all seven sites": a type with no kernel silently renders
    // nothing on the GPU path, and a type whose name does not round-trip becomes
    // EffectType::Custom on open — both are silent failures rather than loud ones.
    ASSERT_TRUE(kernelForEffectType(EffectType::ToneCurve).has_value());
    EXPECT_EQ(*kernelForEffectType(EffectType::ToneCurve), EffectKernel::ToneCurve);
    EXPECT_EQ(effectTypeForKernel(EffectKernel::ToneCurve), EffectType::ToneCurve);
    EXPECT_EQ(effectKernelName(EffectKernel::ToneCurve), "tone_curve");

    const std::string src{effectKernelSource(EffectKernel::ToneCurve)};
    EXPECT_NE(src.find("#version 450"), std::string::npos);
    // The kernel must be a LOOKUP, not an evaluation: it reads a table image and does
    // no interpolation. If a future change moved curve arithmetic into the shader, the
    // exactness argument in Requirement 5.3 would quietly stop holding.
    EXPECT_NE(src.find("curveTables"), std::string::npos);
    EXPECT_EQ(src.find("mix("), std::string::npos);
}

// --- InvertColors (upstream PR 408; Requirements 14.4, 14.5) ----------------

TEST(SoftwareEffect, InvertColorsSubtractsEachRgbChannelFrom255) {
    auto px = solid(2, 2, 0, 128, 255, 200);
    applyEffectSoftware(makeEffect(EffectType::InvertColors, {}), px.data(), 2, 2);
    for (std::size_t i = 0; i < px.size(); i += 4) {
        EXPECT_EQ(px[i + 0], 255); // 255 - 0
        EXPECT_EQ(px[i + 1], 127); // 255 - 128
        EXPECT_EQ(px[i + 2], 0);   // 255 - 255
        EXPECT_EQ(px[i + 3], 200); // alpha unchanged
    }
}

TEST(SoftwareEffect, InvertColorsHoldsForEveryByteValueAndLeavesAlphaAlone) {
    // One pixel per possible channel value; alpha carries a distinct value so a
    // stray inversion of alpha would be caught.
    std::vector<std::uint8_t> px(256u * 4u);
    for (std::size_t v = 0; v < 256; ++v) {
        px[v * 4 + 0] = static_cast<std::uint8_t>(v);
        px[v * 4 + 1] = static_cast<std::uint8_t>(255 - v);
        px[v * 4 + 2] = static_cast<std::uint8_t>((v * 7) % 256);
        px[v * 4 + 3] = static_cast<std::uint8_t>((v * 3) % 256);
    }
    const auto before = px;
    applyEffectSoftware(makeEffect(EffectType::InvertColors, {}), px.data(), 256, 1);
    for (std::size_t v = 0; v < 256; ++v) {
        for (int c = 0; c < 3; ++c) {
            EXPECT_EQ(px[v * 4 + c], 255 - before[v * 4 + c]) << "value " << v << " channel " << c;
        }
        EXPECT_EQ(px[v * 4 + 3], before[v * 4 + 3]); // alpha unchanged
    }
}

TEST(SoftwareEffect, InvertColorsAppliedTwiceIsTheIdentity) {
    auto px = solid(3, 2, 17, 200, 99, 42);
    px[4] = 1;  // a couple of odd values so the round trip is not trivially symmetric
    px[8] = 254;
    const auto before = px;
    const Effect invert = makeEffect(EffectType::InvertColors, {});
    applyEffectSoftware(invert, px.data(), 3, 2);
    applyEffectSoftware(invert, px.data(), 3, 2);
    EXPECT_EQ(px, before);
}

TEST(SoftwareEffect, InvertColorsIgnoresStrayParameters) {
    auto px = solid(1, 1, 10, 20, 30, 40);
    applyEffectSoftware(makeEffect(EffectType::InvertColors, {{"amount", 0.5}}), px.data(), 1, 1);
    EXPECT_EQ(px[0], 245);
    EXPECT_EQ(px[1], 235);
    EXPECT_EQ(px[2], 225);
    EXPECT_EQ(px[3], 40);
}

// The invert-colors kernel must produce the same bytes as the software
// reference so playback (software or GPU) and export agree within 1 of 255
// (Requirements 14.5). The kernel's semantics are computed here the way its
// GLSL does — in normalized [0,1] space with a round-to-nearest rgba8 store —
// independently of the host's integer arithmetic.
TEST(SoftwareEffect, InvertColorsKernelSemanticsMatchTheSoftwareReference) {
    const auto storeUnorm = [](double n) -> int {
        const double s = std::clamp(n, 0.0, 1.0) * 255.0;
        return static_cast<int>(std::lround(s));
    };

    std::vector<std::uint8_t> cpu(256u * 4u);
    for (std::size_t v = 0; v < 256; ++v) {
        cpu[v * 4 + 0] = static_cast<std::uint8_t>(v);
        cpu[v * 4 + 1] = static_cast<std::uint8_t>((v * 5) % 256);
        cpu[v * 4 + 2] = static_cast<std::uint8_t>(255 - v);
        cpu[v * 4 + 3] = static_cast<std::uint8_t>((v * 11) % 256);
    }
    const auto src = cpu;
    applyEffectSoftware(makeEffect(EffectType::InvertColors, {}), cpu.data(), 256, 1);

    for (std::size_t v = 0; v < 256; ++v) {
        for (int c = 0; c < 3; ++c) {
            // GLSL: clamp(1.0 - value/255, 0, 1) stored back to rgba8.
            const int kernel = storeUnorm(1.0 - static_cast<double>(src[v * 4 + c]) / 255.0);
            EXPECT_LE(std::abs(kernel - static_cast<int>(cpu[v * 4 + c])), 1)
                << "value " << static_cast<int>(src[v * 4 + c]) << " channel " << c;
        }
        const int kernelAlpha = static_cast<int>(src[v * 4 + 3]); // kernel copies alpha
        EXPECT_EQ(kernelAlpha, static_cast<int>(cpu[v * 4 + 3]));
    }
}

TEST(EffectKernels, InvertColorsKernelIsMappedAndCarriesTheInversionMath) {
    EXPECT_EQ(kernelForEffectType(EffectType::InvertColors), EffectKernel::InvertColors);
    EXPECT_EQ(effectTypeForKernel(EffectKernel::InvertColors), EffectType::InvertColors);
    EXPECT_EQ(effectKernelName(EffectKernel::InvertColors), "invert_colors");

    const std::string src{effectKernelSource(EffectKernel::InvertColors)};
    EXPECT_NE(src.find("vec3(1.0) - c.rgb"), std::string::npos); // 255 - value
    EXPECT_NE(src.find("vec4(rgb, c.a)"), std::string::npos);    // alpha unchanged
}

TEST(SoftwareEffect, CustomEffectIsPassThrough) {
    auto px = solid(2, 2, 11, 22, 33, 44);
    const auto before = px;
    applyEffectSoftware(makeEffect(EffectType::Custom, {{"anything", 1.0}}),
                        px.data(), 2, 2);
    EXPECT_EQ(px, before);
}

// Requirement 6.4 (usable-editor task 9): a clip's rendered output depends on
// its effect chain's order. applyEffectSoftware transforms one effect at a time
// in place, so a chain is composited by calling it once per effect in sequence —
// this is the primitive any order-sensitive chain render is built from, and it is
// what ReorderEffectsCommand's own contract (core::EditCommands) rests on: the
// command only ever permutes an Effect list, and it is this repeated application
// that turns "which permutation" into "which pixels".
//
// Brightness is additive (px + amount*255, clamped) and Contrast is affine about
// mid-gray ((px-128)*(1+amount)+128, clamped); applying them in the two different
// orders is a textbook non-commutative pair. Hand-computed for starting value 150,
// Brightness amount 0.1, Contrast amount 0.5, with neither order clamping (so the
// difference is not an artifact of saturation):
//   brightness-then-contrast: 150 -> 176 -> 200
//   contrast-then-brightness: 150 -> 161 -> 186
TEST(SoftwareEffect, ChainOrderChangesTheRenderedResult) {
    auto brightnessThenContrast = solid(1, 1, 150, 150, 150, 255);
    applyEffectSoftware(makeEffect(EffectType::Brightness, {{"amount", 0.1}}),
                        brightnessThenContrast.data(), 1, 1);
    applyEffectSoftware(makeEffect(EffectType::Contrast, {{"amount", 0.5}}),
                        brightnessThenContrast.data(), 1, 1);

    auto contrastThenBrightness = solid(1, 1, 150, 150, 150, 255);
    applyEffectSoftware(makeEffect(EffectType::Contrast, {{"amount", 0.5}}),
                        contrastThenBrightness.data(), 1, 1);
    applyEffectSoftware(makeEffect(EffectType::Brightness, {{"amount", 0.1}}),
                        contrastThenBrightness.data(), 1, 1);

    EXPECT_NEAR(brightnessThenContrast[0], 200, 1);
    EXPECT_NEAR(contrastThenBrightness[0], 186, 1);
    EXPECT_NE(brightnessThenContrast[0], contrastThenBrightness[0])
        << "the two orders must not collapse to the same rendered value";
    // Both orders leave alpha untouched, regardless of order.
    EXPECT_EQ(brightnessThenContrast[3], 255);
    EXPECT_EQ(contrastThenBrightness[3], 255);
}

TEST(SoftwareEffect, TransitionCrossDissolvesTwoFrames) {
    auto a = solid(2, 2, 0, 0, 0, 255);
    auto b = solid(2, 2, 255, 255, 255, 255);
    std::vector<std::uint8_t> out(a.size());
    applyTransitionSoftware(a.data(), b.data(), out.data(), 2, 2, 0.5);
    for (std::size_t i = 0; i < out.size(); i += 4) {
        EXPECT_NEAR(out[i + 0], 128, 1); // 0*0.5 + 255*0.5
        EXPECT_EQ(out[i + 3], 255);       // alpha blended (255<->255)
    }
}

TEST(SoftwareEffect, TransitionEndpointsReturnInputs) {
    auto a = solid(1, 1, 10, 20, 30, 40);
    auto b = solid(1, 1, 200, 210, 220, 230);
    std::vector<std::uint8_t> out(4);

    applyTransitionSoftware(a.data(), b.data(), out.data(), 1, 1, 0.0);
    EXPECT_EQ(out, a); // progress 0 -> frame A

    applyTransitionSoftware(a.data(), b.data(), out.data(), 1, 1, 1.0);
    EXPECT_EQ(out, b); // progress 1 -> frame B
}

// ===========================================================================
// Effect kernel registry (GLSL sources, mappings, SPIR-V compilation)
// ===========================================================================

TEST(EffectKernels, EveryKernelHasWellFormedGlslSource) {
    for (const EffectKernel kernel : allEffectKernels()) {
        const std::string src{effectKernelSource(kernel)};
        SCOPED_TRACE(std::string(effectKernelName(kernel)));
        EXPECT_NE(src.find("#version 450"), std::string::npos);
        EXPECT_NE(src.find("void main"), std::string::npos);
        EXPECT_NE(src.find("layout"), std::string::npos);
        EXPECT_NE(src.find("local_size"), std::string::npos);
    }
}

TEST(EffectKernels, KernelNamesAreDistinct) {
    std::set<std::string_view> names;
    for (const EffectKernel kernel : allEffectKernels()) {
        names.insert(effectKernelName(kernel));
    }
    EXPECT_EQ(names.size(), allEffectKernels().size());
}

TEST(EffectKernels, KernelAndEffectTypeMappingsRoundTrip) {
    // The six per-clip effect kinds map both ways.
    const EffectType types[] = {EffectType::Brightness, EffectType::Contrast,
                                EffectType::Blur, EffectType::CropTransform,
                                EffectType::ColorGrade, EffectType::InvertColors};
    for (const EffectType type : types) {
        const auto kernel = kernelForEffectType(type);
        ASSERT_TRUE(kernel.has_value());
        EXPECT_EQ(effectTypeForKernel(*kernel), type);
    }

    // Transition has no EffectType; Custom has no built-in kernel.
    EXPECT_FALSE(effectTypeForKernel(EffectKernel::Transition).has_value());
    EXPECT_FALSE(kernelForEffectType(EffectType::Custom).has_value());
}

TEST(EffectKernels, CompileProducesValidSpirvOrUnsupportedWithoutShaderc) {
    Result<SpirvModule> result = compileEffectKernel(EffectKernel::Brightness);
    if (shadercAvailable()) {
        ASSERT_TRUE(result.isOk()) << result.error().toString();
        const SpirvModule& mod = result.value();
        EXPECT_TRUE(mod.valid());
        ASSERT_FALSE(mod.code.empty());
        EXPECT_EQ(mod.code.front(), 0x07230203u); // SPIR-V magic number
        EXPECT_EQ(mod.entryPoint, "main");
    } else {
        ASSERT_TRUE(result.isError());
        EXPECT_EQ(result.error().code(), ErrorCode::Unsupported);
    }
}

TEST(EffectKernels, RegistryBuildsAndRegistersEffectTypeKernels) {
    Result<EffectKernelRegistry> built = EffectKernelRegistry::build();

    if (!shadercAvailable()) {
        ASSERT_TRUE(built.isError());
        EXPECT_EQ(built.error().code(), ErrorCode::Unsupported);
        GTEST_SKIP() << "shaderc unavailable; SPIR-V registration path not exercised";
    }

    ASSERT_TRUE(built.isOk()) << built.error().toString();
    const EffectKernelRegistry& registry = built.value();
    EXPECT_EQ(registry.size(), allEffectKernels().size()); // every kernel compiled

    // Every kernel module is valid SPIR-V, including the transition kernel.
    for (const EffectKernel kernel : allEffectKernels()) {
        const SpirvModule* mod = registry.module(kernel);
        ASSERT_NE(mod, nullptr);
        EXPECT_TRUE(mod->valid());
    }
    EXPECT_NE(registry.transitionModule(), nullptr);

    auto ctx = GpuContext::softwareFallback();
    Compositor comp(ctx);
    const std::size_t registered = registry.registerWith(comp);

    // Every kernel that maps to an EffectType registers; Transition does not, because
    // it blends two inputs and so has no EffectType. Derived from allEffectKernels()
    // rather than written as a literal: this assertion was a hardcoded 6 and went stale
    // the moment ToneCurve was added, which is the whole failure mode a computed
    // expectation removes.
    std::size_t expectedRegistrations = 0;
    for (const EffectKernel kernel : allEffectKernels()) {
        if (kernel != EffectKernel::Transition) {
            ++expectedRegistrations;
        }
    }
    EXPECT_EQ(registered, expectedRegistrations);
    EXPECT_EQ(comp.registeredEffectCount(), expectedRegistrations);

    // And each one by name, so a kernel that silently failed to map is still caught.
    EXPECT_TRUE(comp.isEffectRegistered(EffectType::Brightness));
    EXPECT_TRUE(comp.isEffectRegistered(EffectType::Blur));
    EXPECT_TRUE(comp.isEffectRegistered(EffectType::CropTransform));
    EXPECT_TRUE(comp.isEffectRegistered(EffectType::ColorGrade));
    EXPECT_TRUE(comp.isEffectRegistered(EffectType::Contrast));
    EXPECT_TRUE(comp.isEffectRegistered(EffectType::InvertColors));
    EXPECT_TRUE(comp.isEffectRegistered(EffectType::ToneCurve));
}

} // namespace
} // namespace palmier::gpu
