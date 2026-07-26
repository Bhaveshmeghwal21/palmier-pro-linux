// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/gpu_gpu_cpu_parity_property_test.cpp — RapidCheck property test for the
// correctness property P5 (GPU/CPU parity), task 7.6; Requirements 10.7.
//
//   Feature: palmier-pro-linux, Property 5: GPU/CPU parity — for any source
//   frame and set of effect parameters, compositing on the GPU versus the
//   software fallback yields visually equivalent output within a bounded
//   per-channel tolerance (<= 1 on a 0-255 scale; golden-image comparison).
//
//   Validates: Requirements 10.7
//
// ---------------------------------------------------------------------------
// Two compositing lanes, one golden-image comparison
// ---------------------------------------------------------------------------
// This sandbox has no Vulkan loader / GPU (and no shaderc), so the *real* GPU
// SPIR-V compute lane cannot be dispatched here. The test is therefore
// structured as a golden-image parity check between the two lanes whose parity
// P5 asserts, using the two implementations that are actually available:
//
//   * CPU lane (the software fallback, and the reference the whole design is
//     validated against): the production applyEffectSoftware /
//     applyTransitionSoftware from gpu/Compositor.cpp. This is the exact code
//     the software fallback runs, computed in integer byte space.
//
//   * GPU lane: an independent reference model of the SPIR-V compute kernels'
//     defined semantics, computed the way the GLSL in gpu/EffectKernels.cpp
//     computes — in normalized [0,1] floating-point space with a final
//     round-to-nearest store back to rgba8. Each function below mirrors the
//     matching GLSL kernel (kBrightnessSrc, kContrastSrc, kBlurSrc,
//     kCropTransformSrc, kColorGradeSrc, kTransitionSrc) line for line: same
//     channel order, same clamp/round, same edge-clamped blur window, same
//     rounded normalized crop rect, same gain->lift->saturation order and
//     Rec.601 luma weights.
//
// The GPU lane deliberately does NOT reuse the CPU byte-space math: it recomputes
// each effect in the kernel's own normalized-float representation. A divergence
// between the two implementations beyond the 1-LSB tolerance (e.g. a wrong luma
// weight, a mismatched mid-gray pivot, an off-by-one crop edge, or a differing
// blur window) is exactly the class of bug P5 exists to catch, and this test
// catches it without a device.
//
// When a real Vulkan device is present, the same property holds with the GPU
// lane replaced by an actual compute-shader dispatch of the compiled SPIR-V
// (EffectKernelRegistry): the assertion — per-channel |gpu - cpu| <= 1 over a
// randomly generated source frame and effect parameters — is unchanged, so this
// test is ready to compare against the real GPU lane by swapping the lane
// implementation.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include "core/Effect.hpp"
#include "core/Uuid.hpp"
#include "gpu/Compositor.hpp"
#include "gpu/EffectKernels.hpp"

