// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/gpu_cube_lut_test.cpp — the `.cube` parser and the trilinear interpolator
// (monitoring-and-grading task 7; Requirement 7.4, 7.5, 7.6, 7.7).
//
// Requirement 7.7 makes the identity LUT the shared correctness anchor of the parser and
// the interpolator: if a table round-trips as the identity AND sampling it returns its
// input, then the index order, the domain scaling and the interpolation weights are all
// consistent. Almost every other case here is about what must be REJECTED, because
// Requirement 7.4's real content is "rather than applying a partially read table" -- a LUT
// missing its last rows renders most of the image correctly and the highlights wrongly,
// which is far harder to diagnose than a refusal.

#include "gpu/CubeLut.hpp"

#include <string>

#include <cmath>
#include <vector>

#include <gtest/gtest.h>

namespace palmier::gpu {
namespace {

/// `.cube` text for a size-2 identity, written out longhand so the fixture itself is
/// readable and does not depend on the code under test.
const char* kIdentity2 = R"(# a hand-written identity
TITLE "identity"
LUT_3D_SIZE 2

0.0 0.0 0.0
1.0 0.0 0.0
0.0 1.0 0.0
1.0 1.0 0.0
0.0 0.0 1.0
1.0 0.0 1.0
0.0 1.0 1.0
1.0 1.0 1.0
)";

/// Serialise a CubeLut back to `.cube` text, so a generated table can be fed to the parser.
std::string toCubeText(const CubeLut& lut) {
    std::string out = "LUT_3D_SIZE " + std::to_string(lut.size()) + "\n";
    for (const LutEntry& e : lut.entries()) {
        out += std::to_string(e.r) + " " + std::to_string(e.g) + " " + std::to_string(e.b) + "\n";
    }
    return out;
}

// --- The shared anchor (7.7) -----------------------------------------------

TEST(CubeLutParse, AHandWrittenIdentityParsesAsTheIdentityWithTheExpectedIndexOrder) {
    const auto parsed = parseCubeLut(kIdentity2);
    ASSERT_TRUE(parsed.isOk()) << parsed.error().message();
    const CubeLut& lut = parsed.value();
    EXPECT_EQ(lut.size(), 2);
    EXPECT_EQ(lut.entries().size(), 8u);
    EXPECT_TRUE(lut.isIdentity());

    // RED VARIES FASTEST: row 1 is (1,0,0), so at(1,0,0) is red. A transposed table would
    // still be a plausible LUT and would still have eight rows, so this is the assertion
    // that catches it.
    //
    // Held in named variables because a braced initialiser at a gtest macro's TOP LEVEL is
    // split by the preprocessor on its commas -- parentheses protect a macro argument and
    // braces do not. This is CI incident 4 repeating; see the note in core_tone_curve_test.
    const LutEntry red{1.0f, 0.0f, 0.0f};
    const LutEntry green{0.0f, 1.0f, 0.0f};
    const LutEntry blue{0.0f, 0.0f, 1.0f};
    EXPECT_EQ(lut.at(1, 0, 0), red);
    EXPECT_EQ(lut.at(0, 1, 0), green);
    EXPECT_EQ(lut.at(0, 0, 1), blue);
}

TEST(CubeLutParse, CommentsBlankLinesAndTitleAreIgnoredRatherThanRejected) {
    const std::string text = std::string("\n\n# leading comment\n") + kIdentity2 +
                             "\n   \n# trailing comment\n";
    const auto parsed = parseCubeLut(text);
    ASSERT_TRUE(parsed.isOk()) << parsed.error().message();
    EXPECT_TRUE(parsed.value().isIdentity());
}

TEST(CubeLutParse, ATrailingCommentOnADataRowIsStrippedRatherThanFailingTheRow) {
    std::string text = "LUT_3D_SIZE 2\n";
    const CubeLut identity = identityCubeLut(2);
    for (const LutEntry& e : identity.entries()) {
        text += std::to_string(e.r) + " " + std::to_string(e.g) + " " + std::to_string(e.b) +
                "  # a note\n";
    }
    const auto parsed = parseCubeLut(text);
    ASSERT_TRUE(parsed.isOk()) << parsed.error().message();
    EXPECT_TRUE(parsed.value().isIdentity());
}

TEST(CubeLutParse, TheDefaultDomainIsAcceptedAndANonDefaultOneIsRejectedRatherThanIgnored) {
    const std::string ok = std::string("DOMAIN_MIN 0.0 0.0 0.0\nDOMAIN_MAX 1.0 1.0 1.0\n") +
                           kIdentity2;
    EXPECT_TRUE(parseCubeLut(ok).isOk());

    // Silently ignoring a 0..4 HDR domain would apply the look at the wrong scale, which
    // reads as a bad LUT rather than as unsupported input.
    const std::string hdr = std::string("DOMAIN_MAX 4.0 4.0 4.0\n") + kIdentity2;
    const auto rejected = parseCubeLut(hdr);
    ASSERT_TRUE(rejected.isError());
    EXPECT_EQ(rejected.error().code(), ErrorCode::InvalidArgument);
    EXPECT_NE(rejected.error().message().find("DOMAIN_MAX"), std::string::npos)
        << rejected.error().message();
}

// --- Every rejection names its fault (7.4, 7.5) ----------------------------

TEST(CubeLutParse, EveryMalformedFileIsRejectedWithAnErrorNamingWhatWasWrong) {
    struct Case {
        const char* text;
        const char* mustMention;
        const char* why;
    };
    const Case cases[] = {
        {"", "LUT_3D_SIZE", "an empty file declares no size"},
        {"# only a comment\n", "LUT_3D_SIZE", "a comment-only file declares no size"},
        {"0.0 0.0 0.0\nLUT_3D_SIZE 2\n", "before LUT_3D_SIZE", "data before the declaration"},
        {"LUT_3D_SIZE\n", "single integer", "the size is missing its value"},
        {"LUT_3D_SIZE two\n", "single integer", "the size is not a number"},
        {"LUT_3D_SIZE 1\n", "outside the supported range", "1 defines no interpolation"},
        {"LUT_3D_SIZE 0\n", "outside the supported range", "zero is not a size"},
        {"LUT_3D_SIZE -2\n", "outside the supported range", "negative is not a size"},
        {"LUT_3D_SIZE 4096\n", "outside the supported range", "beyond the format's bound"},
        {"LUT_1D_SIZE 8\n", "1D LUT", "a 1D LUT is a different format"},
        {"LUT_3D_SIZE 2\nLUT_3D_SIZE 2\n", "more than once", "two declarations conflict"},
        {"LUT_3D_SIZE 2\n0.0 0.0\n", "exactly 3", "a row of two numbers"},
        {"LUT_3D_SIZE 2\n0.0 0.0 0.0 0.0\n", "exactly 3", "a row of four numbers"},
        {"LUT_3D_SIZE 2\n0.0 0.0 abc\n", "is not a number", "a non-numeric field"},
        {"LUT_3D_SIZE 2\n0.0 0.0 0.5x\n", "is not a number", "a partially numeric field"},
        {"LUT_3D_SIZE 2\n0.0 0.0 1.5\n", "outside the 0..1 domain", "a value above 1"},
        {"LUT_3D_SIZE 2\n0.0 0.0 -0.1\n", "outside the 0..1 domain", "a value below 0"},
    };
    for (const Case& c : cases) {
        const auto result = parseCubeLut(c.text);
        ASSERT_TRUE(result.isError()) << c.why;
        EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument) << c.why;
        EXPECT_NE(result.error().message().find(c.mustMention), std::string::npos)
            << c.why << " -- got: " << result.error().message();
    }
}

