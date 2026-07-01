// SPDX-License-Identifier: GPL-3.0-or-later
//
// gpu/GpuSelection.hpp — the vendor-neutral device selection algorithm.
//
// Implements design.md "Device selection policy" (ALGORITHM selectGpu) as a pure
// function over a list of already-probed GpuDeviceInfo descriptors. Keeping the
// scoring/selection logic free of Vulkan makes it directly unit-testable with
// synthetic device lists (the sandbox has no GPU) and lets GpuContext reuse it
// for both the real Vulkan probe and the software fallback.
//
// Scoring rules (design "discrete GPU > iGPU; hwEncode adds weight"):
//   * discrete GPU ranks above integrated, which ranks above other/virtual;
//   * hardware encode is weighted above hardware decode (export is the costliest
//     GPU win) which is weighted above compute-only;
//   * larger VRAM breaks ties among otherwise-equal devices;
//   * PreferVendor adds a large bias toward the requested vendor without
//     excluding the others.
// A device that cannot run Vulkan compute is never selected — effect kernels
// require compute (see the ASSERT in the design pseudocode); such a device
// scores below the software fallback threshold and is skipped.

#ifndef PALMIER_GPU_GPUSELECTION_HPP
#define PALMIER_GPU_GPUSELECTION_HPP

#include <cstdint>
#include <optional>
#include <vector>

#include "gpu/GpuTypes.hpp"

namespace palmier::gpu {

/// Score a single device under a policy. Higher is better. A device that does
/// not support compute returns a score of 0 (it is unusable for effect kernels
/// and will not be selected over the software fallback).
[[nodiscard]] std::int64_t scoreDevice(const GpuDeviceInfo& device,
                                       const GpuSelectionPolicy& policy) noexcept;

/// Select a device index from `devices` under `policy`, mirroring the design's
/// selectGpu algorithm. Returns std::nullopt to mean "use the software
/// fallback" (empty device list, ForceSoftware, an out-of-range ForceIndex, or
/// no compute-capable device). Ties are broken by the lower enumeration index
/// for determinism.
[[nodiscard]] std::optional<int> selectDevice(const std::vector<GpuDeviceInfo>& devices,
                                              const GpuSelectionPolicy& policy) noexcept;

} // namespace palmier::gpu

#endif // PALMIER_GPU_GPUSELECTION_HPP
