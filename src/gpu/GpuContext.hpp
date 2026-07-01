// SPDX-License-Identifier: GPL-3.0-or-later
//
// gpu/GpuContext.hpp — Vulkan context creation, device enumeration & selection.
//
// This is the entry point of the GPU Abstraction Layer (design.md "Component 4:
// GPU Abstraction Layer (Vulkan)" and "Low-Level Design: GPU Integration
// Layer"). GpuContext::create enumerates the physical devices Vulkan exposes,
// probes each device's capabilities into a GpuCaps descriptor, and selects one
// according to a GpuSelectionPolicy (Auto / PreferVendor / ForceIndex /
// ForceSoftware). A user's explicit choice is persisted and reapplied on the
// next launch (Requirement 10.6).
//
// Key guarantees (design + Requirements 10.1/10.4/10.6):
//   * Never throws / never fails for "no GPU": if the Vulkan loader is absent,
//     no device is compatible, or detection exceeds the time budget, create()
//     returns a *software fallback* context (CPU compositing / FFmpeg SW codecs)
//     and records a non-blocking notice that acceleration is unavailable.
//   * Detection is bounded by a configurable time budget (default 10 s, per
//     Requirement 10.1); exceeding it degrades to the software path.
//
// The actual Vulkan calls are compiled only when the Vulkan headers are present
// (PALMIER_HAVE_VULKAN); otherwise the layer still builds and runs, always
// yielding the software fallback. This lets the code build and be tested on
// machines with no Vulkan loader or GPU at all (e.g. CI/sandbox), which the
// selection algorithm and fallback behavior do not depend on.
//
// Frame pooling, the compositor render graph, effect kernels, and the hardware
// codec bridge are implemented by later tasks (7.2–7.5); this file covers only
// context/device selection and capability probing.

#ifndef PALMIER_GPU_GPUCONTEXT_HPP
#define PALMIER_GPU_GPUCONTEXT_HPP

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/Result.hpp"
#include "gpu/GpuSelectionStore.hpp"
#include "gpu/GpuTypes.hpp"

namespace palmier::gpu {

class FramePool;

/// A function that enumerates and probes the available physical devices.
/// Injectable so tests can supply synthetic device lists without a GPU; the
/// default enumerator uses Vulkan when available (and returns an empty list when
/// the loader/driver is absent).
using PhysicalDeviceEnumerator = std::function<std::vector<GpuDeviceInfo>()>;

/// Tunables for context creation.
struct GpuContextConfig {
    /// Maximum wall-clock time allowed for device detection before degrading to
    /// software (Requirement 10.1: within 10 seconds).
    std::chrono::milliseconds detectionBudget{std::chrono::seconds{10}};

    /// When true (default) an Auto policy honors a valid persisted user
    /// selection (Requirement 10.6). Ignored for explicit Force* policies.
    bool honorPersistedSelection{true};
};

/// The selected GPU context (or the software fallback). Owns the chosen device's
/// capabilities and identity; the underlying Vulkan handles are managed
/// internally and released on destruction.
class GpuContext {
public:
    GpuContext(const GpuContext&) = delete;
    GpuContext& operator=(const GpuContext&) = delete;
    GpuContext(GpuContext&&) noexcept;
    GpuContext& operator=(GpuContext&&) noexcept;
    ~GpuContext();

    /// Create a context using the real Vulkan enumerator and the default config.
    /// Never returns an error for "no GPU" — degrades to the software fallback.
    [[nodiscard]] static Result<GpuContext> create(GpuSelectionPolicy policy);

    /// As above, with an explicit configuration (time budget, persistence).
    [[nodiscard]] static Result<GpuContext> create(GpuSelectionPolicy policy,
                                                   GpuContextConfig config);

    /// Create a context with an injected enumerator and selection store (testing
    /// seam, and the composition point the two convenience overloads build on).
    /// A null `store` disables persistence.
    [[nodiscard]] static Result<GpuContext> createWith(GpuSelectionPolicy policy,
                                                       const PhysicalDeviceEnumerator& enumerate,
                                                       GpuSelectionStore* store,
                                                       GpuContextConfig config = {});

    /// The always-available CPU fallback context.
    [[nodiscard]] static GpuContext softwareFallback();

    // --- Accessors ---------------------------------------------------------

    /// Capabilities of the selected device (or software fallback caps).
    [[nodiscard]] const GpuCaps& capabilities() const noexcept { return caps_; }

    /// True when this context is the CPU software fallback (no GPU selected).
    [[nodiscard]] bool isSoftwareFallback() const noexcept { return softwareFallback_; }

    /// Enumeration index of the selected device, or -1 for the software fallback.
    [[nodiscard]] int selectedDeviceIndex() const noexcept { return selectedIndex_; }

    /// Human-readable name of the selected device ("Software (CPU)" for fallback).
    [[nodiscard]] const std::string& deviceName() const noexcept { return deviceName_; }

    /// The devices discovered during creation (for UI presentation of choices,
    /// Requirement 10.6). Empty on the software fallback path.
    [[nodiscard]] const std::vector<GpuDeviceInfo>& availableDevices() const noexcept {
        return availableDevices_;
    }

    /// A non-blocking, user-facing notice set when acceleration is unavailable
    /// (Requirement 10.4) — e.g. no compatible GPU or detection timed out.
    /// std::nullopt when GPU acceleration is active.
    [[nodiscard]] const std::optional<std::string>& unavailableNotice() const noexcept {
        return unavailableNotice_;
    }

    /// The default Vulkan-backed enumerator. Returns an empty list when the
    /// Vulkan loader/driver is unavailable or no physical device is present.
    [[nodiscard]] static std::vector<GpuDeviceInfo> enumerateVulkanDevices();

    /// The pool of GPU-resident frames for this context (design.md "Component 4",
    /// Requirement 10.2). Created lazily on first use and bounded by the selected
    /// device's VRAM (GpuCaps.vramBytes); on the software fallback (vramBytes == 0)
    /// a modest host-memory budget is used so the pipeline stays functional off-GPU.
    [[nodiscard]] FramePool& framePool();

private:
    GpuContext() = default;

    GpuCaps                    caps_{GpuCaps::software()};
    bool                       softwareFallback_{true};
    int                        selectedIndex_{-1};
    std::string                deviceName_{"Software (CPU)"};
    std::vector<GpuDeviceInfo> availableDevices_{};
    std::optional<std::string> unavailableNotice_{};
    std::unique_ptr<FramePool> framePool_{}; ///< Lazily constructed; see framePool().
};

} // namespace palmier::gpu

#endif // PALMIER_GPU_GPUCONTEXT_HPP
