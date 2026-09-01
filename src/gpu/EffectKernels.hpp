// SPDX-License-Identifier: GPL-3.0-or-later
//
// gpu/EffectKernels.hpp — the SPIR-V compute kernels that realize per-clip
// effects, and the registry that compiles and installs them (design.md
// "Effects as SPIR-V compute kernels"; Requirements 10.2 and 10.7, task 7.4).
//
// The design specifies that effects (brightness/contrast, blur, crop/transform,
// color grade, transitions) are implemented as compute shaders compiled to
// SPIR-V and registered with the Compositor: "Each kernel reads an input
// VkImage, writes an output VkImage, and receives parameters via a
// uniform/push-constant block." This file provides:
//
//   * The GLSL compute source for every effect kernel (effectKernelSource).
//     Each kernel's math mirrors the corresponding applyEffectSoftware branch in
//     Compositor.cpp bit-for-bit — that host-memory path is the reference the
//     GPU kernels are validated against for property P5 (GPU/CPU parity).
//
//   * compileEffectKernel(): compiles a kernel's GLSL to a SPIR-V SpirvModule
//     using shaderc. shaderc is a real project dependency (see
//     cmake/PalmierDependencies.cmake); when it is compiled in
//     (PALMIER_HAVE_SHADERC) compilation runs for real and the produced module
//     is a valid SPIR-V word stream (its first word is the 0x07230203 magic).
//     When shaderc is absent from the build (e.g. a minimal CI/sandbox image)
//     compilation returns an Unsupported error, and rendering still works
//     everywhere through the software reference path — matching the guard
//     pattern used by GpuContext / FramePool / Compositor.
//
//   * EffectKernelRegistry: compiles all kernels once and registers the
//     EffectType-keyed ones with a Compositor (Compositor::registerEffect). The
//     transition kernel blends two inputs and has no EffectType, so it is held
//     for the compositor's transition stage rather than registered as a per-clip
//     effect.
//
// Nothing here requires a GPU or a Vulkan loader: shaderc compiles GLSL to
// SPIR-V purely on the CPU, so the kernels can be built and validated in a
// headless environment, and the registry degrades cleanly when shaderc is not
// present.

#ifndef PALMIER_GPU_EFFECTKERNELS_HPP
#define PALMIER_GPU_EFFECTKERNELS_HPP

#include <array>
#include <map>
#include <optional>
#include <string_view>

#include "core/Effect.hpp"
#include "core/Result.hpp"
#include "gpu/Compositor.hpp"

namespace palmier::gpu {

/// The set of built-in effect compute kernels.
///
/// The first six map one-to-one onto the core EffectType kinds and are what a
/// clip's effects dispatch to. `Transition` is a two-input cross-dissolve used
/// by the compositor's transition stage; it has no EffectType (the core model
/// represents transitions with the separate Transition type on a clip).
enum class EffectKernel {
    Brightness,
    Contrast,
    Blur,
    CropTransform,
    ColorGrade,
    InvertColors,
    ToneCurve,
    Transition,
};

/// All kernels, in a stable order (handy for iterating / registering).
[[nodiscard]] constexpr std::array<EffectKernel, 8> allEffectKernels() noexcept {
    return {EffectKernel::Brightness,   EffectKernel::Contrast,
            EffectKernel::Blur,         EffectKernel::CropTransform,
            EffectKernel::ColorGrade,   EffectKernel::InvertColors,
            EffectKernel::ToneCurve,    EffectKernel::Transition};
}

/// Stable, human-readable kernel name (for logs / diagnostics / the shaderc
/// artifact name).
[[nodiscard]] std::string_view effectKernelName(EffectKernel kernel) noexcept;

/// The EffectType a kernel realizes, or std::nullopt for `Transition` (which is
/// not a per-clip EffectType).
[[nodiscard]] std::optional<EffectType> effectTypeForKernel(EffectKernel kernel) noexcept;

/// The kernel that realizes a per-clip EffectType. `Custom` has no built-in
/// kernel (its behavior comes from a caller-supplied module), so it returns
/// std::nullopt.
[[nodiscard]] std::optional<EffectKernel> kernelForEffectType(EffectType type) noexcept;

/// The GLSL (compute) source for a kernel. Never empty; each source declares
/// `#version 450`, a compute `local_size`, an input image at binding 0, an
/// output image at the last binding, and a push-constant parameter block (the
/// invert-colors kernel takes no parameters, so its block is a reserved
/// placeholder that keeps the dispatch signature uniform). The
/// transition kernel additionally reads a second input at binding 1. Exposed so
/// the sources can be inspected/validated even in builds without shaderc.
[[nodiscard]] std::string_view effectKernelSource(EffectKernel kernel) noexcept;

/// Whether this build can compile GLSL to SPIR-V (i.e. shaderc is linked in).
/// When false, compileEffectKernel / EffectKernelRegistry::build return an
/// Unsupported error and callers fall back to the software effect path.
[[nodiscard]] bool shadercAvailable() noexcept;

/// Compile a kernel's GLSL to a SPIR-V module via shaderc.
///
/// On success the returned SpirvModule carries a valid SPIR-V word stream
/// (module.valid() is true and code.front() == 0x07230203). Fails with:
///   * Unsupported — shaderc is not compiled into this build.
///   * Internal    — the GLSL failed to compile (message carries shaderc's log).
[[nodiscard]] Result<SpirvModule> compileEffectKernel(EffectKernel kernel);

/// Compiles the built-in effect kernels once and installs them on a Compositor.
///
/// build() compiles every kernel (requires shaderc). registerWith() then calls
/// Compositor::registerEffect for each kernel that maps to an EffectType; the
/// transition module is kept accessible via transitionModule() for the
/// compositor's transition stage. Because the registry owns the compiled
/// modules, a single build() can install kernels on several compositors.
class EffectKernelRegistry {
public:
    /// Compile all built-in kernels. Returns an Unsupported error when shaderc
    /// is not available in this build, or an Internal error (with the shaderc
    /// log) if any kernel fails to compile.
    [[nodiscard]] static Result<EffectKernelRegistry> build();

    /// The compiled module for a kernel, or nullptr if it is not present.
    [[nodiscard]] const SpirvModule* module(EffectKernel kernel) const noexcept;

    /// The transition kernel module (two-input cross-dissolve).
    [[nodiscard]] const SpirvModule* transitionModule() const noexcept {
        return module(EffectKernel::Transition);
    }

    /// Number of compiled kernels held (7 on a successful build()).
    [[nodiscard]] std::size_t size() const noexcept { return modules_.size(); }

    /// Register every EffectType-mapped kernel (all but Transition) with
    /// `compositor` via Compositor::registerEffect. Returns the number of
    /// kernels registered.
    std::size_t registerWith(Compositor& compositor) const;

private:
    EffectKernelRegistry() = default;

    std::map<EffectKernel, SpirvModule> modules_{};
};

} // namespace palmier::gpu

#endif // PALMIER_GPU_EFFECTKERNELS_HPP