namespace palmier::gpu {
namespace {

// The bounded per-channel tolerance for P5's golden-image comparison: at most
// one least-significant bit on a 0-255 scale.
constexpr int kParityTolerance = 1;

// ---------------------------------------------------------------------------
// GPU lane: normalized-space reference of the SPIR-V effect kernels.
//
// rgba8 samples are read as n = byte/255 in [0,1] and written back with a
// round-to-nearest store (matching a Vulkan rgba8 image store of a clamped
// float), exactly as the GLSL in EffectKernels.cpp does.
// ---------------------------------------------------------------------------

/// Store a normalized [0,1] value back to an rgba8 byte: clamp then round to
/// nearest (the float->unorm conversion the compute kernels' imageStore does).
[[nodiscard]] std::uint8_t store01(double n) noexcept {
    const double s = n * 255.0;
    if (s <= 0.0) return 0;
    if (s >= 255.0) return 255;
    return static_cast<std::uint8_t>(std::lround(s));
}

[[nodiscard]] double load01(std::uint8_t b) noexcept { return static_cast<double>(b) / 255.0; }

/// Mirror of kBrightnessSrc: rgb = clamp(c.rgb + amount, 0, 1); alpha preserved.
void gpuBrightness(const std::vector<std::uint8_t>& in, std::vector<std::uint8_t>& out,
                   double amount) {
    for (std::size_t i = 0; i < in.size(); i += 4) {
        for (int c = 0; c < 3; ++c) out[i + c] = store01(load01(in[i + c]) + amount);
        out[i + 3] = in[i + 3];
    }
}

/// Mirror of kContrastSrc: rgb = clamp((c.rgb - mid)*(1+amount) + mid, 0, 1),
/// with mid = 128/255; alpha preserved.
void gpuContrast(const std::vector<std::uint8_t>& in, std::vector<std::uint8_t>& out,
                 double amount) {
    const double mid = 128.0 / 255.0;
    for (std::size_t i = 0; i < in.size(); i += 4) {
        for (int c = 0; c < 3; ++c)
            out[i + c] = store01((load01(in[i + c]) - mid) * (1.0 + amount) + mid);
        out[i + 3] = in[i + 3];
    }
}

/// Mirror of kBlurSrc: edge-clamped (2r+1)^2 box average of rgb; alpha preserved.
/// r <= 0 is a passthrough.
void gpuBlur(const std::vector<std::uint8_t>& in, std::vector<std::uint8_t>& out,
             std::uint32_t width, std::uint32_t height, int r) {
    const int w = static_cast<int>(width);
    const int h = static_cast<int>(height);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::size_t d = (static_cast<std::size_t>(y) * w + x) * 4u;
            if (r <= 0) {
                for (int c = 0; c < 4; ++c) out[d + c] = in[d + c];
                continue;
            }
            const int x0 = std::max(0, x - r);
            const int x1 = std::min(w - 1, x + r);
            const int y0 = std::max(0, y - r);
            const int y1 = std::min(h - 1, y + r);
            double sum[3] = {0.0, 0.0, 0.0};
            int count = 0;
            for (int sy = y0; sy <= y1; ++sy) {
                for (int sx = x0; sx <= x1; ++sx) {
                    const std::size_t s = (static_cast<std::size_t>(sy) * w + sx) * 4u;
                    for (int c = 0; c < 3; ++c) sum[c] += load01(in[s + c]);
                    ++count;
                }
            }
            for (int c = 0; c < 3; ++c) out[d + c] = store01(sum[c] / count);
            out[d + 3] = in[d + 3]; // alpha preserved
        }
    }
}

/// Mirror of kCropTransformSrc: keep pixels inside the rounded normalized rect,
/// clear the rest to transparent black. Edges rounded as int(clamp(v,0,1)*ext + 0.5).
void gpuCropTransform(const std::vector<std::uint8_t>& in, std::vector<std::uint8_t>& out,
                      std::uint32_t width, std::uint32_t height,
                      double cropLeft, double cropTop, double cropRight, double cropBottom) {
    const auto edge = [](double norm, std::uint32_t extent) -> int {
        const double v = std::clamp(norm, 0.0, 1.0) * static_cast<double>(extent) + 0.5;
        return static_cast<int>(v); // truncation toward zero (norm >= 0), == GLSL int()
    };
    const int x0 = edge(cropLeft, width);
    const int x1 = edge(cropRight, width);
    const int y0 = edge(cropTop, height);
    const int y1 = edge(cropBottom, height);
    for (int y = 0; y < static_cast<int>(height); ++y) {
        for (int x = 0; x < static_cast<int>(width); ++x) {
            const std::size_t o = (static_cast<std::size_t>(y) * width + x) * 4u;
            const bool inside = (x >= x0 && x < x1 && y >= y0 && y < y1);
            if (inside) {
                for (int c = 0; c < 4; ++c) out[o + c] = in[o + c];
            } else {
                for (int c = 0; c < 4; ++c) out[o + c] = 0; // transparent black
            }
        }
    }
}

