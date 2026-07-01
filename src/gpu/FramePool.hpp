// SPDX-License-Identifier: GPL-3.0-or-later
//
// gpu/FramePool.hpp — zero-copy, VRAM-capped pool of GPU-resident frames.
//
// This implements the "Zero-copy Frame Pool" of design.md "Component 4: GPU
// Abstraction Layer (Vulkan)" (Requirement 10.2). The pool is the memory
// backbone that keeps frames GPU-resident from decode -> effects -> composite
// -> encode:
//
//   * Pooled frames — the pool allocates and owns them; on release they return
//     to a free list and are reused for a later request of the same geometry,
//     avoiding per-frame (re)allocation. The total memory the pool allocates is
//     bounded by a byte budget derived from the selected device's VRAM
//     (GpuCaps.vramBytes); a request that cannot fit even after reclaiming free
//     frames fails cleanly rather than over-committing the GPU.
//
//   * Imported frames — a frame whose pixels already live in memory owned by
//     someone else (a hardware decoder's DMA-BUF export on VAAPI/Intel/AMD, or a
//     CUDA device pointer from NVDEC on NVIDIA). These are adopted *without a
//     copy* and exposed through the same image-handle abstraction, so the
//     compositor and encoder bind them exactly like pooled frames. Because the
//     memory is externally owned, imports do not draw down the pool's allocation
//     budget; they are tracked separately and freed (not recycled) on release.
//
// Every frame is surfaced through an ImageHandle — the vendor-neutral binding
// point downstream GPU stages consume. In a Vulkan build the handle carries the
// VkImage/VkDeviceMemory imported from the external allocation; the platform
// bits are compiled only under PALMIER_HAVE_VULKAN. So the layer builds and is
// fully exercisable on a machine with no Vulkan loader or GPU (e.g. CI/sandbox)
// via a host-memory frame representation, mirroring GpuContext's guard pattern.
//
// Ownership/lifetime: acquire() hands back a move-only FrameLease (RAII) that
// returns its frame to the pool on destruction; a lease must not outlive the
// FramePool that produced it.
//
// The compositor render graph (7.3), effect kernels (7.4), and the hardware
// codec bridge (7.5) build on this pool but are out of scope here.

#ifndef PALMIER_GPU_FRAMEPOOL_HPP
#define PALMIER_GPU_FRAMEPOOL_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/Result.hpp"

#if defined(PALMIER_HAVE_VULKAN)
#include <vulkan/vulkan.h>
#endif

namespace palmier::gpu {

/// Pixel layout of a pooled/imported frame. Deliberately minimal — the
/// compositor and effect kernels (7.3/7.4) extend this catalog as needed. The
/// value determines a frame's memory footprint (see bytesPerFrame).
enum class FrameFormat {
    RGBA8,    ///< 8-bit RGBA, 4 bytes/pixel — the compositor's working format.
    RGBA16F,  ///< 16-bit float RGBA, 8 bytes/pixel — HDR intermediate.
    NV12,     ///< 4:2:0 Y + interleaved CbCr, 12 bits/pixel — common HW-decode output.
};

/// Bytes occupied by a `width` x `height` frame in `format`. Returns 0 when
/// either dimension is 0 (an empty/degenerate frame).
[[nodiscard]] std::size_t bytesPerFrame(std::uint32_t width, std::uint32_t height,
                                        FrameFormat format) noexcept;

/// Geometry + format describing a frame's identity and memory footprint. Two
/// frames with an equal FrameDesc are interchangeable, so a released pooled
/// frame can satisfy a later acquire() of the same FrameDesc.
struct FrameDesc {
    std::uint32_t width{0};
    std::uint32_t height{0};
    FrameFormat   format{FrameFormat::RGBA8};

    [[nodiscard]] std::size_t byteSize() const noexcept {
        return bytesPerFrame(width, height, format);
    }

    friend bool operator==(const FrameDesc& a, const FrameDesc& b) noexcept {
        return a.width == b.width && a.height == b.height && a.format == b.format;
    }
    friend bool operator!=(const FrameDesc& a, const FrameDesc& b) noexcept {
        return !(a == b);
    }
};

/// Where a frame's memory originates. Pooled frames are owned (and recycled) by
/// the pool; the Imported* variants adopt externally-owned memory zero-copy.
enum class FrameOrigin {
    Pooled,               ///< Allocated and owned by the FramePool.
    ImportedDmaBuf,       ///< Zero-copy adoption of a DMA-BUF fd (VAAPI / Intel / AMD).
    ImportedCudaPointer,  ///< Zero-copy adoption of a CUDA device pointer (NVIDIA NVDEC).
};

[[nodiscard]] constexpr bool isImported(FrameOrigin origin) noexcept {
    return origin != FrameOrigin::Pooled;
}

/// Externally-owned memory to adopt without a copy (see FramePool::acquireImported).
struct ExternalImageSource {
    FrameOrigin origin{FrameOrigin::ImportedDmaBuf};
    int         dmaBufFd{-1};            ///< Valid when origin == ImportedDmaBuf (fd >= 0).
    void*       cudaDevicePtr{nullptr};  ///< Valid when origin == ImportedCudaPointer (non-null).
    /// Optional host-visible mapping of the external memory (may be null). When
    /// provided it becomes the frame's ImageHandle.hostData so software paths and
    /// tests can inspect the pixels without a GPU.
    void*       mappedHostData{nullptr};

