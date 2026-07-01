// SPDX-License-Identifier: GPL-3.0-or-later
//
// gpu/GpuContext.cpp — Vulkan context creation, enumeration, selection.
//
// See GpuContext.hpp for the contract. The Vulkan-specific enumeration is
// compiled only when the Vulkan headers are available (PALMIER_HAVE_VULKAN);
// everywhere else the layer still builds and always yields the software
// fallback, so the selection logic and graceful-degradation behavior can be
// built and exercised without a Vulkan loader or a physical GPU.

#include "gpu/GpuContext.hpp"

#include <utility>

#include "gpu/FramePool.hpp"
#include "gpu/GpuSelection.hpp"

#if defined(PALMIER_HAVE_VULKAN)
#include <vulkan/vulkan.h>
#endif

namespace palmier::gpu {

// ---------------------------------------------------------------------------
// Vulkan-backed enumeration (compiled only when the headers are present).
// ---------------------------------------------------------------------------
#if defined(PALMIER_HAVE_VULKAN)

namespace {

[[nodiscard]] GpuDeviceType mapDeviceType(VkPhysicalDeviceType t) noexcept {
    switch (t) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return GpuDeviceType::DiscreteGpu;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return GpuDeviceType::IntegratedGpu;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return GpuDeviceType::VirtualGpu;
        case VK_PHYSICAL_DEVICE_TYPE_CPU:            return GpuDeviceType::Cpu;
        default:                                     return GpuDeviceType::Other;
    }
}

[[nodiscard]] std::size_t deviceLocalMemoryBytes(VkPhysicalDevice device) {
    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(device, &mem);
    std::size_t total = 0;
    for (uint32_t i = 0; i < mem.memoryHeapCount; ++i) {
        if (mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            total += static_cast<std::size_t>(mem.memoryHeaps[i].size);
        }
    }
    return total;
}

// Probe queue-family flags for compute and (where the headers expose Vulkan
// Video) hardware decode/encode support.
void probeQueueCapabilities(VkPhysicalDevice device, GpuCaps& caps) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    if (count == 0) return;
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
    for (const auto& f : families) {
        if (f.queueFlags & VK_QUEUE_COMPUTE_BIT) caps.supportsCompute = true;
#if defined(VK_KHR_video_queue)
        if (f.queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) caps.hwDecode = true;
#endif
#if defined(VK_KHR_video_encode_queue)
        if (f.queueFlags & VK_QUEUE_VIDEO_ENCODE_BIT_KHR) caps.hwEncode = true;
#endif
    }
}

} // namespace

std::vector<GpuDeviceInfo> GpuContext::enumerateVulkanDevices() {
    std::vector<GpuDeviceInfo> result;

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "Palmier Pro Linux";
    appInfo.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;

    VkInstance instance = VK_NULL_HANDLE;
    // If the loader is present but no ICD/driver initializes, this fails
    // gracefully; we return an empty list and the caller degrades to software.
    if (vkCreateInstance(&createInfo, nullptr, &instance) != VK_SUCCESS) {
        return result;
    }

    uint32_t deviceCount = 0;
    if (vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr) != VK_SUCCESS ||
        deviceCount == 0) {
        vkDestroyInstance(instance, nullptr);
        return result;
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

    result.reserve(deviceCount);
    for (uint32_t i = 0; i < deviceCount; ++i) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(devices[i], &props);

        GpuDeviceInfo info;
        info.index = static_cast<int>(i);
        info.name = props.deviceName;
        info.vendor = vendorFromPciId(props.vendorID);
        info.type = mapDeviceType(props.deviceType);

        GpuCaps caps;
        caps.vendorId = info.vendor;
        caps.vendor = std::string{vendorName(info.vendor)};
        caps.vramBytes = deviceLocalMemoryBytes(devices[i]);
        probeQueueCapabilities(devices[i], caps);
        info.caps = std::move(caps);

        result.push_back(std::move(info));
    }

    vkDestroyInstance(instance, nullptr);
    return result;
}