// Requirement 7.5, checked in BOTH directions: too few rows is a truncated download, too
// many is a concatenated file, and either would render part of the image wrongly.
TEST(CubeLutParse, ADeclaredSizeDisagreeingWithTheRowCountIsRejectedEitherWay) {
    const CubeLut identity = identityCubeLut(2);
    const std::string full = toCubeText(identity);

    // Drop the last row.
    std::string truncated = full;
    truncated.erase(truncated.find_last_of('\n', truncated.size() - 2) + 1);
    const auto tooFew = parseCubeLut(truncated);
    ASSERT_TRUE(tooFew.isError());
    EXPECT_NE(tooFew.error().message().find("needs 8 data rows"), std::string::npos)
        << tooFew.error().message();
    EXPECT_NE(tooFew.error().message().find("has 7"), std::string::npos)
        << tooFew.error().message();

    const auto tooMany = parseCubeLut(full + "0.5 0.5 0.5\n");
    ASSERT_TRUE(tooMany.isError());
    EXPECT_NE(tooMany.error().message().find("has 9"), std::string::npos)
        << tooMany.error().message();
}

// --- Trilinear interpolation (7.6, 7.7) ------------------------------------

TEST(CubeLutSample, SamplingAnIdentityTableReturnsItsInputAtEverySize) {
    // The other half of the shared anchor: parsing the identity is not enough if sampling
    // it moves the value. Several sizes, since an off-by-one in the domain scaling shows
    // up at one size and not another.
    for (const int size : {2, 3, 5, 17}) {
        const CubeLut lut = identityCubeLut(size);
        ASSERT_TRUE(lut.isIdentity()) << "size " << size;
        for (const float v : {0.0f, 0.125f, 0.5f, 0.7331f, 1.0f}) {
            const LutEntry out = lut.sample(v, v, v);
            EXPECT_NEAR(out.r, v, 1.0f / 255.0f) << "size " << size << " v " << v;
            EXPECT_NEAR(out.g, v, 1.0f / 255.0f) << "size " << size << " v " << v;
            EXPECT_NEAR(out.b, v, 1.0f / 255.0f) << "size " << size << " v " << v;
        }
    }
}

