// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/gpu_lut_cache_test.cpp — the missing-LUT degradation and the effect wiring
// (monitoring-and-grading task 7; Requirement 7.6, 7.8).
//
// Requirement 7.8 is a set of four demands about a LUT whose file is gone: the clip renders
// UN-GRADED, the effect STAYS in the chain, the failure IS reported naming the path, and the
// open is NOT failed. Three of those are properties of this cache returning an empty table
// instead of an error, and the fourth is a property of nothing here throwing or refusing --
// so each is asserted directly rather than inferred from the absence of a crash.
//
// The reader is injected, so the missing-file path is exercised without arranging for a
// missing file and the malformed-file path without writing one.

#include "gpu/LutCache.hpp"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/Effect.hpp"
#include "core/Error.hpp"
#include "core/Uuid.hpp"
#include "gpu/Compositor.hpp"
#include "gpu/EffectKernels.hpp"

namespace palmier::gpu {
namespace {

/// `.cube` text for a size-2 table that halves every channel.
std::string halvingCubeText() {
    std::string text = "LUT_3D_SIZE 2\n";
    for (int b = 0; b < 2; ++b) {
        for (int g = 0; g < 2; ++g) {
            for (int r = 0; r < 2; ++r) {
                text += std::to_string(r * 0.5) + " " + std::to_string(g * 0.5) + " " +
                        std::to_string(b * 0.5) + "\n";
            }
        }
    }
    return text;
}

std::string identityCubeText(int size) {
    const CubeLut lut = identityCubeLut(size);
    std::string text = "LUT_3D_SIZE " + std::to_string(size) + "\n";
    for (const LutEntry& e : lut.entries()) {
        text += std::to_string(e.r) + " " + std::to_string(e.g) + " " + std::to_string(e.b) + "\n";
    }
    return text;
}

/// A one-pixel RGBA frame.
std::vector<std::uint8_t> pixel(std::uint8_t r, std::uint8_t g, std::uint8_t b,
                                std::uint8_t a = 255) {
    return {r, g, b, a};
}

Effect lutEffect(std::string path) {
    Effect fx;
    fx.id = Uuid::generateV4();
    fx.type = EffectType::Lut;
    fx.resourcePath = std::move(path);
    return fx;
}

// --- The cache's answers ----------------------------------------------------

TEST(LutCache, AGoodFileIsParsedOnceAndRememberedRatherThanReReadPerFrame) {
    LutCache cache;
    int reads = 0;
    cache.setReader([&](const std::string&) -> Result<std::string> {
        ++reads;
        return ok(identityCubeText(2));
    });

    const LutLookup& first = cache.lookup("/luts/a.cube");
    ASSERT_TRUE(first.ok()) << first.failure;
    EXPECT_TRUE(first.table.isIdentity());
    EXPECT_EQ(reads, 1);

    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(cache.lookup("/luts/a.cube").ok());
    }
    EXPECT_EQ(reads, 1) << "a render loop must not re-read the file every frame";
    EXPECT_EQ(cache.readCount(), 1u);
    EXPECT_TRUE(cache.contains("/luts/a.cube"));
}

// The heart of Requirement 7.8: a missing file yields an EMPTY TABLE and a REPORTED
// FAILURE, not an error the render loop has to decide about. And it is remembered, so a
// missing LUT costs one failed read rather than one per frame.
TEST(LutCache, AMissingFileYieldsAnEmptyTableAndAFailureNamingThePath) {
    LutCache cache;
    int reads = 0;
    cache.setReader([&](const std::string& path) -> Result<std::string> {
        ++reads;
        return err<std::string>(notFound("LUT file cannot be read: " + path));
    });

    const LutLookup& lookup = cache.lookup("/luts/gone.cube");
    EXPECT_FALSE(lookup.ok());
    EXPECT_TRUE(lookup.table.empty()) << "un-graded means NO table, not a black one";
    EXPECT_NE(lookup.failure.find("/luts/gone.cube"), std::string::npos)
        << "Requirement 7.8: the report must name the path -- got: " << lookup.failure;

    cache.lookup("/luts/gone.cube");
    cache.lookup("/luts/gone.cube");
    EXPECT_EQ(reads, 1) << "a remembered failure must not be retried per frame";

    const auto failures = cache.failures();
    ASSERT_EQ(failures.size(), 1u);
    EXPECT_EQ(failures.begin()->first, "/luts/gone.cube");
}

TEST(LutCache, AMalformedFileFailsWithBothTheFaultAndThePath) {
    LutCache cache;
    cache.setReader([](const std::string&) -> Result<std::string> {
        return ok(std::string("LUT_3D_SIZE 2\n0.0 0.0 0.0\n"));  // 1 row, needs 8
    });

    const LutLookup& lookup = cache.lookup("/luts/truncated.cube");
    EXPECT_FALSE(lookup.ok());
    EXPECT_TRUE(lookup.table.empty());
    // The parser names the fault and the cache adds the path, because a pure function of
    // text cannot know where the text came from.
    EXPECT_NE(lookup.failure.find("needs 8 data rows"), std::string::npos) << lookup.failure;
    EXPECT_NE(lookup.failure.find("/luts/truncated.cube"), std::string::npos) << lookup.failure;
}

// "No LUT chosen yet" must not raise the same notice as "your LUT is missing". Conflating
// them would report a problem the user does not have, on every effect they have not
// finished configuring.
TEST(LutCache, AnEmptyPathIsNotAFailureAndIsNotReported) {
    LutCache cache;
    cache.setReader([](const std::string&) -> Result<std::string> {
        ADD_FAILURE() << "an empty path must not be read at all";
        return err<std::string>(notFound("unreachable"));
    });

    const LutLookup& lookup = cache.lookup("");
    EXPECT_TRUE(lookup.ok());
    EXPECT_TRUE(lookup.table.empty());
    EXPECT_TRUE(cache.failures().empty());
    EXPECT_EQ(cache.readCount(), 0u);
}

TEST(LutCache, ForgettingAPathLetsAReplacedFileBeReRead) {
    LutCache cache;
    std::string served = identityCubeText(2);
    int reads = 0;
    cache.setReader([&](const std::string&) -> Result<std::string> {
        ++reads;
        return ok(served);
    });

    ASSERT_TRUE(cache.lookup("/luts/a.cube").table.isIdentity());
    served = halvingCubeText();
    EXPECT_TRUE(cache.lookup("/luts/a.cube").table.isIdentity())
        << "still the cached answer until told otherwise";

    cache.forget("/luts/a.cube");
    EXPECT_FALSE(cache.contains("/luts/a.cube"));
    EXPECT_FALSE(cache.lookup("/luts/a.cube").table.isIdentity());
    EXPECT_EQ(reads, 2);
}

TEST(LutCache, InstallingAReaderDiscardsWhatWasRememberedUnderTheOldOne) {
    LutCache cache;
    cache.setReader([](const std::string&) -> Result<std::string> {
        return ok(identityCubeText(2));
    });
    ASSERT_TRUE(cache.lookup("/luts/a.cube").ok());
    ASSERT_EQ(cache.size(), 1u);

    // The point of installing a reader is to read something different, so keeping the old
    // answers would silently ignore it.
    cache.setReader([](const std::string&) -> Result<std::string> {
        return ok(halvingCubeText());
    });
    EXPECT_EQ(cache.size(), 0u);
    EXPECT_FALSE(cache.lookup("/luts/a.cube").table.isIdentity());
}

TEST(LutCache, TheRealReaderReportsAMissingPathByNameWithoutThrowing) {
    // The one case that touches a filesystem, and only to confirm that a path which cannot
    // exist is reported rather than raising.
    const auto result = readFileForLut("/definitely/not/here/palmier-test.cube");
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::NotFound);
    EXPECT_NE(result.error().message().find("palmier-test.cube"), std::string::npos)
        << result.error().message();
    EXPECT_TRUE(readFileForLut("").isError()) << "an empty path is rejected, not opened";
}