#else // !PALMIER_HAVE_VULKAN

std::vector<GpuDeviceInfo> GpuContext::enumerateVulkanDevices() {
    // Built without the Vulkan headers: no hardware enumeration is possible.
    // The caller degrades to the software fallback.
    return {};
}

#endif // PALMIER_HAVE_VULKAN

// ---------------------------------------------------------------------------
// Move operations / destructor. Defaulted here (not in the header) so the
// unique_ptr<FramePool> member can hold an incomplete type in GpuContext.hpp:
// FramePool is complete in this translation unit.
// ---------------------------------------------------------------------------
GpuContext::GpuContext(GpuContext&&) noexcept = default;
GpuContext& GpuContext::operator=(GpuContext&&) noexcept = default;
GpuContext::~GpuContext() = default;

// ---------------------------------------------------------------------------
// Frame pool (lazily constructed, bounded by the selected device's VRAM).
// ---------------------------------------------------------------------------
FramePool& GpuContext::framePool() {
    if (!framePool_) {
        // On real hardware the pool is bounded by device-local memory; on the
        // software fallback (vramBytes == 0) use a modest host-memory budget so
        // the decode -> composite -> encode pipeline still functions on the CPU
        // path. 256 MiB comfortably holds several 4K RGBA8 working frames.
        constexpr std::size_t kSoftwareHostBudget = std::size_t{256} * 1024 * 1024;
        const std::size_t budget = caps_.vramBytes > 0 ? caps_.vramBytes : kSoftwareHostBudget;
        framePool_ = std::make_unique<FramePool>(budget);
    }
    return *framePool_;
}

// ---------------------------------------------------------------------------
// Software fallback.
// ---------------------------------------------------------------------------
GpuContext GpuContext::softwareFallback() {
    GpuContext ctx;
    ctx.caps_ = GpuCaps::software();
    ctx.softwareFallback_ = true;
    ctx.selectedIndex_ = -1;
    ctx.deviceName_ = "Software (CPU)";
    return ctx;
}

