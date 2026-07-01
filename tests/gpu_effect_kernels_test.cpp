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

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "core/Effect.hpp"
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

TEST(SoftwareEffect, CustomEffectIsPassThrough) {
    auto px = solid(2, 2, 11, 22, 33, 44);
    const auto before = px;
    applyEffectSoftware(makeEffect(EffectType::Custom, {{"anything", 1.0}}),
                        px.data(), 2, 2);
    EXPECT_EQ(px, before);
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
    // The five per-clip effect kinds map both ways.
    const EffectType types[] = {EffectType::Brightness, EffectType::Contrast,
                                EffectType::Blur, EffectType::CropTransform,
                                EffectType::ColorGrade};
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
    EXPECT_EQ(registry.size(), allEffectKernels().size()); // all 6 compiled

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

    // The five EffectType kernels register; Transition does not (no EffectType).
    EXPECT_EQ(registered, 5u);
    EXPECT_EQ(comp.registeredEffectCount(), 5u);
    EXPECT_TRUE(comp.isEffectRegistered(EffectType::Brightness));
    EXPECT_TRUE(comp.isEffectRegistered(EffectType::Blur));
    EXPECT_TRUE(comp.isEffectRegistered(EffectType::CropTransform));
    EXPECT_TRUE(comp.isEffectRegistered(EffectType::ColorGrade));
    EXPECT_TRUE(comp.isEffectRegistered(EffectType::Contrast));
}

} // namespace
} // namespace palmier::gpu