// --- The effect, end to end through the software path (7.6, 7.8) ------------

TEST(SoftwareLut, AnEffectWithNoResourcePathLeavesEveryPixelUntouched) {
    auto px = pixel(10, 128, 250, 77);
    const auto before = px;
    applyEffectSoftware(lutEffect(""), px.data(), 1, 1);
    EXPECT_EQ(px, before) << "no LUT chosen is a no-op, not a black frame";
}

TEST(SoftwareLut, AMissingLutRendersUnGradedRatherThanBlack) {
    sharedLutCache().setReader([](const std::string& path) -> Result<std::string> {
        return err<std::string>(notFound("LUT file cannot be read: " + path));
    });

    auto px = pixel(10, 128, 250, 77);
    const auto before = px;
    applyEffectSoftware(lutEffect("/luts/gone.cube"), px.data(), 1, 1);
    EXPECT_EQ(px, before) << "Requirement 7.8: un-graded, and emphatically not black";

    // And the failure is available to report, naming the path.
    const auto failures = sharedLutCache().failures();
    ASSERT_EQ(failures.count("/luts/gone.cube"), 1u);
    sharedLutCache().setReader({});
    sharedLutCache().clear();
}

TEST(SoftwareLut, AnIdentityLutIsANoOpWithinOneLeastSignificantBit) {
    sharedLutCache().setReader([](const std::string&) -> Result<std::string> {
        return ok(identityCubeText(17));
    });

    // Every byte value, so a domain-scaling error at one end is caught rather than sampled
    // around. Requirement 7.7's anchor, asserted through the real render path.
    std::vector<std::uint8_t> px(256u * 4u);
    for (std::size_t v = 0; v < 256; ++v) {
        px[v * 4 + 0] = px[v * 4 + 1] = px[v * 4 + 2] = static_cast<std::uint8_t>(v);
        px[v * 4 + 3] = 255;
    }
    const auto before = px;
    applyEffectSoftware(lutEffect("/luts/identity.cube"), px.data(), 256, 1);

    for (std::size_t v = 0; v < 256; ++v) {
        for (std::size_t ch = 0; ch < 3; ++ch) {
            const int got = px[v * 4 + ch];
            const int want = before[v * 4 + ch];
            EXPECT_LE(std::abs(got - want), 1) << "byte " << v << " channel " << ch;
        }
        EXPECT_EQ(px[v * 4 + 3], 255) << "alpha is not a colour channel";
    }
    sharedLutCache().setReader({});
    sharedLutCache().clear();
}

