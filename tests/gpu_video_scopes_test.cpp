// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/gpu_video_scopes_test.cpp — the histogram, luma waveform and vectorscope
// (monitoring-and-grading task 6; Requirement 6.4, 6.6, 6.8).
//
// Requirement 6.8 asks for a DOCUMENTED, ASSERTED reading on each scope for a fully
// black frame, a fully white frame and a full-saturation primary, "so a wrongly scaled
// axis is caught by test rather than by eye". The expected numbers below are therefore
// written out as literals with the arithmetic that produces them, not computed by calling
// the same helper the code under test uses -- which would assert only that the function
// equals itself and would happily pass with every axis inverted.

#include "gpu/VideoScopes.hpp"

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

namespace palmier::gpu {
namespace {

/// A tightly packed RGBA8 frame of one repeated colour.
std::vector<std::uint8_t> solidFrame(int width, int height, std::uint8_t r, std::uint8_t g,
                                     std::uint8_t b) {
    std::vector<std::uint8_t> px(static_cast<std::size_t>(width) *
                                 static_cast<std::size_t>(height) * 4u);
    for (std::size_t i = 0; i < px.size(); i += 4) {
        px[i + 0] = r;
        px[i + 1] = g;
        px[i + 2] = b;
        px[i + 3] = 255;
    }
    return px;
}

// --- The colour convention itself ------------------------------------------
//
// Every scope reading below follows from these three functions, so they are pinned
// first. Rec.601 full range: Y = 0.299R + 0.587G + 0.114B, Cb = 128 + 0.564(B-Y),
// Cr = 128 + 0.713(R-Y), each rounded and clamped to [0,255].

TEST(VideoScopeColour, LumaMatchesRec601ForBlackWhiteAndEachPrimary) {
    EXPECT_EQ(scopeLuma(0, 0, 0), 0);
    EXPECT_EQ(scopeLuma(255, 255, 255), 255);
    EXPECT_EQ(scopeLuma(255, 0, 0), 76);   // 0.299 * 255 = 76.245
    EXPECT_EQ(scopeLuma(0, 255, 0), 150);  // 0.587 * 255 = 149.685
    EXPECT_EQ(scopeLuma(0, 0, 255), 29);   // 0.114 * 255 = 29.07
}

TEST(VideoScopeColour, GreyIsNeutralChromaAtExactly128) {
    for (const int level : {0, 64, 128, 200, 255}) {
        const auto v = static_cast<std::uint8_t>(level);
        EXPECT_EQ(scopeCb(v, v, v), 128) << "grey " << level;
        EXPECT_EQ(scopeCr(v, v, v), 128) << "grey " << level;
    }
}

// A full-saturation primary lands ON the boundary because Cr for pure red is 255.45
// before clamping. That is not a rounding artefact to be tuned away: a real vectorscope
// puts the primaries at the edge of the graticule too.
TEST(VideoScopeColour, EachPrimarySitsAtItsDocumentedChromaPosition) {
    // Red: Y = 76.245, Cb = 128 + 0.564(0 - 76.245) = 84.998, Cr = 128 + 0.713(255 - 76.245)
    //      = 255.45, clamped.
    EXPECT_EQ(scopeCb(255, 0, 0), 85);
    EXPECT_EQ(scopeCr(255, 0, 0), 255);
    // Green: Y = 149.685, Cb = 128 - 84.42 = 43.58, Cr = 128 - 106.72 = 21.28.
    EXPECT_EQ(scopeCb(0, 255, 0), 44);
    EXPECT_EQ(scopeCr(0, 255, 0), 21);
    // Blue: Y = 29.07, Cb = 128 + 127.42 = 255.42 clamped, Cr = 128 - 20.73 = 107.27.
    EXPECT_EQ(scopeCb(0, 0, 255), 255);
    EXPECT_EQ(scopeCr(0, 0, 255), 107);
}

// --- Histogram -------------------------------------------------------------

TEST(ScopeHistogram, AFullyBlackFrameFillsOnlyBinZeroOnEveryChannel) {
    const auto frame = solidFrame(8, 4, 0, 0, 0);
    const Histogram h = computeHistogram(frame.data(), 8, 4);

    ASSERT_FALSE(h.isEmpty()) << "a black frame is a real reading, not an absent one";
    EXPECT_EQ(h.sampleCount, 32u);
    EXPECT_EQ(h.red[0], 32u);
    EXPECT_EQ(h.green[0], 32u);
    EXPECT_EQ(h.blue[0], 32u);
    EXPECT_EQ(h.luma[0], 32u);
    EXPECT_EQ(h.red[255], 0u);
    EXPECT_EQ(h.luma[255], 0u);
    EXPECT_EQ(h.peakCount(), 32u);
}

TEST(ScopeHistogram, AFullyWhiteFrameFillsOnlyBin255OnEveryChannel) {
    const auto frame = solidFrame(8, 4, 255, 255, 255);
    const Histogram h = computeHistogram(frame.data(), 8, 4);

    EXPECT_EQ(h.red[255], 32u);
    EXPECT_EQ(h.green[255], 32u);
    EXPECT_EQ(h.blue[255], 32u);
    EXPECT_EQ(h.luma[255], 32u) << "white's Rec.601 luma is exactly 255, not 254";
    EXPECT_EQ(h.red[0], 0u);
    EXPECT_EQ(h.luma[0], 0u);
}

// The channel-separation case: if the three channels shared a buffer, or if luma were
// read from one of them, this frame would look grey.
TEST(ScopeHistogram, AFullSaturationRedFrameSeparatesTheChannelsAndLuma) {
    const auto frame = solidFrame(10, 10, 255, 0, 0);
    const Histogram h = computeHistogram(frame.data(), 10, 10);

    EXPECT_EQ(h.red[255], 100u);
    EXPECT_EQ(h.green[0], 100u);
    EXPECT_EQ(h.blue[0], 100u);
    EXPECT_EQ(h.red[0], 0u);
    EXPECT_EQ(h.luma[76], 100u) << "0.299 * 255 = 76.245, so red's luma bin is 76";
    EXPECT_EQ(h.luma[255], 0u);
}

TEST(ScopeHistogram, EveryPixelIsCountedExactlyOncePerChannel) {
    // A gradient, so a loop that skipped or double-counted rows would not cancel out.
    std::vector<std::uint8_t> px(4u * 3u * 4u);
    for (std::size_t i = 0; i < 12; ++i) {
        px[i * 4 + 0] = static_cast<std::uint8_t>(i * 20);
        px[i * 4 + 1] = 10;
        px[i * 4 + 2] = 250;
        px[i * 4 + 3] = 255;
    }
    const Histogram h = computeHistogram(px.data(), 4, 3);

    std::uint64_t redTotal = 0;
    for (const std::uint32_t count : h.red) redTotal += count;
    EXPECT_EQ(redTotal, 12u);
    EXPECT_EQ(h.green[10], 12u);
    EXPECT_EQ(h.blue[250], 12u);
}

// Requirement 6.6: no frame is an EXPLICIT empty state. Distinguishable from black,
// which is the whole point -- a stale or misleading reading is what it replaces.
TEST(ScopeHistogram, ANullOrZeroSizedFrameIsEmptyRatherThanBlack) {
    EXPECT_TRUE(computeHistogram(nullptr, 4, 4).isEmpty());
    const auto frame = solidFrame(4, 4, 0, 0, 0);
    EXPECT_TRUE(computeHistogram(frame.data(), 0, 4).isEmpty());
    EXPECT_TRUE(computeHistogram(frame.data(), 4, 0).isEmpty());
    EXPECT_EQ(computeHistogram(nullptr, 4, 4).peakCount(), 0u);
    EXPECT_FALSE(computeHistogram(frame.data(), 4, 4).isEmpty())
        << "and a black frame must NOT read as empty";
}

// --- Luma waveform ---------------------------------------------------------

TEST(ScopeWaveform, ABlackFrameTracesTheBottomAndAWhiteFrameTheTop) {
    const auto black = solidFrame(16, 4, 0, 0, 0);
    const LumaWaveform lo = computeLumaWaveform(black.data(), 16, 4, 8);
    ASSERT_FALSE(lo.isEmpty());
    EXPECT_EQ(lo.columns, 8);
    EXPECT_EQ(lo.minLuma(), 0);
    EXPECT_EQ(lo.maxLuma(), 0);
    for (int c = 0; c < 8; ++c) {
        EXPECT_EQ(lo.at(c, 0), 8u) << "column " << c;  // 16 px wide / 8 traces * 4 rows
        EXPECT_EQ(lo.at(c, 255), 0u) << "column " << c;
    }

    const auto white = solidFrame(16, 4, 255, 255, 255);
    const LumaWaveform hi = computeLumaWaveform(white.data(), 16, 4, 8);
    EXPECT_EQ(hi.minLuma(), 255);
    EXPECT_EQ(hi.maxLuma(), 255);
    EXPECT_EQ(hi.at(0, 255), 8u);
}

TEST(ScopeWaveform, AFullSaturationRedFrameTracesItsLumaAndNotItsRedLevel) {
    const auto frame = solidFrame(8, 2, 255, 0, 0);
    const LumaWaveform w = computeLumaWaveform(frame.data(), 8, 2, 4);
    EXPECT_EQ(w.minLuma(), 76);
    EXPECT_EQ(w.maxLuma(), 76) << "a waveform monitor shows LUMA; 255 here would mean it "
                                  "is plotting the red channel";
    EXPECT_EQ(w.at(0, 76), 4u);
}

// The claim that makes a waveform usable on a 4K frame in a small panel: columns are
// BUCKETED and summed, never sampled, so nothing is dropped.
TEST(ScopeWaveform, ColumnsAreBucketedAndSummedSoNoPixelIsDropped) {
    const auto frame = solidFrame(1920, 3, 128, 128, 128);
    const LumaWaveform w = computeLumaWaveform(frame.data(), 1920, 3, 200);

    ASSERT_EQ(w.columns, 200);
    std::uint64_t total = 0;
    for (int c = 0; c < w.columns; ++c) {
        for (int level = 0; level < kScopeLevels; ++level) {
            total += w.at(c, level);
        }
    }
    EXPECT_EQ(total, 1920u * 3u) << "every pixel must appear in exactly one bucket";
    EXPECT_EQ(w.sampleCount, 1920u * 3u);
}

// A left-dark, right-bright frame must trace dark on the LEFT. An inverted or mirrored
// column mapping is the most plausible wrong answer and is invisible on a uniform frame.
TEST(ScopeWaveform, ColumnPositionFollowsHorizontalPositionInTheFrame) {
    std::vector<std::uint8_t> px(8u * 1u * 4u);
    for (std::size_t x = 0; x < 8; ++x) {
        const auto v = static_cast<std::uint8_t>(x < 4 ? 0 : 255);
        px[x * 4 + 0] = px[x * 4 + 1] = px[x * 4 + 2] = v;
        px[x * 4 + 3] = 255;
    }
    const LumaWaveform w = computeLumaWaveform(px.data(), 8, 1, 2);
    ASSERT_EQ(w.columns, 2);
    EXPECT_EQ(w.at(0, 0), 4u) << "the left half is black";
    EXPECT_EQ(w.at(0, 255), 0u);
    EXPECT_EQ(w.at(1, 255), 4u) << "the right half is white";
    EXPECT_EQ(w.at(1, 0), 0u);
}

TEST(ScopeWaveform, MoreTracesThanPixelColumnsAreClampedRatherThanLeavingGaps) {
    const auto frame = solidFrame(4, 1, 200, 200, 200);
    const LumaWaveform w = computeLumaWaveform(frame.data(), 4, 1, 64);
    EXPECT_EQ(w.columns, 4) << "an empty trace between populated ones would read as a gap "
                               "in the picture that is not there";
}

TEST(ScopeWaveform, NoFrameOrNoColumnsIsEmpty) {
    const auto frame = solidFrame(4, 4, 100, 100, 100);
    EXPECT_TRUE(computeLumaWaveform(nullptr, 4, 4, 8).isEmpty());
    EXPECT_TRUE(computeLumaWaveform(frame.data(), 4, 4, 0).isEmpty());
    EXPECT_TRUE(computeLumaWaveform(frame.data(), 0, 4, 8).isEmpty());
    EXPECT_EQ(computeLumaWaveform(nullptr, 4, 4, 8).at(0, 0), 0u);
}

// --- Vectorscope -----------------------------------------------------------

TEST(ScopeVectorscope, BlackAndWhiteBothLandEntirelyAtTheNeutralCentre) {
    for (const std::uint8_t level : {std::uint8_t{0}, std::uint8_t{255}}) {
        const auto frame = solidFrame(6, 6, level, level, level);
        const Vectorscope v = computeVectorscope(frame.data(), 6, 6);
        ASSERT_FALSE(v.isEmpty());
        EXPECT_EQ(v.neutralCount(), 36u) << "grey level " << int{level};
        EXPECT_EQ(v.at(128, 128), 36u);
        EXPECT_EQ(v.peakCount(), 36u) << "and nowhere else can hold more";
        EXPECT_EQ(v.at(85, 255), 0u);
    }
}

TEST(ScopeVectorscope, EachFullSaturationPrimaryLandsAtItsDocumentedPosition) {
    struct Expected {
        std::uint8_t r, g, b;
        int cb, cr;
        const char* name;
    };
    // The same positions asserted in EachPrimarySitsAtItsDocumentedChromaPosition, so a
    // change to the convention fails in both places rather than silently in one.
    const Expected cases[] = {{255, 0, 0, 85, 255, "red"},
                              {0, 255, 0, 44, 21, "green"},
                              {0, 0, 255, 255, 107, "blue"}};
    for (const Expected& e : cases) {
        const auto frame = solidFrame(5, 5, e.r, e.g, e.b);
        const Vectorscope v = computeVectorscope(frame.data(), 5, 5);
        EXPECT_EQ(v.at(e.cb, e.cr), 25u) << e.name;
        EXPECT_EQ(v.neutralCount(), 0u) << e.name << " is saturated, so nothing is neutral";
        EXPECT_EQ(v.sampleCount, 25u) << e.name;
    }
}

TEST(ScopeVectorscope, EveryPixelIsBinnedExactlyOnce) {
    std::vector<std::uint8_t> px(3u * 3u * 4u);
    for (std::size_t i = 0; i < 9; ++i) {
        px[i * 4 + 0] = static_cast<std::uint8_t>(i * 25);
        px[i * 4 + 1] = 40;
        px[i * 4 + 2] = 90;
        px[i * 4 + 3] = 255;
    }
    const Vectorscope v = computeVectorscope(px.data(), 3, 3);
    std::uint64_t total = 0;
    for (int cr = 0; cr < kScopeLevels; ++cr) {
        for (int cb = 0; cb < kScopeLevels; ++cb) {
            total += v.at(cb, cr);
        }
    }
    EXPECT_EQ(total, 9u);
}

TEST(ScopeVectorscope, NoFrameIsEmptyAndReadsZeroEverywhere) {
    const Vectorscope v = computeVectorscope(nullptr, 4, 4);
    EXPECT_TRUE(v.isEmpty());
    EXPECT_EQ(v.neutralCount(), 0u);
    EXPECT_EQ(v.peakCount(), 0u);
    EXPECT_EQ(v.at(128, 128), 0u);
    // Out-of-range lookups answer 0 rather than reading past the end.
    EXPECT_EQ(v.at(-1, 0), 0u);
    EXPECT_EQ(v.at(0, kScopeLevels), 0u);
}

TEST(ScopeVectorscope, AlphaDoesNotAffectAnyScope) {
    auto opaque = solidFrame(4, 4, 200, 30, 60);
    auto transparent = opaque;
    for (std::size_t i = 3; i < transparent.size(); i += 4) {
        transparent[i] = 0;
    }
    EXPECT_EQ(computeVectorscope(opaque.data(), 4, 4).bins,
              computeVectorscope(transparent.data(), 4, 4).bins);
    EXPECT_EQ(computeHistogram(opaque.data(), 4, 4).luma,
              computeHistogram(transparent.data(), 4, 4).luma);
}

}  // namespace
}  // namespace palmier::gpu