    [[nodiscard]] static ExternalImageSource dmaBuf(int fd, void* mapped = nullptr) {
        return ExternalImageSource{FrameOrigin::ImportedDmaBuf, fd, nullptr, mapped};
    }
    [[nodiscard]] static ExternalImageSource cudaPointer(void* ptr, void* mapped = nullptr) {
        return ExternalImageSource{FrameOrigin::ImportedCudaPointer, -1, ptr, mapped};
    }
};

/// Vendor-neutral, copyable view of the image backing a frame — the binding
/// point downstream GPU stages (compositor, effect kernels, encoder bridge)
/// consume. In a Vulkan build it carries the VkImage imported from the external
/// or pooled allocation; in the software build the host pointer is the frame
/// representation. The external identity (fd / CUDA pointer) is retained so a
/// device-backed build can (re)import it.
struct ImageHandle {
    FrameOrigin origin{FrameOrigin::Pooled};
    void*       hostData{nullptr};         ///< Host-memory backing / mapping (may be null for pure GPU imports).
    int         dmaBufFd{-1};              ///< External DMA-BUF fd (origin == ImportedDmaBuf).
    void*       cudaDevicePtr{nullptr};    ///< External CUDA device pointer (origin == ImportedCudaPointer).
#if defined(PALMIER_HAVE_VULKAN)
    VkImage        vkImage{VK_NULL_HANDLE};  ///< Image imported without copy (device-backed builds).
    VkDeviceMemory vkMemory{VK_NULL_HANDLE}; ///< Backing memory of vkImage.
#endif

    [[nodiscard]] bool isZeroCopy() const noexcept { return isImported(origin); }

    /// A handle is valid when it can actually be bound: a live external
    /// reference for imports, or a host/GPU backing for pooled frames.
    [[nodiscard]] bool valid() const noexcept {
        switch (origin) {
            case FrameOrigin::ImportedDmaBuf:      return dmaBufFd >= 0;
            case FrameOrigin::ImportedCudaPointer: return cudaDevicePtr != nullptr;
            case FrameOrigin::Pooled:
#if defined(PALMIER_HAVE_VULKAN)
                return hostData != nullptr || vkImage != VK_NULL_HANDLE;
#else
                return hostData != nullptr;
#endif
        }
        return false;
    }
};

/// A GPU-resident frame owned by a FramePool. Not copyable or movable by callers
/// — the pool hands out FrameLease references to it. Pooled frames own a
/// host-memory backing buffer (the software frame representation) sized to the
/// FrameDesc; imported frames reference externally-owned memory and own nothing.
class GpuFrame {
public:
    GpuFrame(const GpuFrame&) = delete;
    GpuFrame& operator=(const GpuFrame&) = delete;

    [[nodiscard]] const FrameDesc& desc() const noexcept { return desc_; }
    [[nodiscard]] FrameOrigin origin() const noexcept { return handle_.origin; }
    [[nodiscard]] bool isZeroCopy() const noexcept { return handle_.isZeroCopy(); }

    /// Bytes this frame contributes to the pool's allocation budget: the backing
    /// size for pooled frames, and 0 for imports (their memory is externally
    /// owned).
    [[nodiscard]] std::size_t poolBytes() const noexcept {
        return isZeroCopy() ? 0 : desc_.byteSize();
    }

    /// The binding handle other GPU stages consume.
    [[nodiscard]] const ImageHandle& image() const noexcept { return handle_; }

    /// Mutable host-memory backing for pooled frames (null for pure GPU imports).
    /// Present so CPU fallback paths and tests can read/write pixels.
    [[nodiscard]] void* hostData() noexcept { return handle_.hostData; }
    [[nodiscard]] const void* hostData() const noexcept { return handle_.hostData; }

private:
    friend class FramePool;
    GpuFrame() = default;

    FrameDesc               desc_{};
    ImageHandle             handle_{};
    std::vector<std::byte>  backing_{}; ///< Owned host memory for pooled frames.
    bool                    inUse_{false};
};

class FramePool;

/// Move-only RAII lease of a GpuFrame. Returns the frame to its pool on
/// destruction (or on an explicit release()). A lease must not outlive the pool
/// that created it.
class FrameLease {
public:
    FrameLease() = default;
    FrameLease(const FrameLease&) = delete;
    FrameLease& operator=(const FrameLease&) = delete;
    FrameLease(FrameLease&& other) noexcept;
    FrameLease& operator=(FrameLease&& other) noexcept;
    ~FrameLease();

    [[nodiscard]] bool valid() const noexcept { return pool_ != nullptr && frame_ != nullptr; }
    explicit operator bool() const noexcept { return valid(); }