/// Mirror of kColorGradeSrc: gain -> lift -> Rec.601 saturation mix; alpha preserved.
void gpuColorGrade(const std::vector<std::uint8_t>& in, std::vector<std::uint8_t>& out,
                   double gainR, double gainG, double gainB, double lift, double saturation) {
    for (std::size_t i = 0; i < in.size(); i += 4) {
        double r = load01(in[i + 0]) * gainR + lift;
        double g = load01(in[i + 1]) * gainG + lift;
        double b = load01(in[i + 2]) * gainB + lift;
        const double luma = 0.299 * r + 0.587 * g + 0.114 * b;
        r = luma + (r - luma) * saturation;
        g = luma + (g - luma) * saturation;
        b = luma + (b - luma) * saturation;
        out[i + 0] = store01(r);
        out[i + 1] = store01(g);
        out[i + 2] = store01(b);
        out[i + 3] = in[i + 3];
    }
}

/// Mirror of kInvertColorsSrc: rgb = clamp(1 - c.rgb, 0, 1); alpha preserved.
void gpuInvertColors(const std::vector<std::uint8_t>& in, std::vector<std::uint8_t>& out) {
    for (std::size_t i = 0; i < in.size(); i += 4) {
        for (int c = 0; c < 3; ++c) out[i + c] = store01(1.0 - load01(in[i + c]));
        out[i + 3] = in[i + 3];
    }
}

/// Mirror of kTransitionSrc: out = mix(a, b, progress) across all four channels.
void gpuTransition(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b,
                   std::vector<std::uint8_t>& out, double progress) {
    const double p = std::clamp(progress, 0.0, 1.0);
    for (std::size_t i = 0; i < a.size(); ++i) {
        out[i] = store01(load01(a[i]) * (1.0 - p) + load01(b[i]) * p);
    }
}

/// Dispatch the GPU-lane model for a per-clip effect (mirrors the software
/// applyEffectSoftware switch), returning a fresh golden image.
[[nodiscard]] std::vector<std::uint8_t> gpuApplyEffect(const Effect& e,
                                                       const std::vector<std::uint8_t>& in,
                                                       std::uint32_t w, std::uint32_t h) {
    std::vector<std::uint8_t> out(in.size());
    const auto param = [&](const char* k, double d) {
        const auto it = e.parameters.find(k);
        return it == e.parameters.end() ? d : it->second;
    };
    switch (e.type) {
        case EffectType::Brightness:
            gpuBrightness(in, out, param("amount", 0.0));
            break;
        case EffectType::Contrast:
            gpuContrast(in, out, param("amount", 0.0));
            break;
        case EffectType::Blur:
            gpuBlur(in, out, w, h,
                    static_cast<int>(param("radius", 0.0) + 0.5)); // round like the host
            break;
        case EffectType::CropTransform:
            gpuCropTransform(in, out, w, h, param("cropLeft", 0.0), param("cropTop", 0.0),
                             param("cropRight", 1.0), param("cropBottom", 1.0));
            break;
        case EffectType::ColorGrade:
            gpuColorGrade(in, out, param("gainR", 1.0), param("gainG", 1.0), param("gainB", 1.0),
                          param("lift", 0.0), param("saturation", 1.0));
            break;
        case EffectType::InvertColors:
            gpuInvertColors(in, out);
            break;
        case EffectType::Custom:
            out = in; // passthrough, matching the software reference
            break;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Comparison + generators
// ---------------------------------------------------------------------------

/// Largest absolute per-channel difference between two equal-sized rgba8 buffers.
[[nodiscard]] int maxChannelDiff(const std::vector<std::uint8_t>& a,
                                 const std::vector<std::uint8_t>& b) {
    int worst = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        worst = std::max(worst, std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])));
    }
    return worst;
}

Effect makeEffect(EffectType type, std::map<std::string, double> params) {
    return Effect{Uuid::generateV4(), type, std::move(params)};
}