TEST(CubeLutSample, InterpolationIsTrilinearRatherThanNearestNeighbour) {
    // A size-2 table that inverts. Halfway along every axis must give 0.5 exactly; a
    // nearest-neighbour lookup would give 0 or 1 and pass every identity test above.
    std::vector<LutEntry> entries;
    for (int b = 0; b < 2; ++b) {
        for (int g = 0; g < 2; ++g) {
            for (int r = 0; r < 2; ++r) {
                entries.push_back(LutEntry{1.0f - static_cast<float>(r),
                                           1.0f - static_cast<float>(g),
                                           1.0f - static_cast<float>(b)});
            }
        }
    }
    const CubeLut inverting{2, std::move(entries)};
    EXPECT_FALSE(inverting.isIdentity());

    const LutEntry mid = inverting.sample(0.5f, 0.5f, 0.5f);
    EXPECT_NEAR(mid.r, 0.5f, 1e-6f);
    EXPECT_NEAR(mid.g, 0.5f, 1e-6f);
    EXPECT_NEAR(mid.b, 0.5f, 1e-6f);

    // And a quarter of the way is 0.75, which a linear blend gives and a nearest lookup
    // does not.
    EXPECT_NEAR(inverting.sample(0.25f, 0.25f, 0.25f).r, 0.75f, 1e-6f);
    EXPECT_NEAR(inverting.sample(0.0f, 0.0f, 0.0f).r, 1.0f, 1e-6f);
    EXPECT_NEAR(inverting.sample(1.0f, 1.0f, 1.0f).r, 0.0f, 1e-6f);
}

