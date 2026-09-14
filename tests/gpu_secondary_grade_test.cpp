// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/gpu_secondary_grade_test.cpp — the qualifier-masked secondary grade and the
// three colour wheels, the two halves of the upstream `color` parity gap.
//
// Added to the palmier_gpu_tests target beside gpu_effect_kernels_test.cpp rather than as a
// new executable, because that file is within twenty lines of the 1000-line ceiling.
//
// The tests are organised around the two claims that make a secondary grade a secondary
// grade rather than a second primary:
//
//   1. Where the qualifier selects nothing, the pixel is untouched. Where it selects fully,
//      the result is EXACTLY what EffectType::ColorGrade would have produced — asserted
//      byte for byte, which is what keeps the two corrections from drifting apart.
//   2. The qualifier selects on hue, saturation and luma independently, feathers by
//      `softness`, and reads the UNGRADED pixel so a correction cannot widen its own
//      selection.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "core/Effect.hpp"
#include "core/Uuid.hpp"
#include "gpu/Compositor.hpp"
#include "gpu/EffectKernels.hpp"

namespace palmier::gpu {
namespace {

[[nodiscard]] Effect makeEffect(EffectType type, std::map<std::string, double> params) {
    return Effect{Uuid::generateV4(), type, std::move(params)};
}

/// A `width` x 1 strip, one pixel per supplied RGB triple, alpha fixed at 200.
[[nodiscard]] std::vector<std::uint8_t> strip(
    const std::vector<std::array<std::uint8_t, 3>>& colours) {
    std::vector<std::uint8_t> px(colours.size() * 4u);
    for (std::size_t i = 0; i < colours.size(); ++i) {
        px[i * 4 + 0] = colours[i][0];
        px[i * 4 + 1] = colours[i][1];
        px[i * 4 + 2] = colours[i][2];
        px[i * 4 + 3] = 200;
    }
    return px;
}

/// The ten correction values, as a parameter map. Deliberately not the identity, and with
/// gamma off unity so the guarded gamma step is exercised too.
[[nodiscard]] std::map<std::string, double> correction() {
    return {{"gainR", 1.20},  {"gainG", 0.95},     {"gainB", 1.05},  {"liftR", 0.04},
            {"liftG", -0.02}, {"liftB", 0.01},     {"gammaR", 1.15}, {"gammaG", 1.0},
            {"gammaB", 0.9},  {"saturation", 1.30}};
}

// ---------------------------------------------------------------------------
// Equivalence with the primary grade
// ---------------------------------------------------------------------------

TEST(SecondaryGrade, MatchesColorGradeExactlyWhenTheQualifierSelectsEverything) {
    // THE anti-drift test. The two corrections are written out separately in
    // Compositor.cpp; this is what holds them identical, and it pins the blend as well --
    // a fully selected pixel must be graded in full, not to within a rounding step.
    const std::vector<std::array<std::uint8_t, 3>> colours{
        {0, 0, 0},     {255, 255, 255}, {128, 128, 128}, {200, 40, 40},
        {40, 200, 40}, {40, 40, 200},   {17, 200, 99},   {250, 250, 5}};

    auto primary = strip(colours);
    auto secondary = strip(colours);

    applyEffectSoftware(makeEffect(EffectType::ColorGrade, correction()), primary.data(),
                        static_cast<std::uint32_t>(colours.size()), 1);
    applyEffectSoftware(makeEffect(EffectType::SecondaryGrade, correction()), secondary.data(),
                        static_cast<std::uint32_t>(colours.size()), 1);

    EXPECT_EQ(secondary, primary)
        << "a default qualifier must select the whole frame, making the secondary grade "
           "byte-identical to the primary";
}

TEST(SecondaryGrade, AnIdentityCorrectionIsANoOpWhateverTheQualifierSelects) {
    auto px = strip({{200, 40, 40}, {40, 200, 40}, {10, 10, 10}});
    const auto before = px;
    applyEffectSoftware(makeEffect(EffectType::SecondaryGrade,
                                   {{"hueCenter", 0.0}, {"hueWidth", 30.0}, {"softness", 0.5}}),
                        px.data(), 3, 1);
    EXPECT_EQ(px, before);
}

// ---------------------------------------------------------------------------
// The qualifier actually restricts the correction
// ---------------------------------------------------------------------------

TEST(SecondaryGrade, AHueArcGradesOnlyTheSelectedHue) {
    // Pure red, green and blue. A narrow arc around red must move the red pixel and leave
    // the other two byte-identical -- which is the whole purpose of a secondary.
    auto px = strip({{200, 20, 20}, {20, 200, 20}, {20, 20, 200}});
    const auto before = px;

    auto params = correction();
    params["hueCenter"] = 0.0;  // red
    params["hueWidth"] = 40.0;  // +/- 20 degrees
    params["softness"] = 0.0;   // hard edged, so "untouched" means exactly untouched
    applyEffectSoftware(makeEffect(EffectType::SecondaryGrade, params), px.data(), 3, 1);

    // Red moved.
    EXPECT_NE(px[0], before[0]);
    // Green and blue did not, in any channel.
    for (std::size_t i = 4; i < px.size(); ++i) {
        EXPECT_EQ(px[i], before[i]) << "byte " << i << " should be outside the hue arc";
    }
}

TEST(SecondaryGrade, ALumaBandGradesOnlyTheSelectedBrightnessRange) {
    // Three greys. Selecting only the brightest must leave the darker two alone. Grey has
    // zero saturation and an undefined hue, so this isolates the luma term.
    auto px = strip({{10, 10, 10}, {128, 128, 128}, {245, 245, 245}});
    const auto before = px;

    auto params = correction();
    params["lumaMin"] = 0.8;
    params["lumaMax"] = 1.0;
    params["softness"] = 0.0;
    applyEffectSoftware(makeEffect(EffectType::SecondaryGrade, params), px.data(), 3, 1);

    EXPECT_EQ(px[0], before[0]) << "the darkest grey is below the luma band";
    EXPECT_EQ(px[4], before[4]) << "mid grey is below the luma band";
    EXPECT_NE(px[8], before[8]) << "the brightest grey is inside the luma band";
}

TEST(SecondaryGrade, ASaturationBandSeparatesGreyFromColour) {
    // Selecting only saturated pixels must leave grey alone even though grey's luma sits
    // inside the (default, full) luma band.
    auto px = strip({{128, 128, 128}, {200, 20, 20}});
    const auto before = px;

    auto params = correction();
    params["satMin"] = 0.5;
    params["satMax"] = 1.0;
    params["softness"] = 0.0;
    applyEffectSoftware(makeEffect(EffectType::SecondaryGrade, params), px.data(), 2, 1);

    EXPECT_EQ(px[0], before[0]) << "grey has zero saturation and is outside the band";
    EXPECT_NE(px[4], before[4]) << "saturated red is inside the band";
}

TEST(SecondaryGrade, SoftnessFeathersTheSelectionInsteadOfCuttingIt) {
    // A pixel just outside a hard-edged luma band is untouched; the same pixel with
    // softness applied is partially graded -- strictly between untouched and fully graded.
    // That "strictly between" is what makes this a feather rather than a wider band.
    const auto gradeOnePixel = [](double softness) {
        auto px = strip({{128, 128, 128}});
        auto params = correction();
        params["lumaMin"] = 0.65;  // mid grey's luma is 0.502, so it sits below the band
        params["lumaMax"] = 1.0;
        params["softness"] = softness;
        applyEffectSoftware(makeEffect(EffectType::SecondaryGrade, params), px.data(), 1, 1);
        return static_cast<int>(px[0]);
    };

    auto full = strip({{128, 128, 128}});
    applyEffectSoftware(makeEffect(EffectType::ColorGrade, correction()), full.data(), 1, 1);
    const int untouched = 128;
    const int fullyGraded = static_cast<int>(full[0]);
    ASSERT_NE(untouched, fullyGraded) << "the correction must move mid grey for this to mean "
                                         "anything";

    EXPECT_EQ(gradeOnePixel(0.0), untouched) << "a hard edge excludes the pixel entirely";

    const int feathered = gradeOnePixel(1.0);
    const int lo = std::min(untouched, fullyGraded);
    const int hi = std::max(untouched, fullyGraded);
    EXPECT_GT(feathered, lo);
    EXPECT_LT(feathered, hi);
}

TEST(SecondaryGrade, TheQualifierReadsTheUngradedPixel) {
    // A correction that would push an unselected pixel INTO the selection must still not
    // grade it: the mask is computed from the source image. Otherwise a gain would widen
    // its own selection, and the effect would not be idempotent under re-render.
    //
    // Dark grey (luma 0.078) with a band selecting only bright pixels, and a large gain
    // that would lift it to well inside the band if the mask were read after grading.
    auto px = strip({{20, 20, 20}});
    const auto before = px;
    applyEffectSoftware(makeEffect(EffectType::SecondaryGrade, {{"gainR", 8.0},
                                                                {"gainG", 8.0},
                                                                {"gainB", 8.0},
                                                                {"lumaMin", 0.5},
                                                                {"lumaMax", 1.0},
                                                                {"softness", 0.0}}),
                        px.data(), 1, 1);
    EXPECT_EQ(px, before);
}

TEST(SecondaryGrade, AlphaIsNeverTouched) {
    auto px = strip({{200, 20, 20}, {20, 200, 20}});
    applyEffectSoftware(makeEffect(EffectType::SecondaryGrade, correction()), px.data(), 2, 1);
    EXPECT_EQ(px[3], 200);
    EXPECT_EQ(px[7], 200);
}

// ---------------------------------------------------------------------------
// The mask itself
// ---------------------------------------------------------------------------

TEST(QualifierMask, DefaultsSelectEveryPixelAtFullWeight) {
    const QualifierValues wide;
    EXPECT_TRUE(wide.selectsEverything());
    for (const auto& c : std::vector<std::array<double, 3>>{{0.0, 0.0, 0.0},
                                                            {1.0, 1.0, 1.0},
                                                            {0.5, 0.5, 0.5},
                                                            {1.0, 0.0, 0.0},
                                                            {0.0, 1.0, 0.0},
                                                            {0.0, 0.0, 1.0},
                                                            {0.2, 0.7, 0.4}}) {
        EXPECT_DOUBLE_EQ(qualifierMask(c[0], c[1], c[2], wide), 1.0);
    }
}

TEST(QualifierMask, HueIsMeasuredTheShortWayRoundTheCircle) {
    // Red sits at hue 0. An arc centred on 350 degrees must still contain it, which only
    // holds if the distance wraps rather than being |0 - 350| = 350.
    QualifierValues q;
    q.hueWidth = 40.0;
    q.hueCenter = 350.0;
    EXPECT_DOUBLE_EQ(qualifierMask(1.0, 0.0, 0.0, q), 1.0);

    // And a centre expressed as a negative angle means the same arc.
    q.hueCenter = -10.0;
    EXPECT_DOUBLE_EQ(qualifierMask(1.0, 0.0, 0.0, q), 1.0);

    // 180 degrees away is excluded.
    q.hueCenter = 170.0;
    EXPECT_DOUBLE_EQ(qualifierMask(1.0, 0.0, 0.0, q), 0.0);
}

TEST(QualifierMask, EachPrimaryLandsOnItsExpectedHue) {
    // Pins the sector arithmetic: red 0, green 120, blue 240. A sign error in the
    // floor-based modulo shows up here and nowhere else.
    const std::vector<std::pair<std::array<double, 3>, double>> cases{
        {{1.0, 0.0, 0.0}, 0.0},  {{0.0, 1.0, 0.0}, 120.0}, {{0.0, 0.0, 1.0}, 240.0},
        {{1.0, 1.0, 0.0}, 60.0}, {{0.0, 1.0, 1.0}, 180.0}, {{1.0, 0.0, 1.0}, 300.0}};
    for (const auto& [rgb, hue] : cases) {
        QualifierValues q;
        q.hueWidth = 10.0;  // a tight arc, so only the right hue passes
        q.hueCenter = hue;
        EXPECT_DOUBLE_EQ(qualifierMask(rgb[0], rgb[1], rgb[2], q), 1.0) << "expected hue " << hue;
    }
}

TEST(QualifierMask, GreyHasNoSaturationSoAHueArcCannotSelectIt) {
    // Grey's hue is undefined; the implementation reports 0, which would make a red arc
    // select every grey pixel if saturation were not also considered. Here the arc alone
    // does select it (hue 0), so the guard that matters in practice is the saturation band.
    QualifierValues q;
    q.hueWidth = 20.0;
    q.hueCenter = 0.0;
    q.satMin = 0.2;  // grey has saturation 0
    EXPECT_DOUBLE_EQ(qualifierMask(0.5, 0.5, 0.5, q), 0.0);
}

TEST(QualifierMask, WeightFallsOffLinearlyAcrossTheFeatherAndThenStopsAtZero) {
    QualifierValues q;
    q.lumaMin = 0.5;
    q.lumaMax = 1.0;
    q.softness = 1.0;  // luma feather is softness * 0.5 == 0.5

    // Grey at luma 0.5 is exactly on the boundary: full weight.
    EXPECT_DOUBLE_EQ(qualifierMask(0.5, 0.5, 0.5, q), 1.0);
    // A quarter of the way through the feather (luma 0.375) keeps half its weight.
    EXPECT_NEAR(qualifierMask(0.375, 0.375, 0.375, q), 0.75, 1e-9);
    // Beyond the feather, zero rather than a negative weight.
    EXPECT_DOUBLE_EQ(qualifierMask(0.0, 0.0, 0.0, q), 0.0);
}

TEST(QualifierMask, AnOutOfRangeSoftnessIsClampedRatherThanExtrapolated) {
    QualifierValues q;
    q.lumaMin = 0.9;
    q.lumaMax = 1.0;
    q.softness = 50.0;  // clamped to 1
    QualifierValues clamped = q;
    clamped.softness = 1.0;
    EXPECT_DOUBLE_EQ(qualifierMask(0.5, 0.5, 0.5, q), qualifierMask(0.5, 0.5, 0.5, clamped));

    q.softness = -3.0;  // clamped to 0
    clamped.softness = 0.0;
    EXPECT_DOUBLE_EQ(qualifierMask(0.5, 0.5, 0.5, q), qualifierMask(0.5, 0.5, 0.5, clamped));
}

TEST(SecondaryGrade, ValuesResolveTheCorrectionAndTheQualifierIndependently) {
    const SecondaryGradeValues absent = secondaryGradeValues(std::map<std::string, double>{});
    EXPECT_TRUE(absent.correction.isIdentity());
    EXPECT_TRUE(absent.qualifier.selectsEverything());
    EXPECT_TRUE(absent.isIdentity());

    // The correction shares ColorGrade's names, including its legacy scalar `lift` fallback,
    // so an old project's grade means the same thing in a secondary.
    const SecondaryGradeValues legacy =
        secondaryGradeValues(std::map<std::string, double>{{"lift", 0.25}});
    EXPECT_DOUBLE_EQ(legacy.correction.liftR, 0.25);
    EXPECT_DOUBLE_EQ(legacy.correction.liftG, 0.25);
    EXPECT_DOUBLE_EQ(legacy.correction.liftB, 0.25);

    const SecondaryGradeValues narrowed =
        secondaryGradeValues(std::map<std::string, double>{{"hueWidth", 25.0}, {"satMin", 0.3}});
    EXPECT_FALSE(narrowed.qualifier.selectsEverything());
    EXPECT_DOUBLE_EQ(narrowed.qualifier.hueWidth, 25.0);
    EXPECT_DOUBLE_EQ(narrowed.qualifier.satMin, 0.3);
    // An unnamed qualifier field keeps its widest default rather than collapsing to zero.
    EXPECT_DOUBLE_EQ(narrowed.qualifier.satMax, 1.0);
    EXPECT_DOUBLE_EQ(narrowed.qualifier.lumaMax, 1.0);
}

// ---------------------------------------------------------------------------
// Colour wheels
// ---------------------------------------------------------------------------
//
// The wheels are folded into the nine grade numbers by core::colorGradeValues, so these
// tests work on that resolution rather than on pixels: if the fold is right, every render
// path inherits it, which is the reason the fold lives there.

TEST(ColorWheel, AnUnsetWheelIsAnExactNoOp) {
    // The whole compatibility argument for folding wheels into an existing effect rests on
    // this: a project saved before wheels existed must resolve to exactly what it did.
    const ColorGradeValues plain = colorGradeValues(std::map<std::string, double>{
        {"gainR", 1.1}, {"liftG", 0.05}, {"gammaB", 1.2}, {"saturation", 0.9}});
    const ColorGradeValues withZeroWheels =
        colorGradeValues(std::map<std::string, double>{{"gainR", 1.1},
                                                       {"liftG", 0.05},
                                                       {"gammaB", 1.2},
                                                       {"saturation", 0.9},
                                                       {"shadowsHue", 210.0},
                                                       {"shadowsAmount", 0.0},
                                                       {"midtonesHue", 90.0},
                                                       {"midtonesAmount", 0.0},
                                                       {"highlightsHue", 45.0},
                                                       {"highlightsAmount", 0.0}});

    EXPECT_DOUBLE_EQ(withZeroWheels.liftR, plain.liftR);
    EXPECT_DOUBLE_EQ(withZeroWheels.liftG, plain.liftG);
    EXPECT_DOUBLE_EQ(withZeroWheels.liftB, plain.liftB);
    EXPECT_DOUBLE_EQ(withZeroWheels.gammaR, plain.gammaR);
    EXPECT_DOUBLE_EQ(withZeroWheels.gammaB, plain.gammaB);
    EXPECT_DOUBLE_EQ(withZeroWheels.gainR, plain.gainR);
    EXPECT_TRUE(colorGradeValues(std::map<std::string, double>{{"shadowsHue", 33.0}}).isIdentity())
        << "a hue with no amount must not disturb the identity check";
}

TEST(ColorWheel, OffsetsSumToZeroSoAWheelIsABalanceNotABrightness) {
    // The defining property. Checked across the circle, including between primaries.
    for (double hue = 0.0; hue < 360.0; hue += 17.0) {
        for (const double amount : {0.25, 0.5, 1.0}) {
            const ColorWheelOffsets d = colorWheelOffsets(hue, amount);
            EXPECT_NEAR(d.r + d.g + d.b, 0.0, 1e-12) << "hue " << hue << " amount " << amount;
        }
    }
}

TEST(ColorWheel, EachPrimaryHuePushesItsOwnChannelHardest) {
    const ColorWheelOffsets red = colorWheelOffsets(0.0, 1.0);
    EXPECT_GT(red.r, red.g);
    EXPECT_GT(red.r, red.b);

    const ColorWheelOffsets green = colorWheelOffsets(120.0, 1.0);
    EXPECT_GT(green.g, green.r);
    EXPECT_GT(green.g, green.b);

    const ColorWheelOffsets blue = colorWheelOffsets(240.0, 1.0);
    EXPECT_GT(blue.b, blue.r);
    EXPECT_GT(blue.b, blue.g);
}

TEST(ColorWheel, AmountScalesLinearlyAndIsClamped) {
    const ColorWheelOffsets half = colorWheelOffsets(0.0, 0.5);
    const ColorWheelOffsets full = colorWheelOffsets(0.0, 1.0);
    EXPECT_NEAR(half.r * 2.0, full.r, 1e-12);

    // Beyond the edge of the circle the wheel stops rather than extrapolating.
    EXPECT_DOUBLE_EQ(colorWheelOffsets(0.0, 9.0).r, full.r);
    EXPECT_DOUBLE_EQ(colorWheelOffsets(0.0, -4.0).r, 0.0);
}

TEST(ColorWheel, HueWrapsInBothDirections) {
    const ColorWheelOffsets red = colorWheelOffsets(0.0, 1.0);
    for (const double equivalent : {360.0, 720.0, -360.0}) {
        const ColorWheelOffsets same = colorWheelOffsets(equivalent, 1.0);
        EXPECT_NEAR(same.r, red.r, 1e-12) << "hue " << equivalent;
        EXPECT_NEAR(same.g, red.g, 1e-12) << "hue " << equivalent;
        EXPECT_NEAR(same.b, red.b, 1e-12) << "hue " << equivalent;
    }
    // A negative angle wraps rather than reflecting: -120 is the same hue as 240 (blue).
    const ColorWheelOffsets blue = colorWheelOffsets(240.0, 1.0);
    const ColorWheelOffsets negative = colorWheelOffsets(-120.0, 1.0);
    EXPECT_NEAR(negative.b, blue.b, 1e-12);
}

TEST(ColorWheel, EachWheelDrivesItsOwnBandAndLeavesTheOthersAlone) {
    // shadows -> lift, midtones -> gamma, highlights -> gain. A wheel that leaked into
    // another band would make the three controls interfere, which is the one thing a
    // three-wheel grade must not do.
    const ColorGradeValues shadows = colorGradeValues(
        std::map<std::string, double>{{"shadowsHue", 0.0}, {"shadowsAmount", 1.0}});
    EXPECT_GT(shadows.liftR, 0.0);
    EXPECT_DOUBLE_EQ(shadows.gammaR, 1.0);
    EXPECT_DOUBLE_EQ(shadows.gainR, 1.0);

    const ColorGradeValues midtones = colorGradeValues(
        std::map<std::string, double>{{"midtonesHue", 0.0}, {"midtonesAmount", 1.0}});
    EXPECT_GT(midtones.gammaR, 1.0);
    EXPECT_DOUBLE_EQ(midtones.liftR, 0.0);
    EXPECT_DOUBLE_EQ(midtones.gainR, 1.0);

    const ColorGradeValues highlights = colorGradeValues(
        std::map<std::string, double>{{"highlightsHue", 0.0}, {"highlightsAmount", 1.0}});
    EXPECT_GT(highlights.gainR, 1.0);
    EXPECT_DOUBLE_EQ(highlights.liftR, 0.0);
    EXPECT_DOUBLE_EQ(highlights.gammaR, 1.0);
}

TEST(ColorWheel, AWheelComposesWithAnExplicitValueRatherThanReplacingIt) {
    // Lift is an offset so the wheel adds; gain is a multiplier so the wheel scales. Both
    // must build on whatever the caller already set, or a wheel would silently discard it.
    const ColorGradeValues v =
        colorGradeValues(std::map<std::string, double>{{"liftR", 0.10},
                                                       {"gainR", 2.0},
                                                       {"shadowsHue", 0.0},
                                                       {"shadowsAmount", 1.0},
                                                       {"highlightsHue", 0.0},
                                                       {"highlightsAmount", 1.0}});
    const ColorWheelOffsets d = colorWheelOffsets(0.0, 1.0);
    EXPECT_NEAR(v.liftR, 0.10 + d.r * kColorWheelScale, 1e-12);
    EXPECT_NEAR(v.gainR, 2.0 * (1.0 + d.r * kColorWheelScale), 1e-12);
}

TEST(ColorWheel, AWheelActuallyChangesRenderedPixels) {
    // The fold is only worth anything if it reaches the image. A neutral grey graded with
    // a red highlights wheel must come out with more red than blue.
    auto px = strip({{160, 160, 160}});
    applyEffectSoftware(
        makeEffect(EffectType::ColorGrade, {{"highlightsHue", 0.0}, {"highlightsAmount", 1.0}}),
        px.data(), 1, 1);
    EXPECT_GT(px[0], px[2]) << "a red wheel must leave red above blue";
    EXPECT_GT(px[0], 160) << "and must raise red above where it started";
}

TEST(ColorWheel, TheSecondaryGradeInheritsTheWheelsThroughTheSharedCorrection) {
    // SecondaryGrade resolves its correction with colorGradeValues, so it gets the wheels
    // for free -- and with a default qualifier it must match the primary exactly.
    const std::map<std::string, double> params{{"midtonesHue", 200.0},
                                               {"midtonesAmount", 0.8},
                                               {"highlightsHue", 20.0},
                                               {"highlightsAmount", 0.4}};
    auto primary = strip({{60, 120, 200}, {200, 120, 60}});
    auto secondary = primary;
    applyEffectSoftware(makeEffect(EffectType::ColorGrade, params), primary.data(), 2, 1);
    applyEffectSoftware(makeEffect(EffectType::SecondaryGrade, params), secondary.data(), 2, 1);
    EXPECT_EQ(secondary, primary);
}

TEST(ColorWheel, TheParameterNamesAreTheOnesTheResolverReads) {
    // Pins the published vocabulary against the resolver: a name in the list that the
    // resolver ignores would be documented and inert, which is worse than absent.
    ASSERT_EQ(kColorWheelParameterNames.size(), 6u);
    for (const char* name : kColorWheelParameterNames) {
        const std::string key{name};
        const bool isAmount = key.size() > 6 && key.substr(key.size() - 6) == "Amount";
        // Setting a hue alone changes nothing; setting its amount must change something.
        const ColorGradeValues moved =
            colorGradeValues(std::map<std::string, double>{{key, isAmount ? 1.0 : 120.0}});
        if (isAmount) {
            EXPECT_FALSE(moved.isIdentity()) << key << " must move the grade";
        } else {
            EXPECT_TRUE(moved.isIdentity()) << key << " alone must not move the grade";
        }
    }
}

TEST(EffectKernels, SecondaryGradeKernelIsMappedAndCarriesBothHalves) {
    EXPECT_EQ(kernelForEffectType(EffectType::SecondaryGrade), EffectKernel::SecondaryGrade);
    EXPECT_EQ(effectTypeForKernel(EffectKernel::SecondaryGrade), EffectType::SecondaryGrade);
    EXPECT_EQ(effectKernelName(EffectKernel::SecondaryGrade), "secondary_grade");

    const std::string src{effectKernelSource(EffectKernel::SecondaryGrade)};
    EXPECT_NE(src.find("#version 450"), std::string::npos);
    // The qualifier half.
    EXPECT_NE(src.find("qualifierMask"), std::string::npos);
    EXPECT_NE(src.find("bandWeight"), std::string::npos);
    // `mod`, not a C-style remainder: the host reference floors for exactly this reason.
    EXPECT_NE(src.find("mod(sector, 6.0)"), std::string::npos);
    // The correction half, identical to the primary grade's.
    EXPECT_NE(src.find("mix(vec3(luma), rgb, pc.saturation)"), std::string::npos);
    // A fully selected pixel takes the graded value directly rather than mixing to it.
    EXPECT_NE(src.find("mask >= 1.0 ? rgb"), std::string::npos);
}

}  // namespace
}  // namespace palmier::gpu