// ---------------------------------------------------------------------------
// Selection helpers.
// ---------------------------------------------------------------------------
namespace {

// Find the device that best matches a persisted user selection. Prefers an
// exact (index + name) match, then a name match, then an index match. Returns
// the matched enumeration index, or nullopt when nothing matches (e.g. the
// device was removed since the selection was saved).
[[nodiscard]] std::optional<int> matchPersisted(const std::vector<GpuDeviceInfo>& devices,
                                                const PersistedGpuSelection& ps) {
    const GpuDeviceInfo* byName = nullptr;
    const GpuDeviceInfo* byIndex = nullptr;
    for (const auto& d : devices) {
        const bool nameMatch = !ps.deviceName.empty() && d.name == ps.deviceName;
        if (nameMatch && d.index == ps.index) return d.index; // exact match
        if (nameMatch && byName == nullptr) byName = &d;
        if (d.index == ps.index && byIndex == nullptr) byIndex = &d;
    }
    if (byName) return byName->index;
    if (byIndex && (ps.vendor == GpuVendor::Unknown || byIndex->vendor == ps.vendor)) {
        return byIndex->index;
    }
    return std::nullopt;
}

[[nodiscard]] const GpuDeviceInfo* deviceByIndex(const std::vector<GpuDeviceInfo>& devices,
                                                 int index) {
    for (const auto& d : devices) {
        if (d.index == index) return &d;
    }
    return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// Core creation path.
// ---------------------------------------------------------------------------
Result<GpuContext> GpuContext::createWith(GpuSelectionPolicy policy,
                                          const PhysicalDeviceEnumerator& enumerate,
                                          GpuSelectionStore* store,
                                          GpuContextConfig config) {
    using clock = std::chrono::steady_clock;
    const auto start = clock::now();

    // A persisted software choice (Requirement 10.6) short-circuits to the CPU
    // path and is treated as an intentional user decision (no "unavailable"
    // notice). Only consulted for an Auto policy.
    bool userChoseSoftware = (policy.mode == GpuSelectionMode::ForceSoftware);

    std::optional<PersistedGpuSelection> persisted;
    if (policy.mode == GpuSelectionMode::Auto && config.honorPersistedSelection &&
        store != nullptr) {
        if (auto loaded = store->load(); loaded.isOk()) {
            persisted = std::move(loaded.value());
            if (persisted && persisted->mode == GpuSelectionMode::ForceSoftware) {
                userChoseSoftware = true;
            }
        }
        // A corrupt selection file is non-fatal: ignore it and auto-select.
    }

    // Explicit or persisted software request -> CPU fallback immediately.
    if (userChoseSoftware) {
        GpuContext ctx = softwareFallback();
        if (policy.mode != GpuSelectionMode::ForceSoftware) {
            // Reflects a persisted user preference rather than a forced policy.
            ctx.unavailableNotice_.reset();
        }
        return ctx;
    }

    // Enumerate + probe available hardware, bounded by the detection budget.
    std::vector<GpuDeviceInfo> devices = enumerate ? enumerate() : std::vector<GpuDeviceInfo>{};
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - start);

    if (elapsed > config.detectionBudget) {
        // Requirement 10.1/10.4: exceeding the detection budget degrades to CPU
        // with a non-blocking notice.
        GpuContext ctx = softwareFallback();
        ctx.availableDevices_ = std::move(devices);
        ctx.unavailableNotice_ =
            "GPU detection exceeded the time budget; using CPU processing.";
        return ctx;
    }

    // Resolve the effective policy, folding in a valid persisted selection when
    // running Auto (Requirement 10.6).
    GpuSelectionPolicy effective = policy;
    if (policy.mode == GpuSelectionMode::Auto && persisted &&
        persisted->mode != GpuSelectionMode::ForceSoftware) {
        if (auto matched = matchPersisted(devices, *persisted)) {
            effective = GpuSelectionPolicy::forceIndex(*matched);
        }
    }

    const std::optional<int> chosen = selectDevice(devices, effective);
    if (!chosen.has_value()) {
        // No compatible GPU -> CPU path with a non-blocking notice (Req 10.4).
        GpuContext ctx = softwareFallback();
        ctx.availableDevices_ = std::move(devices);
        ctx.unavailableNotice_ =
            "No compatible GPU was found; using CPU processing.";
        return ctx;
    }

    const GpuDeviceInfo* dev = deviceByIndex(devices, *chosen);
    // Defensive: selectDevice only returns compute-capable indices, but never
    // trust an invariant we can cheaply re-check — degrade rather than throw.
    if (dev == nullptr || !dev->caps.supportsCompute) {
        GpuContext ctx = softwareFallback();
        ctx.availableDevices_ = std::move(devices);
        ctx.unavailableNotice_ =
            "Selected GPU is not usable for compute; using CPU processing.";
        return ctx;
    }

    GpuContext ctx;
    ctx.softwareFallback_ = false;
    ctx.selectedIndex_ = dev->index;
    ctx.deviceName_ = dev->name.empty() ? std::string{"GPU"} : dev->name;
    ctx.caps_ = dev->caps;
    ctx.availableDevices_ = std::move(devices);
    ctx.unavailableNotice_.reset();

    // Persist explicit user selections so they survive restarts (Req 10.6).
    if (store != nullptr && (policy.mode == GpuSelectionMode::ForceIndex)) {
        const auto rec = GpuSelectionStore::fromDevice(*dev);
        (void)store->save(rec); // Persistence failure must not fail context creation.
    }

    return ctx;
}

Result<GpuContext> GpuContext::create(GpuSelectionPolicy policy, GpuContextConfig config) {
    GpuSelectionStore store; // default per-user config location
    return createWith(policy, &GpuContext::enumerateVulkanDevices, &store, config);
}

Result<GpuContext> GpuContext::create(GpuSelectionPolicy policy) {
    return create(policy, GpuContextConfig{});
}

} // namespace palmier::gpu
