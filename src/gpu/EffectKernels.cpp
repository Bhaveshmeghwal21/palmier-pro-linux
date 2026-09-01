// SPDX-License-Identifier: GPL-3.0-or-later
//
// gpu/EffectKernels.cpp — GLSL compute sources for the built-in effect kernels,
// their shaderc-based SPIR-V compilation, and the registry that installs them on
// a Compositor (design.md "Effects as SPIR-V compute kernels"; Requirements 10.2
// and 10.7, task 7.4).
//
// Each GLSL kernel below mirrors the matching branch of applyEffectSoftware in
// Compositor.cpp: rgba8 images are sampled/stored in normalized [0,1] space, so
// e.g. brightness adds `amount` (== amount*255 / 255), contrast scales about
// 128/255, color grade applies gain then lift then a luma-mix saturation, blur
// averages the same edge-clamped (2r+1)^2 window, and crop clears pixels outside
// the same rounded normalized rect. That keeps the GPU output within property
// P5's per-channel tolerance of the software reference.
//
// Compilation uses shaderc (a real project dependency). The whole file builds
// without shaderc too: shadercAvailable() reports false and the compile entry
// points return an Unsupported error, so the compositor simply keeps using the
// software effect path (the guard pattern shared with the rest of the GPU layer).

#include "gpu/EffectKernels.hpp"

#include <utility>

#include "core/Error.hpp"

#if defined(PALMIER_HAVE_SHADERC)
#include <shaderc/shaderc.hpp>
#endif

namespace palmier::gpu {
namespace {

// --- GLSL compute sources ---------------------------------------------------
//
// Common layout across kernels: input image at binding 0, output image at the
// last binding, parameters in a push_constant block, one invocation per pixel
// with an out-of-bounds guard. The math is the normalized-space twin of the
// host-memory reference in Compositor.cpp.

constexpr std::string_view kBrightnessSrc = R"glsl(#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(binding = 0, rgba8) uniform readonly  image2D inImage;
layout(binding = 1, rgba8) uniform writeonly image2D outImage;
layout(push_constant) uniform Params { float amount; } pc;
void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(inImage);
    if (p.x >= size.x || p.y >= size.y) return;
    vec4 c = imageLoad(inImage, p);
    // amount*255 in byte space == amount in normalized [0,1] space.
    vec3 rgb = clamp(c.rgb + vec3(pc.amount), 0.0, 1.0);
    imageStore(outImage, p, vec4(rgb, c.a));
}
)glsl";

constexpr std::string_view kContrastSrc = R"glsl(#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(binding = 0, rgba8) uniform readonly  image2D inImage;
layout(binding = 1, rgba8) uniform writeonly image2D outImage;
layout(push_constant) uniform Params { float amount; } pc;
void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(inImage);
    if (p.x >= size.x || p.y >= size.y) return;
    const float mid = 128.0 / 255.0;   // match the CPU reference's mid-gray (128)
    vec4 c = imageLoad(inImage, p);
    vec3 rgb = clamp((c.rgb - vec3(mid)) * (1.0 + pc.amount) + vec3(mid), 0.0, 1.0);
    imageStore(outImage, p, vec4(rgb, c.a));
}
)glsl";

constexpr std::string_view kBlurSrc = R"glsl(#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(binding = 0, rgba8) uniform readonly  image2D inImage;
layout(binding = 1, rgba8) uniform writeonly image2D outImage;
layout(push_constant) uniform Params { int radius; } pc;
void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(inImage);
    if (p.x >= size.x || p.y >= size.y) return;
    vec4 c = imageLoad(inImage, p);
    int r = pc.radius;
    if (r <= 0) { imageStore(outImage, p, c); return; }
    int x0 = max(0, p.x - r);
    int x1 = min(size.x - 1, p.x + r);
    int y0 = max(0, p.y - r);
    int y1 = min(size.y - 1, p.y + r);
    vec3 sum = vec3(0.0);
    int count = 0;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            sum += imageLoad(inImage, ivec2(x, y)).rgb;
            count += 1;
        }
    }
    vec3 avg = sum / float(count);
    imageStore(outImage, p, vec4(avg, c.a)); // alpha preserved
}
)glsl";

constexpr std::string_view kCropTransformSrc = R"glsl(#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(binding = 0, rgba8) uniform readonly  image2D inImage;
layout(binding = 1, rgba8) uniform writeonly image2D outImage;
layout(push_constant) uniform Params {
    float cropLeft; float cropTop; float cropRight; float cropBottom;
} pc;
void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(inImage);
    if (p.x >= size.x || p.y >= size.y) return;
    int x0 = int(clamp(pc.cropLeft,   0.0, 1.0) * float(size.x) + 0.5);
    int x1 = int(clamp(pc.cropRight,  0.0, 1.0) * float(size.x) + 0.5);
    int y0 = int(clamp(pc.cropTop,    0.0, 1.0) * float(size.y) + 0.5);
    int y1 = int(clamp(pc.cropBottom, 0.0, 1.0) * float(size.y) + 0.5);
    bool inside = (p.x >= x0 && p.x < x1 && p.y >= y0 && p.y < y1);
    vec4 c = imageLoad(inImage, p);
    imageStore(outImage, p, inside ? c : vec4(0.0)); // outside -> transparent black
}
)glsl";