TEST(CubeLutSample, EachAxisIsIndependentSoAChannelSwapWouldBeCaught) {
    // A table that maps red to 1 and leaves green and blue alone. If the sampler mixed
    // its axes, moving GREEN would change the red output.
    const int size = 2;
    std::vector<LutEntry> entries;
    for (int b = 0; b < size; ++b) {
        for (int g = 0; g < size; ++g) {
            for (int r = 0; r < size; ++r) {
                entries.push_back(LutEntry{static_cast<float>(r) * 0.5f,
                                           static_cast<float>(g), static_cast<float>(b)});
            }
        }
    }
    const CubeLut lut{size, std::move(entries)};
    EXPECT_NEAR(lut.sample(1.0f, 0.0f, 0.0f).r, 0.5f, 1e-6f) << "red is halved";
    EXPECT_NEAR(lut.sample(0.0f, 1.0f, 0.0f).r, 0.0f, 1e-6f) << "green must not affect red";
    EXPECT_NEAR(lut.sample(0.0f, 1.0f, 0.0f).g, 1.0f, 1e-6f);
    EXPECT_NEAR(lut.sample(0.0f, 0.0f, 1.0f).b, 1.0f, 1e-6f);
}

TEST(CubeLutSample, OutOfDomainInputIsClampedRatherThanExtrapolated) {
    const CubeLut lut = identityCubeLut(4);
    // Continuing the edge gradient past the domain is how a LUT blows a highlight nobody
    // asked for, so 1.5 must read as 1.0 rather than as 1.5.
    EXPECT_NEAR(lut.sample(1.5f, 1.5f, 1.5f).r, 1.0f, 1e-6f);
    EXPECT_NEAR(lut.sample(-0.5f, -0.5f, -0.5f).r, 0.0f, 1e-6f);
}

TEST(CubeLutSample, ANaNInputIsTreatedAsZeroRatherThanPropagating) {
    // NaN in a colour channel does not glitch visibly, it poisons everything downstream,
    // so the clamp catches it deliberately rather than by accident.
    const CubeLut lut = identityCubeLut(3);
    const LutEntry out = lut.sample(std::nanf(""), 0.5f, 0.5f);
    EXPECT_FALSE(std::isnan(out.r));
    EXPECT_NEAR(out.r, 0.0f, 1e-6f);
}

TEST(CubeLutSample, AnEmptyTableIsTheIdentityTransformAndNotAnError) {
    const CubeLut none;
    EXPECT_TRUE(none.empty());
    EXPECT_FALSE(none.isIdentity()) << "an absent table is nothing, not the identity table";
    const LutEntry out = none.sample(0.25f, 0.5f, 0.75f);
    EXPECT_NEAR(out.r, 0.25f, 1e-6f) << "but sampling it must pass the value through";
    EXPECT_NEAR(out.g, 0.5f, 1e-6f);
    EXPECT_NEAR(out.b, 0.75f, 1e-6f);
}

TEST(CubeLutSample, AnOutOfRangeLatticeLookupAnswersZeroRatherThanReadingPastTheEnd) {
    const CubeLut lut = identityCubeLut(2);
    EXPECT_EQ(lut.at(-1, 0, 0), LutEntry{});
    EXPECT_EQ(lut.at(0, 2, 0), LutEntry{});
    EXPECT_EQ(lut.at(0, 0, 99), LutEntry{});
}

TEST(CubeLutParse, AGeneratedTableSurvivesARoundTripThroughTextAtSeveralSizes) {
    for (const int size : {2, 3, 8}) {
        const CubeLut original = identityCubeLut(size);
        const auto parsed = parseCubeLut(toCubeText(original));
        ASSERT_TRUE(parsed.isOk()) << "size " << size << ": " << parsed.error().message();
        EXPECT_EQ(parsed.value().size(), size);
        EXPECT_EQ(parsed.value().entries().size(), original.entries().size());
        EXPECT_TRUE(parsed.value().isIdentity()) << "size " << size;
    }
}

}  // namespace
}  // namespace palmier::gpu
