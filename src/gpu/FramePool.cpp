// SPDX-License-Identifier: GPL-3.0-or-later
//
// gpu/FramePool.cpp — implementation of the zero-copy, VRAM-capped frame pool.
//
// See FramePool.hpp for the contract and the pooled-vs-imported model. The pool
// logic (budgeting, reuse free list, reclamation, zero-copy adoption) is
// vendor-neutral and fully exercisable without a GPU: pooled frames use a
// host-memory backing buffer as their frame representation. The Vulkan-specific
// image fields on ImageHandle are compiled only under PALMIER_HAVE_VULKAN and
// are populated by the device-backed integration; the accounting here is
// identical either way.

#include "gpu/FramePool.hpp"

#include <algorithm>
#include <cstdint>
#include <new>
#include <utility>

#include "core/Error.hpp"

namespace palmier::gpu {

// ---------------------------------------------------------------------------
// Footprint helper.
// ---------------------------------------------------------------------------
std::size_t bytesPerFrame(std::uint32_t width, std::uint32_t height,
                          FrameFormat format) noexcept {
    if (width == 0 || height == 0) return 0;
    const std::size_t pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    switch (format) {
        case FrameFormat::RGBA8:   return pixels * 4;
        case FrameFormat::RGBA16F: return pixels * 8;
        case FrameFormat::NV12:    return pixels * 3 / 2; // 12 bits/pixel
    }
    return pixels * 4;
}

// ---------------------------------------------------------------------------
// FrameLease — move-only RAII wrapper.
// ---------------------------------------------------------------------------
FrameLease::FrameLease(FrameLease&& other) noexcept
    : pool_(other.pool_), frame_(other.frame_) {
    other.pool_ = nullptr;
    other.frame_ = nullptr;
}

FrameLease& FrameLease::operator=(FrameLease&& other) noexcept {
    if (this != &other) {
        release();
        pool_ = other.pool_;
        frame_ = other.frame_;
        other.pool_ = nullptr;
        other.frame_ = nullptr;
    }
    return *this;
}

FrameLease::~FrameLease() { release(); }

void FrameLease::release() noexcept {
    if (pool_ != nullptr && frame_ != nullptr) {
        pool_->releaseFrame(frame_);
    }
    pool_ = nullptr;
    frame_ = nullptr;
}

// ---------------------------------------------------------------------------
// FramePool.
// ---------------------------------------------------------------------------
FramePool::FramePool(std::size_t vramBudgetBytes) noexcept
    : budgetBytes_(vramBudgetBytes) {}

FramePool::~FramePool() = default;

std::size_t FramePool::reclaimFreeUpTo(std::size_t needBytes) noexcept {
    std::size_t reclaimed = 0;
    // Evict oldest free frames first (front of the vector) until we have enough
    // headroom or nothing is left to reclaim.
    while (reclaimed < needBytes && !free_.empty()) {
        const std::size_t sz = free_.front()->poolBytes();
        free_.erase(free_.begin());
        reservedBytes_ -= sz;
        reclaimed += sz;
    }
    return reclaimed;
}

std::unique_ptr<GpuFrame> FramePool::allocatePooled(const FrameDesc& desc) {
    const std::size_t size = desc.byteSize();

    // Ensure the new allocation fits within budget, reclaiming free frames if
    // the current reservation would overflow.
    if (reservedBytes_ + size > budgetBytes_) {
        const std::size_t deficit = (reservedBytes_ + size) - budgetBytes_;
        reclaimFreeUpTo(deficit);
        if (reservedBytes_ + size > budgetBytes_) {
            return nullptr; // cannot fit even after reclaiming everything free
        }
    }

    auto frame = std::unique_ptr<GpuFrame>(new (std::nothrow) GpuFrame());
    if (!frame) return nullptr;
    frame->desc_ = desc;
    frame->handle_.origin = FrameOrigin::Pooled;
    try {
        frame->backing_.resize(size);
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
    frame->handle_.hostData = frame->backing_.data();

    reservedBytes_ += size;
    return frame;
}

Result<FrameLease> FramePool::acquire(const FrameDesc& desc) {
    if (desc.width == 0 || desc.height == 0) {
        return err<FrameLease>(invalidArgument("FramePool::acquire: frame has zero dimensions"));
    }
    const std::size_t size = desc.byteSize();

    // 1) Reuse a released frame of identical geometry (no new allocation).
    for (auto it = free_.begin(); it != free_.end(); ++it) {
        if ((*it)->desc_ == desc) {
            std::unique_ptr<GpuFrame> frame = std::move(*it);
            free_.erase(it);
            frame->inUse_ = true;
            GpuFrame* raw = frame.get();
            live_.push_back(std::move(frame));
            inUseBytes_ += size;
            peakInUseBytes_ = std::max(peakInUseBytes_, inUseBytes_);
            return FrameLease(this, raw);
        }
    }

    // 2) Allocate a new pooled frame, honoring the budget.
    std::unique_ptr<GpuFrame> frame = allocatePooled(desc);
    if (!frame) {
        return err<FrameLease>(outOfRange(
            "FramePool::acquire: frame does not fit within the VRAM budget"));
    }
    frame->inUse_ = true;
    GpuFrame* raw = frame.get();
    live_.push_back(std::move(frame));
    inUseBytes_ += size;
    peakInUseBytes_ = std::max(peakInUseBytes_, inUseBytes_);
    return FrameLease(this, raw);
}

Result<FrameLease> FramePool::acquireImported(const FrameDesc& desc,
                                              const ExternalImageSource& source) {
    if (desc.width == 0 || desc.height == 0) {
        return err<FrameLease>(
            invalidArgument("FramePool::acquireImported: frame has zero dimensions"));
    }
    switch (source.origin) {
        case FrameOrigin::ImportedDmaBuf:
            if (source.dmaBufFd < 0) {
                return err<FrameLease>(invalidArgument(
                    "FramePool::acquireImported: DMA-BUF import requires a valid fd"));
            }
            break;
        case FrameOrigin::ImportedCudaPointer:
            if (source.cudaDevicePtr == nullptr) {
                return err<FrameLease>(invalidArgument(
                    "FramePool::acquireImported: CUDA import requires a non-null device pointer"));
            }
            break;
        case FrameOrigin::Pooled:
            return err<FrameLease>(invalidArgument(
                "FramePool::acquireImported: source origin must be an imported kind"));
    }

    auto frame = std::unique_ptr<GpuFrame>(new (std::nothrow) GpuFrame());
    if (!frame) {
        return err<FrameLease>(makeError(ErrorCode::Internal,
                                         "FramePool::acquireImported: allocation failed"));
    }
    frame->desc_ = desc;
    frame->inUse_ = true;
    frame->handle_.origin = source.origin;
    frame->handle_.hostData = source.mappedHostData;
    frame->handle_.dmaBufFd = source.dmaBufFd;
    frame->handle_.cudaDevicePtr = source.cudaDevicePtr;

    GpuFrame* raw = frame.get();
    live_.push_back(std::move(frame));

    importedInUse_ += 1;
    importedBytesInUse_ += desc.byteSize();
    return FrameLease(this, raw);
}

void FramePool::releaseFrame(GpuFrame* frame) noexcept {
    if (frame == nullptr) return;

    auto it = std::find_if(live_.begin(), live_.end(),
                           [frame](const std::unique_ptr<GpuFrame>& f) { return f.get() == frame; });
    if (it == live_.end()) return; // not ours / already released — ignore defensively.

    std::unique_ptr<GpuFrame> owned = std::move(*it);
    live_.erase(it);
    owned->inUse_ = false;

    if (owned->isZeroCopy()) {
        // Imported memory is externally owned: drop our reference, do not recycle.
        importedInUse_ = (importedInUse_ > 0) ? importedInUse_ - 1 : 0;
        const std::size_t sz = owned->desc_.byteSize();
        importedBytesInUse_ = (importedBytesInUse_ >= sz) ? importedBytesInUse_ - sz : 0;
        return; // owned is destroyed here
    }

    // Pooled frame: return it to the reuse free list; its bytes stay reserved.
    const std::size_t sz = owned->desc_.byteSize();
    inUseBytes_ = (inUseBytes_ >= sz) ? inUseBytes_ - sz : 0;
    free_.push_back(std::move(owned));
}

std::size_t FramePool::trim() noexcept {
    std::size_t freed = 0;
    for (auto& f : free_) {
        freed += f->poolBytes();
    }
    reservedBytes_ = (reservedBytes_ >= freed) ? reservedBytes_ - freed : 0;
    free_.clear();
    return freed;
}

FramePoolStats FramePool::stats() const noexcept {
    FramePoolStats s;
    s.budgetBytes = budgetBytes_;
    s.reservedBytes = reservedBytes_;
    s.inUseBytes = inUseBytes_;
    s.freeBytes = (reservedBytes_ >= inUseBytes_) ? reservedBytes_ - inUseBytes_ : 0;
    s.peakInUseBytes = peakInUseBytes_;
    s.framesFree = free_.size();
    s.importedInUse = importedInUse_;
    s.importedBytesInUse = importedBytesInUse_;
    // framesInUse counts leased *pooled* frames only.
    std::size_t pooledLive = 0;
    for (const auto& f : live_) {
        if (!f->isZeroCopy()) ++pooledLive;
    }
    s.framesInUse = pooledLive;
    return s;
}

} // namespace palmier::gpu