// Lift / gamma / gain primary grade (monitoring-and-grading Requirement 4).
//
// ORDER OF OPERATIONS — gain, then lift, then gamma, then saturation
// (Requirement 4.5). Fixed and stated here because the software reference in
// gpu::applyColorGrade mirrors it line for line and the two must not drift; any
// other order produces a visibly different image from the same nine numbers.
//
// The gamma step is SKIPPED ENTIRELY when all three exponents are 1.0, and that is
// a correctness requirement rather than an optimisation (Requirement 4.2). Gamma is
// meaningless on a negative value — a negative lift can easily produce one, and
// pow() is undefined there — so the step clamps its input up to 0 first. At an
// exponent of 1.0 that clamp would still be observable, because saturation below 1
// mixes toward a luma computed from the UNCLAMPED values: an existing project with
// a negative lift would shift. Guarding the whole step keeps the default path
// bit-for-bit what it was before this kernel gained gamma at all.
//
// Gamma is applied as pow(x, 1/gamma), so a gamma ABOVE 1 brightens midtones, which
// is the direction every colour tool's midtone control moves. The exponent is
// clamped away from zero so a gamma of 0 cannot divide by it.
constexpr std::string_view kColorGradeSrc = R"glsl(#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(binding = 0, rgba8) uniform readonly  image2D inImage;
layout(binding = 1, rgba8) uniform writeonly image2D outImage;
layout(push_constant) uniform Params {
    float gainR;  float gainG;  float gainB;
    float liftR;  float liftG;  float liftB;
    float gammaR; float gammaG; float gammaB;
    float saturation;
} pc;
void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(inImage);
    if (p.x >= size.x || p.y >= size.y) return;
    vec4 c = imageLoad(inImage, p);
    vec3 rgb = c.rgb * vec3(pc.gainR, pc.gainG, pc.gainB)
                     + vec3(pc.liftR, pc.liftG, pc.liftB);
    vec3 gamma = vec3(pc.gammaR, pc.gammaG, pc.gammaB);
    if (gamma != vec3(1.0)) {
        rgb = pow(max(rgb, vec3(0.0)), vec3(1.0) / max(gamma, vec3(1.0 / 1024.0)));
    }
    float luma = dot(rgb, vec3(0.299, 0.587, 0.114)); // Rec.601
    rgb = mix(vec3(luma), rgb, pc.saturation);
    rgb = clamp(rgb, 0.0, 1.0);
    imageStore(outImage, p, vec4(rgb, c.a));
}
)glsl";

// Invert colors (upstream PR 408; Requirements 14.4): rgb = 1 - rgb, alpha kept.
// In byte space that is exactly 255 - value, which is what the software
// reference computes: 1.0 - n stored back through the rgba8 round-to-nearest
// conversion yields round(255 - value) == 255 - value for every input byte, so
// the two paths agree exactly (well inside property P5's 1-of-255 tolerance).
// The kernel takes no parameters; the push-constant block is a reserved
// placeholder so every kernel keeps the same dispatch signature.
constexpr std::string_view kInvertColorsSrc = R"glsl(#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(binding = 0, rgba8) uniform readonly  image2D inImage;
layout(binding = 1, rgba8) uniform writeonly image2D outImage;
layout(push_constant) uniform Params { float reserved; } pc;
void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(inImage);
    if (p.x >= size.x || p.y >= size.y) return;
    vec4 c = imageLoad(inImage, p);
    // 255 - value in byte space == 1.0 - value in normalized [0,1] space.
    vec3 rgb = clamp(vec3(1.0) - c.rgb, 0.0, 1.0);
    imageStore(outImage, p, vec4(rgb, c.a)); // alpha unchanged
}
)glsl";

constexpr std::string_view kTransitionSrc = R"glsl(#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(binding = 0, rgba8) uniform readonly  image2D inA;
layout(binding = 1, rgba8) uniform readonly  image2D inB;
layout(binding = 2, rgba8) uniform writeonly image2D outImage;
layout(push_constant) uniform Params { float progress; } pc;
void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(inA);
    if (p.x >= size.x || p.y >= size.y) return;
    vec4 a = imageLoad(inA, p);
    vec4 b = imageLoad(inB, p);
    vec4 o = mix(a, b, clamp(pc.progress, 0.0, 1.0)); // cross-dissolve, alpha too
    imageStore(outImage, p, o);
}
)glsl";

} // namespace