    /// The leased frame. Precondition: valid().
    [[nodiscard]] GpuFrame& frame() const noexcept { return *frame_; }
    [[nodiscard]] GpuFrame* operator->() const noexcept { return frame_; }
    [[nodiscard]] GpuFrame& operator*() const noexcept { return *frame_; }

    /// Return the frame to the pool early. Idempotent; leaves the lease empty.
    void release() noexcept;

private:
    friend class FramePool;
    FrameLease(FramePool* pool, GpuFrame* frame) noexcept : pool_(pool), frame_(frame) {}

    FramePool* pool_{nullptr};
    GpuFrame*  frame_{nullptr};
};

/// Snapshot of pool occupancy for observability and tests. All byte figures
/// refer to pool-owned (pooled) memory except `importedBytesInUse`.
struct FramePoolStats {
    std::size_t budgetBytes{0};        ///< Allocation cap (from GpuCaps.vramBytes).
    std::size_t reservedBytes{0};      ///< Pooled memory currently allocated (in-use + free list).
    std::size_t inUseBytes{0};         ///< Pooled memory currently leased out.
    std::size_t freeBytes{0};          ///< Pooled memory sitting in the reuse free list.
    std::size_t peakInUseBytes{0};     ///< High-water mark of pooled in-use bytes.
    std::size_t framesInUse{0};        ///< Pooled frames currently leased.
    std::size_t framesFree{0};         ///< Pooled frames available for reuse.
    std::size_t importedInUse{0};      ///< Zero-copy imported frames currently leased.
    std::size_t importedBytesInUse{0}; ///< Footprint of imported frames (informational; not budgeted).
};

/// A pool of GPU-resident frames bounded by a VRAM byte budget (Requirement
/// 10.2). See the file header for the pooled-vs-imported model.
class FramePool {
public:
    /// Create a pool that will allocate at most `vramBudgetBytes` of pooled
    /// frame memory. Callers typically pass GpuCaps.vramBytes (or a host-memory
    /// budget on the software path). A budget of 0 means "no pooled allocation
    /// is permitted" — only zero-copy imports can be acquired.
    explicit FramePool(std::size_t vramBudgetBytes) noexcept;

    ~FramePool();
    FramePool(const FramePool&) = delete;
    FramePool& operator=(const FramePool&) = delete;
    FramePool(FramePool&&) = delete;   // leases hold raw back-pointers.
    FramePool& operator=(FramePool&&) = delete;

    /// Acquire a pooled frame matching `desc`. Reuses a released frame of the
    /// same geometry when one is available; otherwise allocates a new one,
    /// reclaiming free frames first if needed to stay within budget. Fails with:
    ///   * InvalidArgument  — degenerate desc (zero width/height).
    ///   * OutOfRange       — the frame cannot fit within the VRAM budget even
    ///                        after reclaiming all free frames.
    [[nodiscard]] Result<FrameLease> acquire(const FrameDesc& desc);

    /// Adopt an externally-owned frame (DMA-BUF fd or CUDA device pointer)
    /// zero-copy. The import does not draw down the allocation budget. Fails with
    /// InvalidArgument for a degenerate desc or a source that does not carry a
    /// valid handle for its declared origin.
    [[nodiscard]] Result<FrameLease> acquireImported(const FrameDesc& desc,
                                                      const ExternalImageSource& source);

    /// Total pooled-allocation cap in bytes.
    [[nodiscard]] std::size_t budgetBytes() const noexcept { return budgetBytes_; }

    /// Current occupancy snapshot.
    [[nodiscard]] FramePoolStats stats() const noexcept;

    /// Destroy all free (unleased) pooled frames, returning their bytes to the
    /// budget. In-use frames are untouched. Returns the number of bytes freed.
    std::size_t trim() noexcept;

private:
    friend class FrameLease;

    // Called by FrameLease to return a frame. Pooled frames go to the free list;
    // imported frames are destroyed.
    void releaseFrame(GpuFrame* frame) noexcept;

    // Allocate a brand-new pooled frame of `desc` (host-memory backing), or
    // nullptr if it cannot fit within budget after reclaiming free frames.
    [[nodiscard]] std::unique_ptr<GpuFrame> allocatePooled(const FrameDesc& desc);

    // Evict free frames (oldest first) until at least `needBytes` of budget
    // headroom is available or the free list is empty. Returns bytes reclaimed.
    std::size_t reclaimFreeUpTo(std::size_t needBytes) noexcept;

    std::size_t budgetBytes_{0};
    std::size_t reservedBytes_{0};       // pooled bytes allocated (free + in-use)
    std::size_t inUseBytes_{0};          // pooled bytes leased out
    std::size_t peakInUseBytes_{0};
    std::size_t importedBytesInUse_{0};
    std::size_t importedInUse_{0};

    std::vector<std::unique_ptr<GpuFrame>> free_;  // pooled, released, reusable
    std::vector<std::unique_ptr<GpuFrame>> live_;  // currently leased (pooled + imported)
};

} // namespace palmier::gpu

#endif // PALMIER_GPU_FRAMEPOOL_HPP
