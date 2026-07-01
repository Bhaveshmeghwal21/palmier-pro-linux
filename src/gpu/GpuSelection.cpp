// SPDX-License-Identifier: GPL-3.0-or-later
//
// gpu/GpuSelection.cpp — implementation of the device selection algorithm.

#include "gpu/GpuSelection.hpp"

namespace palmier::gpu {

namespace {

// Base weight by physical-device class: discrete > integrated > virtual > other.
// CPU/software device classes never win over the dedicated software fallback.
[[nodiscard]] std::int64_t deviceTypeScore(GpuDeviceType type) noexcept {
    switch (type) {
        case GpuDeviceType::DiscreteGpu:   return 4000;
        case GpuDeviceType::IntegratedGpu: return 2000;
        case GpuDeviceType::VirtualGpu:    return 1000;
        case GpuDeviceType::Other:         return 500;
        case GpuDeviceType::Cpu:           return 0;
        case GpuDeviceType::Software:      return 0;
    }
    return 0;
}

} // namespace

std::int64_t scoreDevice(const GpuDeviceInfo& device,
                         const GpuSelectionPolicy& policy) noexcept {
    // Compute is mandatory for effect kernels: an incapable device is unusable.
    if (!device.caps.supportsCompute) {
        return 0;
    }

    std::int64_t score = deviceTypeScore(device.type);
    if (score == 0) {
        // A compute-capable device with no meaningful class still ranks above
        // the software fallback so acceleration is preferred when present.
        score = 500;
    }

    // Hardware encode is the most valuable acceleration (export throughput),
    // then hardware decode, then compute-only.
    if (device.caps.hwEncode) score += 800;
    if (device.caps.hwDecode) score += 400;

    // VRAM breaks ties among otherwise-comparable devices. Scaled to MiB and
    // capped so a huge card cannot leapfrog a device class on memory alone.
    const std::int64_t vramMiB =
        static_cast<std::int64_t>(device.caps.vramBytes / (1024ull * 1024ull));
    score += (vramMiB > 512 ? 512 : vramMiB);

    // PreferVendor: strong bias toward the requested vendor without excluding
    // the alternatives (they remain selectable if the preferred vendor is gone).
    if (policy.mode == GpuSelectionMode::PreferVendor &&
        policy.preferredVendor != GpuVendor::Unknown &&
        device.vendor == policy.preferredVendor) {
        score += 100000;
    }

    return score;
}

std::optional<int> selectDevice(const std::vector<GpuDeviceInfo>& devices,
                                const GpuSelectionPolicy& policy) noexcept {
    // Explicit software request, or nothing to choose from -> software path.
    if (policy.mode == GpuSelectionMode::ForceSoftware || devices.empty()) {
        return std::nullopt;
    }

    // ForceIndex: honor exactly the requested device when it exists and is
    // usable (compute-capable); otherwise degrade safely to software.
    if (policy.mode == GpuSelectionMode::ForceIndex) {
        for (const auto& d : devices) {
            if (d.index == policy.forcedIndex) {
                return d.caps.supportsCompute ? std::optional<int>{d.index} : std::nullopt;
            }
        }
        return std::nullopt;
    }

    // Auto / PreferVendor: argmax by score, ties broken by lower index.
    std::optional<int> bestIndex;
    std::int64_t bestScore = 0;
    for (const auto& d : devices) {
        const std::int64_t s = scoreDevice(d, policy);
        if (s <= 0) {
            continue; // unusable (no compute) -> never selected.
        }
        if (!bestIndex.has_value() || s > bestScore ||
            (s == bestScore && d.index < *bestIndex)) {
            bestScore = s;
            bestIndex = d.index;
        }
    }
    return bestIndex; // nullopt if no usable device -> software fallback.
}

} // namespace palmier::gpu