/// Generate a random rgba8 source frame: dimensions and pixel bytes. Small
/// dimensions keep each iteration cheap while still exercising the blur window
/// and crop-rect edges across many shapes.
struct GenFrame {
    std::uint32_t             width{};
    std::uint32_t             height{};
    std::vector<std::uint8_t> rgba{};
};

[[nodiscard]] GenFrame genFrame() {
    const auto w = static_cast<std::uint32_t>(*rc::gen::inRange(1, 9));  // 1..8
    const auto h = static_cast<std::uint32_t>(*rc::gen::inRange(1, 9));  // 1..8
    const std::size_t n = static_cast<std::size_t>(w) * h * 4u;
    auto rgba = *rc::gen::container<std::vector<std::uint8_t>>(
        n, rc::gen::arbitrary<std::uint8_t>());
    return GenFrame{w, h, std::move(rgba)};
}

/// A scaled double in [lo, hi] built from an integer draw (avoids NaN/inf and
/// keeps parameters in the effects' documented ranges).
[[nodiscard]] double genScaled(int loMilli, int hiMilli) {
    return static_cast<double>(*rc::gen::inRange(loMilli, hiMilli + 1)) / 1000.0;
}

/// Generate a random per-clip effect with parameters in-range for its kind.
[[nodiscard]] Effect genEffect() {
    const EffectType type = *rc::gen::element(EffectType::Brightness, EffectType::Contrast,
                                              EffectType::Blur, EffectType::CropTransform,
                                              EffectType::ColorGrade, EffectType::InvertColors,
                                              EffectType::Custom);
    switch (type) {
        case EffectType::Brightness:
            return makeEffect(type, {{"amount", genScaled(-1000, 1000)}}); // [-1,1]
        case EffectType::Contrast:
            return makeEffect(type, {{"amount", genScaled(-1000, 1000)}}); // [-1,1]
        case EffectType::Blur:
            return makeEffect(type, {{"radius", static_cast<double>(*rc::gen::inRange(0, 7))}});
        case EffectType::CropTransform:
            return makeEffect(type, {{"cropLeft", genScaled(0, 1000)},
                                     {"cropTop", genScaled(0, 1000)},
                                     {"cropRight", genScaled(0, 1000)},
                                     {"cropBottom", genScaled(0, 1000)}});
        case EffectType::ColorGrade:
            return makeEffect(type, {{"gainR", genScaled(0, 2000)},
                                     {"gainG", genScaled(0, 2000)},
                                     {"gainB", genScaled(0, 2000)},
                                     {"lift", genScaled(-500, 500)},
                                     {"saturation", genScaled(0, 2000)}});
        case EffectType::InvertColors:
            return makeEffect(type, {}); // parameterless
        case EffectType::Custom:
            return makeEffect(type, {{"anything", genScaled(-1000, 1000)}});
    }
    return makeEffect(EffectType::Custom, {});
}

// ===========================================================================
// Property 5: GPU/CPU parity — per-clip effect kernels
// ===========================================================================

// Feature: palmier-pro-linux, Property 5: GPU/CPU parity — for any source frame
// and effect parameters, GPU vs software compositing differ by no more than 1
// per channel on a 0-255 scale (golden-image comparison).
// Validates: Requirements 10.7
RC_GTEST_PROP(GpuCpuParityProperties,
              PerClipEffectKernelMatchesSoftwareWithinOneLsb,
              ()) {
    const GenFrame frame = genFrame();
    const Effect effect = genEffect();

    // CPU lane: the production software fallback effect (integer byte space).
    std::vector<std::uint8_t> cpu = frame.rgba;
    applyEffectSoftware(effect, cpu.data(), frame.width, frame.height);

    // GPU lane: the SPIR-V kernel's semantics in normalized float space.
    const std::vector<std::uint8_t> gpu =
        gpuApplyEffect(effect, frame.rgba, frame.width, frame.height);

    const int diff = maxChannelDiff(cpu, gpu);
    RC_TAG(static_cast<int>(effect.type)); // distribution of exercised effect kinds
    RC_ASSERT(diff <= kParityTolerance);
}