TEST(SoftwareLut, AHalvingLutActuallyHalvesAndPreservesAlpha) {
    sharedLutCache().setReader([](const std::string&) -> Result<std::string> {
        return ok(halvingCubeText());
    });

    auto px = pixel(255, 128, 0, 200);
    applyEffectSoftware(lutEffect("/luts/half.cube"), px.data(), 1, 1);
    EXPECT_NEAR(px[0], 128, 1) << "255 halved";
    EXPECT_NEAR(px[1], 64, 1) << "128 halved";
    EXPECT_EQ(px[2], 0);
    EXPECT_EQ(px[3], 200) << "alpha preserved";

    sharedLutCache().setReader({});
    sharedLutCache().clear();
}

TEST(SoftwareLut, TheEffectTypeIsWiredToItsOwnKernelAndName) {
    ASSERT_TRUE(kernelForEffectType(EffectType::Lut).has_value());
    EXPECT_EQ(*kernelForEffectType(EffectType::Lut), EffectKernel::Lut);
    EXPECT_EQ(effectTypeForKernel(EffectKernel::Lut), EffectType::Lut);
    EXPECT_EQ(effectKernelName(EffectKernel::Lut), "lut");

    const std::string src{effectKernelSource(EffectKernel::Lut)};
    EXPECT_NE(src.find("#version 450"), std::string::npos);
    // Eight explicit texel reads and seven mixes, written out rather than delegated to a
    // hardware sampler, because a sampler's filtering precision is a device detail while
    // property P5's tolerance is not.
    EXPECT_NE(src.find("lutTexel"), std::string::npos);
    EXPECT_NE(src.find("clamp("), std::string::npos) << "the domain must be clamped";
}

}  // namespace
}  // namespace palmier::gpu
