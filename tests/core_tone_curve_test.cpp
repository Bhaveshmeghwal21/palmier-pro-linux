// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/core_tone_curve_test.cpp — the tone-curve control points, the piecewise-linear
// interpolation and the 256-entry baking (monitoring-and-grading Requirement 5).
//
// These cover the arithmetic only. The effect type, the kernel and the Inspector's
// curve control are separate; this file is what they all rest on, so it is written
// first and verified before anything is built on it.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "core/ToneCurve.hpp"

namespace palmier {
namespace {

/// Build a parameter map holding one channel's points, using the SAME name builder the
/// production code reads through — so a test cannot pass by agreeing with a
/// hand-written name that the reader spells differently.
std::map<std::string, double> pointParams(CurveChannel channel,
                                          const std::vector<CurvePoint>& points) {
    std::map<std::string, double> params;
    for (std::size_t i = 0; i < points.size(); ++i) {
        params.emplace(curvePointParameterName(channel, i, /*isY=*/false), points[i].x);
        params.emplace(curvePointParameterName(channel, i, /*isY=*/true), points[i].y);
    }
    return params;
}

// ---------------------------------------------------------------------------
// The parameter-name encoding (Requirement 5.8)
// ---------------------------------------------------------------------------

TEST(ToneCurveEncoding, EachChannelAndCoordinateHasADistinctStableName) {
    EXPECT_EQ(curvePointParameterName(CurveChannel::Master, 0, false), "curveMasterP0X");
    EXPECT_EQ(curvePointParameterName(CurveChannel::Master, 0, true), "curveMasterP0Y");
    EXPECT_EQ(curvePointParameterName(CurveChannel::Red, 2, false), "curveRedP2X");
    EXPECT_EQ(curvePointParameterName(CurveChannel::Green, 11, true), "curveGreenP11Y");
    EXPECT_EQ(curvePointParameterName(CurveChannel::Blue, 7, false), "curveBlueP7X");

    // Every (channel, index, coordinate) triple must map to a distinct name, or two
    // points would silently share storage. Checked exhaustively over a realistic range
    // rather than spot-checked, because a naming scheme that collides only at index 10
    // (e.g. one that omitted the separator) would pass any handful of examples.
    std::vector<std::string> names;
    for (const CurveChannel channel : kCurveChannels) {
        for (std::size_t i = 0; i < 64; ++i) {
            names.push_back(curvePointParameterName(channel, i, false));
            names.push_back(curvePointParameterName(channel, i, true));
        }
    }
    const std::size_t total = names.size();
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    EXPECT_EQ(names.size(), total) << "two control points would share a parameter name";
}

TEST(ToneCurveEncoding, PointsAreReadBackInTheirStoredOrder) {
    const std::vector<CurvePoint> points = {{0.0, 0.1}, {0.5, 0.9}, {1.0, 0.8}};
    const std::vector<CurvePoint> read =
        curvePoints(pointParams(CurveChannel::Master, points), CurveChannel::Master);
    ASSERT_EQ(read.size(), points.size());
    for (std::size_t i = 0; i < points.size(); ++i) {
        EXPECT_EQ(read[i], points[i]) << "point " << i;
    }
}

TEST(ToneCurveEncoding, ChannelsAreIndependent) {
    std::map<std::string, double> params = pointParams(CurveChannel::Red, {{0.0, 0.0}, {1.0, 0.5}});
    const auto blue = pointParams(CurveChannel::Blue, {{0.25, 0.75}});
    params.insert(blue.begin(), blue.end());

    EXPECT_EQ(curvePoints(params, CurveChannel::Red).size(), 2u);
    EXPECT_EQ(curvePoints(params, CurveChannel::Blue).size(), 1u);
    EXPECT_TRUE(curvePoints(params, CurveChannel::Green).empty());
    EXPECT_TRUE(curvePoints(params, CurveChannel::Master).empty());
}

// A half-written point — an X with no Y, which is exactly what a partially applied
// edit leaves behind — must not be read as a point at (x, 0). Rendering a curve
// through an implied 0 would darken the image for a reason nothing recorded.
TEST(ToneCurveEncoding, AnIndexMissingEitherCoordinateTerminatesTheList) {
    std::map<std::string, double> params =
        pointParams(CurveChannel::Master, {{0.0, 0.0}, {0.5, 0.5}});
    params.emplace(curvePointParameterName(CurveChannel::Master, 2, false), 0.9);  // X only

    const std::vector<CurvePoint> read = curvePoints(params, CurveChannel::Master);
    EXPECT_EQ(read.size(), 2u) << "an X with no Y is not a point";
}

// A gap must TRUNCATE rather than being closed up. Point indices are identities: if a
// missing p1 promoted p2 into its place, an undo entry naming "point 2" would refer to
// a different point after the gap appeared than before it.
TEST(ToneCurveEncoding, AGapTruncatesRatherThanBeingSilentlyClosedUp) {
    std::map<std::string, double> params;
    params.emplace(curvePointParameterName(CurveChannel::Master, 0, false), 0.0);
    params.emplace(curvePointParameterName(CurveChannel::Master, 0, true), 0.0);
    params.emplace(curvePointParameterName(CurveChannel::Master, 2, false), 1.0);
    params.emplace(curvePointParameterName(CurveChannel::Master, 2, true), 1.0);

    const std::vector<CurvePoint> read = curvePoints(params, CurveChannel::Master);
    ASSERT_EQ(read.size(), 1u);
    // Held in a variable rather than written inline: a braced initialiser containing a
    // comma is split by the PREPROCESSOR into two macro arguments, because parentheses
    // protect a macro argument and braces do not.
    const CurvePoint expected{0.0, 0.0};
    EXPECT_EQ(read[0], expected);
}

// ---------------------------------------------------------------------------
// Interpolation (Requirement 5.2, 5.4, 5.5, 5.6)
// ---------------------------------------------------------------------------

// Requirement 5.5, stated as three separate cases because they fail differently: an
// empty curve returning 0 would black the image, and a single-point curve clamping to
// its own y would flatten it to one colour.
TEST(ToneCurveInterpolation, FewerThanTwoPointsIsTheIdentityRatherThanAnError) {
    for (double x = 0.0; x <= 1.0; x += 0.05) {
        EXPECT_DOUBLE_EQ(evaluateToneCurve({}, x), x) << "empty curve at " << x;
        EXPECT_DOUBLE_EQ(evaluateToneCurve({{0.5, 0.9}}, x), x) << "single point at " << x;
    }
    EXPECT_EQ(bakeToneCurve({}), identityToneCurveTable());
    EXPECT_EQ(bakeToneCurve({{0.5, 0.9}}), identityToneCurveTable());
}

TEST(ToneCurveInterpolation, TwoPointsGiveTheStraightLineBetweenThem) {
    // A line from (0,0) to (1,1) is the identity; from (0, 0.25) to (1, 0.75) it is a
    // half-contrast ramp whose value at any x is stateable in closed form.
    const std::vector<CurvePoint> identity = {{0.0, 0.0}, {1.0, 1.0}};
    const std::vector<CurvePoint> ramp = {{0.0, 0.25}, {1.0, 0.75}};
    for (double x = 0.0; x <= 1.0; x += 0.05) {
        EXPECT_NEAR(evaluateToneCurve(identity, x), x, 1e-12);
        EXPECT_NEAR(evaluateToneCurve(ramp, x), 0.25 + 0.5 * x, 1e-12);
    }
}

// The shape Requirement 5 exists for: shadows lifted, highlights untouched. No
// combination of lift, gamma and gain produces this, which is why the whole feature is
// here — so the test states it explicitly rather than only testing generic segments.
TEST(ToneCurveInterpolation, ThreePointsLiftShadowsWithoutMovingHighlights) {
    const std::vector<CurvePoint> curve = {{0.0, 0.1}, {0.5, 0.5}, {1.0, 1.0}};

    EXPECT_NEAR(evaluateToneCurve(curve, 0.0), 0.1, 1e-12) << "black lifted";
    EXPECT_NEAR(evaluateToneCurve(curve, 0.25), 0.3, 1e-12) << "midpoint of the first segment";
    EXPECT_NEAR(evaluateToneCurve(curve, 0.5), 0.5, 1e-12) << "the hinge is fixed";
    EXPECT_NEAR(evaluateToneCurve(curve, 0.75), 0.75, 1e-12) << "second segment is untouched";
    EXPECT_NEAR(evaluateToneCurve(curve, 1.0), 1.0, 1e-12) << "white unmoved";
}

TEST(ToneCurveInterpolation, PointsAreSortedByInputSoTheStoredOrderDoesNotMatter) {
    const std::vector<CurvePoint> ordered = {{0.0, 0.0}, {0.5, 0.8}, {1.0, 1.0}};
    const std::vector<CurvePoint> shuffled = {{1.0, 1.0}, {0.0, 0.0}, {0.5, 0.8}};
    for (double x = 0.0; x <= 1.0; x += 0.05) {
        EXPECT_DOUBLE_EQ(evaluateToneCurve(ordered, x), evaluateToneCurve(shuffled, x))
            << "at " << x;
    }
    EXPECT_EQ(bakeToneCurve(ordered), bakeToneCurve(shuffled));
}

// Held flat, not extrapolated. Extrapolating the user's end segment off the end of the
// range is how an aggressive curve blows a highlight nobody asked for.
TEST(ToneCurveInterpolation, ValuesOutsideTheEndPointsAreHeldFlatNotExtrapolated) {
    const std::vector<CurvePoint> curve = {{0.25, 0.4}, {0.75, 0.6}};

    EXPECT_NEAR(evaluateToneCurve(curve, 0.0), 0.4, 1e-12);
    EXPECT_NEAR(evaluateToneCurve(curve, 0.1), 0.4, 1e-12);
    EXPECT_NEAR(evaluateToneCurve(curve, 0.25), 0.4, 1e-12);
    EXPECT_NEAR(evaluateToneCurve(curve, 0.75), 0.6, 1e-12);
    EXPECT_NEAR(evaluateToneCurve(curve, 0.9), 0.6, 1e-12);
    EXPECT_NEAR(evaluateToneCurve(curve, 1.0), 0.6, 1e-12);

    // Extrapolation would have carried the slope onwards: 0.4 - 0.25*0.4 = 0.3 at x=0
    // and 0.7 at x=1. Neither may appear.
    EXPECT_GT(evaluateToneCurve(curve, 0.0), 0.35);
    EXPECT_LT(evaluateToneCurve(curve, 1.0), 0.65);
}

// Requirement 5.6: an aggressive curve must clamp, never wrap. A wrap is the visible
// failure — a blown highlight coming back round as black, which reads as a hue shift.
TEST(ToneCurveInterpolation, OutputIsClampedWithoutWrapping) {
    const std::vector<CurvePoint> beyond = {{0.0, -3.0}, {1.0, 4.0}};
    for (double x = 0.0; x <= 1.0; x += 0.05) {
        const double y = evaluateToneCurve(beyond, x);
        EXPECT_GE(y, 0.0) << "at " << x;
        EXPECT_LE(y, 1.0) << "at " << x;
    }
    const ToneCurveTable table = bakeToneCurve(beyond);
    // The low end must sit at 0 and the high end at 255, and nothing in between may
    // jump the wrong way — a wrap shows up as a non-monotonic table for a monotonic
    // curve, which is precisely the artefact the requirement names.
    EXPECT_EQ(table[0], 0u);
    EXPECT_EQ(table[255], 255u);
    for (std::size_t v = 1; v < table.size(); ++v) {
        EXPECT_GE(table[v], table[v - 1]) << "table wrapped at " << v;
    }
}

TEST(ToneCurveInterpolation, TwoPointsSharingAnInputAreADeterministicStep) {
    const std::vector<CurvePoint> step = {{0.0, 0.0}, {0.5, 0.2}, {0.5, 0.8}, {1.0, 1.0}};
    // The later point wins, so the step is deterministic rather than a division by a
    // zero span.
    EXPECT_NEAR(evaluateToneCurve(step, 0.5), 0.8, 1e-12);
    EXPECT_EQ(bakeToneCurve(step), bakeToneCurve(step)) << "and repeatable";
}

// Requirement 5.4's "the same control points SHALL always produce the same transfer
// function". Baking twice and comparing catches any dependence on uninitialised
// memory or on iteration order, both of which would otherwise surface as a rare,
// unreproducible difference in a rendered frame.
TEST(ToneCurveInterpolation, BakingIsDeterministicAcrossRepeatedEvaluation) {
    const std::vector<CurvePoint> curve = {{0.05, 0.2}, {0.4, 0.35}, {0.62, 0.81}, {0.95, 0.9}};
    const ToneCurveTable first = bakeToneCurve(curve);
    for (int repeat = 0; repeat < 8; ++repeat) {
        EXPECT_EQ(bakeToneCurve(curve), first) << "repeat " << repeat;
    }

    // And the table really is the evaluated curve, entry by entry — not merely stable.
    for (std::size_t v = 0; v < first.size(); ++v) {
        const double y = evaluateToneCurve(curve, static_cast<double>(v) / 255.0) * 255.0;
        EXPECT_NEAR(first[v], y, 0.5001) << "entry " << v;
    }
}

// ---------------------------------------------------------------------------
// Baking and channel composition
// ---------------------------------------------------------------------------

TEST(ToneCurveTablesTest, NoCurvesAtAllIsAnIdentityNoOp) {
    const ToneCurveTables tables = toneCurveTables({});
    EXPECT_TRUE(tables.isIdentity());
    EXPECT_EQ(tables.red, identityToneCurveTable());
    EXPECT_EQ(tables.green, identityToneCurveTable());
    EXPECT_EQ(tables.blue, identityToneCurveTable());
}

TEST(ToneCurveTablesTest, APerChannelCurveAffectsOnlyItsOwnChannel) {
    const std::map<std::string, double> params =
        pointParams(CurveChannel::Red, {{0.0, 0.0}, {1.0, 0.5}});
    const ToneCurveTables tables = toneCurveTables(params);

    EXPECT_FALSE(tables.isIdentity());
    EXPECT_EQ(tables.red[255], 128u) << "red halved (0.5*255 = 127.5, rounded half-up)";
    EXPECT_EQ(tables.green, identityToneCurveTable());
    EXPECT_EQ(tables.blue, identityToneCurveTable());
}

TEST(ToneCurveTablesTest, AMasterCurveAffectsEveryChannelEqually) {
    const std::map<std::string, double> params =
        pointParams(CurveChannel::Master, {{0.0, 0.0}, {1.0, 0.5}});
    const ToneCurveTables tables = toneCurveTables(params);

    EXPECT_EQ(tables.red, tables.green);
    EXPECT_EQ(tables.green, tables.blue);
    EXPECT_EQ(tables.red[255], 128u);
}

// The documented composition order, asserted rather than assumed. Per-channel first,
// then master: the opposite order gives a different image from the same points, and
// nothing in the stored data records which was intended.
TEST(ToneCurveTablesTest, ThePerChannelCurveIsAppliedBeforeTheMasterCurve) {
    // Red halves its input; master then adds a fixed floor of 0.5 across the range,
    // i.e. master maps [0,1] onto [0.5,1]. Applying red first: 255 -> 128 -> master.
    std::map<std::string, double> params =
        pointParams(CurveChannel::Red, {{0.0, 0.0}, {1.0, 0.5}});
    const auto master = pointParams(CurveChannel::Master, {{0.0, 0.5}, {1.0, 1.0}});
    params.insert(master.begin(), master.end());

    const ToneCurveTables tables = toneCurveTables(params);
    const ToneCurveTable redOnly = bakeToneCurve({{0.0, 0.0}, {1.0, 0.5}});
    const ToneCurveTable masterOnly = bakeToneCurve({{0.0, 0.5}, {1.0, 1.0}});

    for (std::size_t v = 0; v < tables.red.size(); ++v) {
        EXPECT_EQ(tables.red[v], masterOnly[redOnly[v]]) << "entry " << v;
    }
    // And the two orders really do differ, so this test is discriminating rather than
    // vacuous: red-then-master at full white gives 255 -> 128 -> 192, whereas
    // master-then-red gives 255 -> 255 -> 128.
    EXPECT_NE(tables.red[255], redOnly[masterOnly[255]]);
}

TEST(ToneCurveTablesTest, ATableIsAnExactTransferFunctionForAnEightBitPipeline) {
    // The claim the whole design rests on: 256 entries is not an approximation of the
    // transfer function for an 8-bit input, it IS the transfer function. So a lookup
    // and a direct evaluation agree for EVERY possible input byte, exactly.
    const std::vector<CurvePoint> curve = {{0.1, 0.0}, {0.5, 0.7}, {0.9, 1.0}};
    const ToneCurveTable table = bakeToneCurve(curve);
    for (std::size_t v = 0; v < 256; ++v) {
        const double direct = evaluateToneCurve(curve, static_cast<double>(v) / 255.0) * 255.0;
        const auto rounded = static_cast<std::uint8_t>(
            direct <= 0.0 ? 0.0 : (direct >= 255.0 ? 255.0 : direct + 0.5));
        EXPECT_EQ(table[v], rounded) << "input byte " << v;
    }
}

}  // namespace
}  // namespace palmier
