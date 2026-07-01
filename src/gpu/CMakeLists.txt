# SPDX-License-Identifier: GPL-3.0-or-later
#
# src/gpu — Vulkan GPU abstraction layer (headline new capability).
#
# GPU context/device selection, zero-copy frame pool, compositor render graph,
# and SPIR-V effect kernels. Links Vulkan and shaderc (GLSL -> SPIR-V).
#
# Task 7.1 introduces the first concrete sources: the GPU context, physical-
# device enumeration + selection policy (Auto/PreferVendor/ForceIndex/
# ForceSoftware), capability probing into GpuCaps, and persisted user GPU
# selection. The Vulkan-specific enumeration is compiled only when the Vulkan
# headers are available (PALMIER_HAVE_VULKAN); otherwise the layer still builds
# and always yields the software fallback. FramePool, Compositor, effect
# kernels, and the HW codec bridge are added by later tasks (7.2-7.5).

add_library(palmier_gpu STATIC
    GpuSelection.cpp
    GpuSelectionStore.cpp
    GpuContext.cpp
    CodecBridge.cpp
    FramePool.cpp
    Compositor.cpp
    EffectKernels.cpp)
add_library(Palmier::gpu ALIAS palmier_gpu)

# Public headers are included as "gpu/<Header>.hpp" / "core/<Header>.hpp".
target_include_directories(palmier_gpu PUBLIC
    "${PROJECT_SOURCE_DIR}/src")

target_compile_features(palmier_gpu PUBLIC cxx_std_20)

target_link_libraries(palmier_gpu PUBLIC
    Palmier::core)

# Vulkan is a required project dependency (see cmake/PalmierDependencies.cmake).
# When its imported target is present we link it and enable the Vulkan-backed
# enumeration path; if it is somehow absent the layer still builds and degrades
# to the software fallback at runtime.
if(TARGET Vulkan::Vulkan)
    target_link_libraries(palmier_gpu PUBLIC Vulkan::Vulkan)
    target_compile_definitions(palmier_gpu PUBLIC PALMIER_HAVE_VULKAN=1)
endif()

# Vendor hardware codec paths for the HW decode/encode bridge (task 7.5).
# The bridge's *routing/fallback logic* is pure and always built; these defines
# only tell BridgeAvailability::fromBuildConfig() which concrete vendor backends
# are compiled into this build. A define is enabled when the corresponding
# PALMIER_ENABLE_* option is ON *and* its SDK/headers were located during
# dependency discovery (see cmake/PalmierDependencies.cmake). When all are off
# the bridge routes everything to the FFmpeg software path, matching the
# design's graceful-degradation requirement.
if(PALMIER_ENABLE_NVENC AND FFNVCODEC_FOUND)
    target_compile_definitions(palmier_gpu PUBLIC PALMIER_HAVE_NVENC=1)
endif()
if(PALMIER_ENABLE_VAAPI AND LIBVA_FOUND)
    target_compile_definitions(palmier_gpu PUBLIC PALMIER_HAVE_VAAPI=1)
endif()
if(PALMIER_ENABLE_QSV AND LIBVPL_FOUND)
    target_compile_definitions(palmier_gpu PUBLIC PALMIER_HAVE_QSV=1)
endif()

# shaderc (GLSL -> SPIR-V) compiles the effect compute kernels to SPIR-V
# (task 7.4, src/gpu/EffectKernels.cpp). When its imported target/library is
# present we link it and define PALMIER_HAVE_SHADERC so compileEffectKernel()
# runs shaderc for real; otherwise the kernels' GLSL sources are still exposed
# and the compositor falls back to the host-memory software effect path, so the
# layer builds and is fully testable without shaderc.
if(TARGET PkgConfig::SHADERC)
    target_link_libraries(palmier_gpu PUBLIC PkgConfig::SHADERC)
    target_compile_definitions(palmier_gpu PUBLIC PALMIER_HAVE_SHADERC=1)
elseif(SHADERC_LIBRARY)
    target_link_libraries(palmier_gpu PUBLIC ${SHADERC_LIBRARY})
    target_include_directories(palmier_gpu PUBLIC ${SHADERC_INCLUDE_DIR})
    target_compile_definitions(palmier_gpu PUBLIC PALMIER_HAVE_SHADERC=1)
endif()

if(PALMIER_WERROR)
    target_compile_options(palmier_gpu PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic -Werror>)
endif()