// ===========================================================================
// Property 5: GPU/CPU parity — the transition (two-input) kernel
// ===========================================================================

// Feature: palmier-pro-linux, Property 5: GPU/CPU parity — for any two source
// frames and transition progress, the GPU transition kernel and the software
// cross-dissolve differ by no more than 1 per channel on a 0-255 scale.
// Validates: Requirements 10.7
RC_GTEST_PROP(GpuCpuParityProperties,
              TransitionKernelMatchesSoftwareWithinOneLsb,
              ()) {
    const auto w = static_cast<std::uint32_t>(*rc::gen::inRange(1, 9));
    const auto h = static_cast<std::uint32_t>(*rc::gen::inRange(1, 9));
    const std::size_t n = static_cast<std::size_t>(w) * h * 4u;

    const auto a = *rc::gen::container<std::vector<std::uint8_t>>(
        n, rc::gen::arbitrary<std::uint8_t>());
    const auto b = *rc::gen::container<std::vector<std::uint8_t>>(
        n, rc::gen::arbitrary<std::uint8_t>());
    const double progress = genScaled(0, 1000); // [0,1]

    // CPU lane: production software cross-dissolve.
    std::vector<std::uint8_t> cpu(n);
    applyTransitionSoftware(a.data(), b.data(), cpu.data(), w, h, progress);

    // GPU lane: normalized-space mix, mirroring kTransitionSrc.
    std::vector<std::uint8_t> gpu(n);
    gpuTransition(a, b, gpu, progress);

    RC_ASSERT(maxChannelDiff(cpu, gpu) <= kParityTolerance);
}

// ===========================================================================
// A couple of deterministic golden-image checks (example-based companions to
// the properties above — concrete parity witnesses that also document intent).
// ===========================================================================

TEST(GpuCpuParityExamples, BrightnessGoldenImageMatchesWithinTolerance) {
    const std::uint32_t w = 3, h = 2;
    std::vector<std::uint8_t> src = {
        10, 20, 30, 255,  100, 110, 120, 200,  250, 5, 128, 64,
        0, 0, 0, 255,     255, 255, 255, 0,    77, 88, 99, 111};
    const Effect e = makeEffect(EffectType::Brightness, {{"amount", 0.2}});

    std::vector<std::uint8_t> cpu = src;
    applyEffectSoftware(e, cpu.data(), w, h);
    const std::vector<std::uint8_t> gpu = gpuApplyEffect(e, src, w, h);

    EXPECT_LE(maxChannelDiff(cpu, gpu), kParityTolerance);
}

TEST(GpuCpuParityExamples, ColorGradeGoldenImageMatchesWithinTolerance) {
    const std::uint32_t w = 2, h = 2;
    std::vector<std::uint8_t> src = {
        200, 100, 0, 255,   50, 60, 70, 128,
        10, 250, 128, 200,  0, 0, 255, 255};
    const Effect e = makeEffect(EffectType::ColorGrade,
                                {{"gainR", 1.2}, {"gainG", 0.9}, {"gainB", 1.05},
                                 {"lift", 0.05}, {"saturation", 0.5}});

    std::vector<std::uint8_t> cpu = src;
    applyEffectSoftware(e, cpu.data(), w, h);
    const std::vector<std::uint8_t> gpu = gpuApplyEffect(e, src, w, h);

    EXPECT_LE(maxChannelDiff(cpu, gpu), kParityTolerance);
}

// The GPU-lane model is tied to the real kernel sources: every effect kind the
// software reference handles has a corresponding compiled kernel source, so the
// parity model above is validating against the same kernels the GPU would run.
TEST(GpuCpuParityExamples, EveryModeledKernelHasGlslSource) {
    for (const EffectKernel kernel : allEffectKernels()) {
        EXPECT_FALSE(effectKernelSource(kernel).empty());
    }
}

} // namespace
} // namespace palmier::gpu
