// SPDX-License-Identifier: GPL-3.0-or-later
//
// gpu/GpuTypes.hpp — vendor-neutral value types for the GPU abstraction layer.
//
// These types describe the GPU devices the Vulkan layer enumerates, the
// capabilities it probes on each, and the policy the caller uses to steer
// device selection. They are deliberately free of any Vulkan dependency so the
// selection algorithm (see GpuSelection.hpp) and its tests can run on machines
// without a Vulkan loader or a physical GPU — matching the design's requirement
// that the layer "never throws for 'no GPU'" and always offers a software path.
//
// The GpuCaps descriptor mirrors design.md "GPU Capability Descriptor".

#ifndef PALMIER_GPU_GPUTYPES_HPP
#define PALMIER_GPU_GPUTYPES_HPP

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>

namespace palmier::gpu {

/// Coarse hardware vendor classification.
///
/// The design's GpuCaps stores a human-readable vendor string
/// ("NVIDIA" | "AMD" | "Intel" | "software"); this enum is the machine-friendly
/// companion the selection algorithm scores against and the PreferVendor policy
/// targets.
enum class GpuVendor {
    Unknown = 0,
    NVIDIA,
    AMD,
    Intel,
    Software,
};

/// Physical-device class, used by scoring to prefer discrete GPUs over iGPUs.
/// Values mirror Vulkan's VkPhysicalDeviceType ordering conceptually but are
/// intentionally independent of the Vulkan headers.
enum class GpuDeviceType {
    Other = 0,
    IntegratedGpu,
    DiscreteGpu,
    VirtualGpu,
    Cpu,
    Software,
};

/// Media codecs the hardware bridges may decode/encode. Kept minimal here; the
/// media engine (later tasks) owns the full codec catalog. Defined locally so
/// the GPU layer does not depend on the media module.
enum class CodecId {
    Unknown = 0,
    H264,
    HEVC,
    AV1,
    VP9,
    MPEG2,
};

/// Human-readable, stable vendor name. "software" for the CPU fallback, matching
/// the string used in design.md's GpuCaps example.
[[nodiscard]] constexpr std::string_view vendorName(GpuVendor v) noexcept {
    switch (v) {
        case GpuVendor::NVIDIA:   return "NVIDIA";
        case GpuVendor::AMD:      return "AMD";
        case GpuVendor::Intel:    return "Intel";
        case GpuVendor::Software: return "software";
        case GpuVendor::Unknown:  return "unknown";
    }
    return "unknown";
}

/// Map a PCI vendor id (as reported by Vulkan) to a GpuVendor.
[[nodiscard]] constexpr GpuVendor vendorFromPciId(std::uint32_t pciVendorId) noexcept {
    switch (pciVendorId) {
        case 0x10DE: return GpuVendor::NVIDIA; // NVIDIA
        case 0x1002: return GpuVendor::AMD;    // AMD
        case 0x1022: return GpuVendor::AMD;    // AMD (alt)
        case 0x8086: return GpuVendor::Intel;  // Intel
        default:     return GpuVendor::Unknown;
    }
}

/// Capability descriptor for a single device (design.md "GPU Capability
/// Descriptor"). `vendor` is retained as a string for parity with the design;
/// `vendorId` is the enum companion used by scoring and persistence.
struct GpuCaps {
    std::string        vendor{"software"};   ///< "NVIDIA" | "AMD" | "Intel" | "software"
    GpuVendor          vendorId{GpuVendor::Software};
    bool               supportsCompute{false}; ///< Vulkan compute for effect kernels
    bool               hwDecode{false};        ///< VAAPI / NVDEC / QSV decode available
    bool               hwEncode{false};        ///< VAAPI / NVENC / QSV encode available
    std::set<CodecId>  decodeCodecs{};
    std::set<CodecId>  encodeCodecs{};
    std::size_t        vramBytes{0};

    /// Capabilities describing the always-available software (CPU) fallback.
    [[nodiscard]] static GpuCaps software() {
        GpuCaps c;
        c.vendor = std::string{vendorName(GpuVendor::Software)};
        c.vendorId = GpuVendor::Software;
        c.supportsCompute = false; // CPU compositing path; no Vulkan compute.
        c.hwDecode = false;
        c.hwEncode = false;
        c.vramBytes = 0;
        return c;
    }
};

/// A device discovered during enumeration together with its probed capabilities.
/// The selection algorithm scores a list of these; the Vulkan probe produces
/// them from real hardware, while tests can construct them directly.
struct GpuDeviceInfo {
    int           index{0};        ///< Enumeration index (stable per run).
    std::string   name{};          ///< Human-readable device name.
    GpuVendor     vendor{GpuVendor::Unknown};
    GpuDeviceType type{GpuDeviceType::Other};
    GpuCaps       caps{};
};

/// How the caller wants a GPU chosen.
///
///   * Auto          — pick the best available device by score (or a valid
///                     persisted user selection when one exists).
///   * PreferVendor  — bias scoring strongly toward the given vendor, but still
///                     fall back to the next-best device if that vendor is absent.
///   * ForceIndex    — use exactly the device at the given enumeration index;
///                     if the index is invalid, degrade to the software path.
///   * ForceSoftware — bypass all hardware and use the CPU path.
enum class GpuSelectionMode {
    Auto = 0,
    PreferVendor,
    ForceIndex,
    ForceSoftware,
};

struct GpuSelectionPolicy {
    GpuSelectionMode mode{GpuSelectionMode::Auto};
    GpuVendor        preferredVendor{GpuVendor::Unknown}; ///< used when mode == PreferVendor
    int              forcedIndex{-1};                     ///< used when mode == ForceIndex

    [[nodiscard]] static GpuSelectionPolicy automatic() { return {}; }
    [[nodiscard]] static GpuSelectionPolicy preferVendor(GpuVendor v) {
        return {GpuSelectionMode::PreferVendor, v, -1};
    }
    [[nodiscard]] static GpuSelectionPolicy forceIndex(int i) {
        return {GpuSelectionMode::ForceIndex, GpuVendor::Unknown, i};
    }
    [[nodiscard]] static GpuSelectionPolicy forceSoftware() {
        return {GpuSelectionMode::ForceSoftware, GpuVendor::Unknown, -1};
    }
};

} // namespace palmier::gpu

#endif // PALMIER_GPU_GPUTYPES_HPP
