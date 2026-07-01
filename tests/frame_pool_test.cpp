// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/frame_pool_test.cpp — unit tests for the zero-copy, VRAM-capped
// FramePool (task 7.2, Requirement 10.2).
//
// The pool is vendor-neutral and runs entirely on host memory in this build
// (no Vulkan loader / GPU required), so these tests exercise the real budgeting,
// reuse, reclamation, and zero-copy import logic directly. They also verify the
// GpuContext::framePool() accessor and its VRAM-derived budget.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "gpu/FramePool.hpp"
#include "gpu/GpuContext.hpp"
#include "gpu/GpuTypes.hpp"

namespace palmier::gpu {
namespace {

constexpr std::size_t kMiB = std::size_t{1024} * 1024;

FrameDesc hd(FrameFormat fmt = FrameFormat::RGBA8) {
    return FrameDesc{1920, 1080, fmt};
}

// --- Footprint --------------------------------------------------------------

TEST(BytesPerFrame, MatchesFormatFootprint) {
    EXPECT_EQ(bytesPerFrame(1920, 1080, FrameFormat::RGBA8), 1920u * 1080u * 4u);
    EXPECT_EQ(bytesPerFrame(1920, 1080, FrameFormat::RGBA16F), 1920u * 1080u * 8u);
    EXPECT_EQ(bytesPerFrame(1920, 1080, FrameFormat::NV12), 1920u * 1080u * 3u / 2u);
    EXPECT_EQ(bytesPerFrame(0, 1080, FrameFormat::RGBA8), 0u);
    EXPECT_EQ(bytesPerFrame(1920, 0, FrameFormat::RGBA8), 0u);
}

// --- Basic acquire / release ------------------------------------------------

TEST(FramePool, AcquireAllocatesAndAccountsBytes) {
    FramePool pool(64 * kMiB);
    const std::size_t frameBytes = hd().byteSize();

    auto lease = pool.acquire(hd());
    ASSERT_TRUE(lease.isOk());
    ASSERT_TRUE(lease.value().valid());
    EXPECT_EQ(lease.value()->origin(), FrameOrigin::Pooled);
    EXPECT_FALSE(lease.value()->isZeroCopy());
    EXPECT_TRUE(lease.value()->image().valid());
    ASSERT_NE(lease.value()->hostData(), nullptr);

    const auto s = pool.stats();
    EXPECT_EQ(s.inUseBytes, frameBytes);
    EXPECT_EQ(s.reservedBytes, frameBytes);
    EXPECT_EQ(s.framesInUse, 1u);
}

TEST(FramePool, RejectsDegenerateDescriptor) {
    FramePool pool(64 * kMiB);
    auto lease = pool.acquire(FrameDesc{0, 0, FrameFormat::RGBA8});
    ASSERT_TRUE(lease.isError());
    EXPECT_EQ(lease.error().code(), ErrorCode::InvalidArgument);
}

TEST(FramePool, ReleaseReturnsBytesToFreeListAndReuses) {
    FramePool pool(64 * kMiB);
    const std::size_t frameBytes = hd().byteSize();

    void* firstBacking = nullptr;
    {
        auto lease = pool.acquire(hd());
        ASSERT_TRUE(lease.isOk());
        firstBacking = lease.value()->hostData();
        EXPECT_EQ(pool.stats().inUseBytes, frameBytes);
    } // lease destroyed -> released

    auto afterRelease = pool.stats();
    EXPECT_EQ(afterRelease.inUseBytes, 0u);
    EXPECT_EQ(afterRelease.reservedBytes, frameBytes); // still reserved for reuse
    EXPECT_EQ(afterRelease.framesFree, 1u);

    // Re-acquiring the same geometry reuses the freed frame (no new allocation).
    auto reused = pool.acquire(hd());
    ASSERT_TRUE(reused.isOk());
    EXPECT_EQ(reused.value()->hostData(), firstBacking); // same backing buffer
    EXPECT_EQ(pool.stats().reservedBytes, frameBytes);   // unchanged
    EXPECT_EQ(pool.stats().framesFree, 0u);
}

TEST(FramePool, PeakInUseTracksHighWaterMark) {
    FramePool pool(64 * kMiB);
    const std::size_t frameBytes = hd().byteSize();
    {
        auto a = pool.acquire(hd());
        auto b = pool.acquire(hd());
        ASSERT_TRUE(a.isOk());
        ASSERT_TRUE(b.isOk());
        EXPECT_EQ(pool.stats().inUseBytes, 2 * frameBytes);
    }
    EXPECT_EQ(pool.stats().inUseBytes, 0u);
    EXPECT_EQ(pool.stats().peakInUseBytes, 2 * frameBytes);
}

// --- Budget cap (Requirement 10.2) ------------------------------------------

TEST(FramePool, CapsAllocationByBudget) {
    const std::size_t frameBytes = hd().byteSize();
    // Budget for exactly two concurrent frames.
    FramePool pool(2 * frameBytes);

    auto a = pool.acquire(hd());
    auto b = pool.acquire(hd());
    ASSERT_TRUE(a.isOk());
    ASSERT_TRUE(b.isOk());

    // A third concurrent frame exceeds the budget (nothing free to reclaim).
    auto c = pool.acquire(hd());
    ASSERT_TRUE(c.isError());
    EXPECT_EQ(c.error().code(), ErrorCode::OutOfRange);
    EXPECT_EQ(pool.stats().inUseBytes, 2 * frameBytes);
}

TEST(FramePool, ReclaimsFreeFramesToFitNewGeometry) {
    // Budget holds exactly one HD frame at a time.
    const std::size_t frameBytes = hd().byteSize();
    FramePool pool(frameBytes);

    // Allocate and release an RGBA8 frame; it sits in the free list, reserved.
    { auto l = pool.acquire(hd(FrameFormat::RGBA8)); ASSERT_TRUE(l.isOk()); }
    EXPECT_EQ(pool.stats().reservedBytes, frameBytes);
    EXPECT_EQ(pool.stats().framesFree, 1u);

    // A different geometry (RGBA16F is larger) cannot be satisfied by reuse and
    // would overflow the budget; the pool must reclaim the free RGBA8 frame,
    // but the larger frame still does not fit -> OutOfRange.
    auto big = pool.acquire(hd(FrameFormat::RGBA16F));
    ASSERT_TRUE(big.isError());
    EXPECT_EQ(big.error().code(), ErrorCode::OutOfRange);

    // A same-size (RGBA8) frame of *different* dimensions forces reclamation of
    // the mismatched free frame, then fits within budget.
    FramePool pool2(frameBytes);
    { auto l = pool2.acquire(FrameDesc{1280, 720, FrameFormat::RGBA8}); ASSERT_TRUE(l.isOk()); }
    auto other = pool2.acquire(hd()); // 1080p needs the 720p frame reclaimed first
    ASSERT_TRUE(other.isOk());
    EXPECT_EQ(pool2.stats().reservedBytes, frameBytes);
    EXPECT_EQ(pool2.stats().framesFree, 0u);
}

TEST(FramePool, TrimReleasesFreeReservations) {
    FramePool pool(64 * kMiB);
    const std::size_t frameBytes = hd().byteSize();
    { auto l = pool.acquire(hd()); ASSERT_TRUE(l.isOk()); }
    EXPECT_EQ(pool.stats().reservedBytes, frameBytes);

    const std::size_t freed = pool.trim();
    EXPECT_EQ(freed, frameBytes);
    EXPECT_EQ(pool.stats().reservedBytes, 0u);
    EXPECT_EQ(pool.stats().framesFree, 0u);
}

// --- Zero-copy imports ------------------------------------------------------

TEST(FramePool, ImportedDmaBufIsZeroCopyAndNotBudgeted) {
    FramePool pool(0); // zero budget: only imports are possible
    std::vector<std::byte> external(hd().byteSize());

    auto lease = pool.acquireImported(hd(),
                                      ExternalImageSource::dmaBuf(/*fd=*/7, external.data()));
    ASSERT_TRUE(lease.isOk());
    EXPECT_TRUE(lease.value()->isZeroCopy());
    EXPECT_EQ(lease.value()->origin(), FrameOrigin::ImportedDmaBuf);
    EXPECT_EQ(lease.value()->image().dmaBufFd, 7);
    EXPECT_EQ(lease.value()->hostData(), external.data());
    EXPECT_EQ(lease.value()->poolBytes(), 0u); // imports do not draw the budget
    EXPECT_TRUE(lease.value()->image().valid());

    const auto s = pool.stats();
    EXPECT_EQ(s.importedInUse, 1u);
    EXPECT_EQ(s.reservedBytes, 0u); // no pooled allocation occurred
}

TEST(FramePool, ImportedCudaPointerAdopted) {
    FramePool pool(0);
    int deviceMem = 0;
    auto lease = pool.acquireImported(
        hd(), ExternalImageSource::cudaPointer(&deviceMem));
    ASSERT_TRUE(lease.isOk());
    EXPECT_EQ(lease.value()->origin(), FrameOrigin::ImportedCudaPointer);
    EXPECT_EQ(lease.value()->image().cudaDevicePtr, &deviceMem);
}

TEST(FramePool, RejectsInvalidImportSources) {
    FramePool pool(0);
    // DMA-BUF with no fd.
    auto bad1 = pool.acquireImported(hd(), ExternalImageSource::dmaBuf(-1));
    ASSERT_TRUE(bad1.isError());
    EXPECT_EQ(bad1.error().code(), ErrorCode::InvalidArgument);

    // CUDA with null pointer.
    auto bad2 = pool.acquireImported(hd(), ExternalImageSource::cudaPointer(nullptr));
    ASSERT_TRUE(bad2.isError());
    EXPECT_EQ(bad2.error().code(), ErrorCode::InvalidArgument);

    // Pooled origin is not a valid import kind.
    ExternalImageSource pooledSrc;
    pooledSrc.origin = FrameOrigin::Pooled;
    auto bad3 = pool.acquireImported(hd(), pooledSrc);
    ASSERT_TRUE(bad3.isError());
    EXPECT_EQ(bad3.error().code(), ErrorCode::InvalidArgument);
}

TEST(FramePool, ImportedFrameNotRecycledOnRelease) {
    FramePool pool(0);
    int deviceMem = 0;
    {
        auto lease = pool.acquireImported(
            hd(), ExternalImageSource::cudaPointer(&deviceMem));
        ASSERT_TRUE(lease.isOk());
        EXPECT_EQ(pool.stats().importedInUse, 1u);
    }
    // Released imports are dropped, not added to the reuse free list.
    const auto s = pool.stats();
    EXPECT_EQ(s.importedInUse, 0u);
    EXPECT_EQ(s.framesFree, 0u);
}

// --- Lease move semantics ---------------------------------------------------

TEST(FrameLease, MoveTransfersOwnershipWithoutDoubleRelease) {
    FramePool pool(64 * kMiB);
    const std::size_t frameBytes = hd().byteSize();

    auto a = pool.acquire(hd());
    ASSERT_TRUE(a.isOk());
    FrameLease moved = std::move(a.value());
    EXPECT_TRUE(moved.valid());
    EXPECT_EQ(pool.stats().inUseBytes, frameBytes);

    moved.release();
    EXPECT_FALSE(moved.valid());
    EXPECT_EQ(pool.stats().inUseBytes, 0u);
    EXPECT_EQ(pool.stats().framesFree, 1u);

    // A second release is a harmless no-op.
    moved.release();
    EXPECT_EQ(pool.stats().framesFree, 1u);
}

// --- GpuContext integration -------------------------------------------------

TEST(GpuContextFramePool, SoftwareFallbackUsesHostBudget) {
    auto ctx = GpuContext::softwareFallback();
    FramePool& pool = ctx.framePool();
    EXPECT_GT(pool.budgetBytes(), 0u); // host budget even with 0 VRAM
    auto lease = pool.acquire(hd());
    EXPECT_TRUE(lease.isOk());
}

TEST(GpuContextFramePool, BudgetDerivedFromVramWhenPresent) {
    const std::vector<GpuDeviceInfo> devices = [] {
        GpuDeviceInfo d;
        d.index = 0;
        d.name = "NVIDIA RTX";
        d.vendor = GpuVendor::NVIDIA;
        d.type = GpuDeviceType::DiscreteGpu;
        d.caps.vendorId = GpuVendor::NVIDIA;
        d.caps.supportsCompute = true;
        d.caps.vramBytes = 8ull * 1024 * 1024 * 1024;
        return std::vector<GpuDeviceInfo>{d};
    }();

    auto ctx = GpuContext::createWith(GpuSelectionPolicy::automatic(),
                                      [devices]() { return devices; }, nullptr);
    ASSERT_TRUE(ctx.isOk());
    EXPECT_FALSE(ctx.value().isSoftwareFallback());
    EXPECT_EQ(ctx.value().framePool().budgetBytes(), 8ull * 1024 * 1024 * 1024);
}

} // namespace
} // namespace palmier::gpu