std::string_view effectKernelName(EffectKernel kernel) noexcept {
    switch (kernel) {
        case EffectKernel::Brightness:    return "brightness";
        case EffectKernel::Contrast:      return "contrast";
        case EffectKernel::Blur:          return "blur";
        case EffectKernel::CropTransform: return "crop_transform";
        case EffectKernel::ColorGrade:    return "color_grade";
        case EffectKernel::InvertColors:  return "invert_colors";
        case EffectKernel::Transition:    return "transition";
    }
    return "unknown";
}

std::optional<EffectType> effectTypeForKernel(EffectKernel kernel) noexcept {
    switch (kernel) {
        case EffectKernel::Brightness:    return EffectType::Brightness;
        case EffectKernel::Contrast:      return EffectType::Contrast;
        case EffectKernel::Blur:          return EffectType::Blur;
        case EffectKernel::CropTransform: return EffectType::CropTransform;
        case EffectKernel::ColorGrade:    return EffectType::ColorGrade;
        case EffectKernel::InvertColors:  return EffectType::InvertColors;
        case EffectKernel::Transition:    return std::nullopt; // no per-clip EffectType
    }
    return std::nullopt;
}

std::optional<EffectKernel> kernelForEffectType(EffectType type) noexcept {
    switch (type) {
        case EffectType::Brightness:    return EffectKernel::Brightness;
        case EffectType::Contrast:      return EffectKernel::Contrast;
        case EffectType::Blur:          return EffectKernel::Blur;
        case EffectType::CropTransform: return EffectKernel::CropTransform;
        case EffectType::ColorGrade:    return EffectKernel::ColorGrade;
        case EffectType::InvertColors:  return EffectKernel::InvertColors;
        case EffectType::Custom:        return std::nullopt; // caller-supplied kernel
    }
    return std::nullopt;
}

std::string_view effectKernelSource(EffectKernel kernel) noexcept {
    switch (kernel) {
        case EffectKernel::Brightness:    return kBrightnessSrc;
        case EffectKernel::Contrast:      return kContrastSrc;
        case EffectKernel::Blur:          return kBlurSrc;
        case EffectKernel::CropTransform: return kCropTransformSrc;
        case EffectKernel::ColorGrade:    return kColorGradeSrc;
        case EffectKernel::InvertColors:  return kInvertColorsSrc;
        case EffectKernel::Transition:    return kTransitionSrc;
    }
    return {};
}

bool shadercAvailable() noexcept {
#if defined(PALMIER_HAVE_SHADERC)
    return true;
#else
    return false;
#endif
}

Result<SpirvModule> compileEffectKernel([[maybe_unused]] EffectKernel kernel) {
#if defined(PALMIER_HAVE_SHADERC)
    const std::string_view source = effectKernelSource(kernel);
    const std::string_view name = effectKernelName(kernel);

    shaderc::Compiler compiler;
    shaderc::CompileOptions options;
    options.SetOptimizationLevel(shaderc_optimization_level_performance);

    const shaderc::SpvCompilationResult result = compiler.CompileGlslToSpv(
        source.data(), source.size(), shaderc_glsl_compute_shader,
        std::string(name).c_str(), "main", options);

    if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
        return err<SpirvModule>(makeError(
            ErrorCode::Internal,
            "EffectKernels: failed to compile '" + std::string(name) +
                "' GLSL to SPIR-V: " + result.GetErrorMessage()));
    }

    SpirvModule module;
    module.code.assign(result.cbegin(), result.cend());
    module.entryPoint = "main";
    if (!module.valid()) {
        return err<SpirvModule>(makeError(
            ErrorCode::Internal,
            "EffectKernels: shaderc produced an empty SPIR-V module for '" +
                std::string(name) + "'"));
    }
    return module;
#else
    return err<SpirvModule>(unsupported(
        "EffectKernels: shaderc is not compiled into this build; GLSL cannot be "
        "compiled to SPIR-V (the software effect path remains available)"));
#endif
}

Result<EffectKernelRegistry> EffectKernelRegistry::build() {
    if (!shadercAvailable()) {
        return err<EffectKernelRegistry>(unsupported(
            "EffectKernelRegistry::build: shaderc is not available in this build"));
    }

    EffectKernelRegistry registry;
    for (const EffectKernel kernel : allEffectKernels()) {
        Result<SpirvModule> compiled = compileEffectKernel(kernel);
        if (compiled.isError()) {
            return err<EffectKernelRegistry>(std::move(compiled).error());
        }
        registry.modules_.emplace(kernel, std::move(compiled).value());
    }
    return registry;
}

const SpirvModule* EffectKernelRegistry::module(EffectKernel kernel) const noexcept {
    const auto it = modules_.find(kernel);
    return it == modules_.end() ? nullptr : &it->second;
}

std::size_t EffectKernelRegistry::registerWith(Compositor& compositor) const {
    std::size_t registered = 0;
    for (const auto& [kernel, module] : modules_) {
        const std::optional<EffectType> type = effectTypeForKernel(kernel);
        if (!type) continue; // Transition is not a per-clip EffectType.
        compositor.registerEffect(*type, module);
        ++registered;
    }
    return registered;
}

} // namespace palmier::gpu
