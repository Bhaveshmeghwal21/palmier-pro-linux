// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/gpu_context_test.cpp — unit tests for GPU device selection, capability
// probing, the software fallback, and persisted GPU selection (task 7.1).
//
// These tests drive GpuContext through an injected synthetic device enumerator
// so they run on machines with no Vulkan loader or physical GPU (Requirements
// 10.1, 10.4, 10.6). They exercise the selection scoring, the never-throw
// "no GPU" degradation, and persistence round-tripping across "restarts".

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "gpu/GpuContext.hpp"
#include "gpu/GpuSelection.hpp"
#include "gpu/GpuSelectionStore.hpp"
#include "gpu/GpuTypes.hpp"

namespace palmier::gpu {
namespace {

// --- Synthetic device builders --------------------------------------------

GpuDeviceInfo makeDevice(int index, std::string name, GpuVendor vendor,
                         GpuDeviceType type, bool compute, bool hwDecode,
                         bool hwEncode, std::size_t vramBytes) {
    GpuDeviceInfo d;
    d.index = index;
    d.name = std::move(name);
    d.vendor = vendor;
    d.type = type;
    d.caps.vendorId = vendor;
    d.caps.vendor = std::string{vendorName(vendor)};
    d.caps.supportsCompute = compute;
    d.caps.hwDecode = hwDecode;
    d.caps.hwEncode = hwEncode;
    d.caps.vramBytes = vramBytes;
    return d;
}

GpuDeviceInfo discreteNvidia(int index = 0) {
    return makeDevice(index, "NVIDIA RTX", GpuVendor::NVIDIA,
                      GpuDeviceType::DiscreteGpu, true, true, true,
                      8ull * 1024 * 1024 * 1024);
}

GpuDeviceInfo integratedIntel(int index = 1) {
    return makeDevice(index, "Intel iGPU", GpuVendor::Intel,
                      GpuDeviceType::IntegratedGpu, true, true, false,
                      512ull * 1024 * 1024);
}

PhysicalDeviceEnumerator fixedList(std::vector<GpuDeviceInfo> devices) {
    return [devices = std::move(devices)]() { return devices; };
}

std::string tempConfigPath(const std::string& tag) {
    auto p = std::filesystem::temp_directory_path() /
             ("palmier_gpu_test_" + tag + "_" +
              std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
              std::to_string(reinterpret_cast<std::uintptr_t>(&tag)) + ".conf");
    return p.string();
}

// --- Pure selection algorithm ----------------------------------------------

TEST(GpuSelection, EmptyDeviceListSelectsSoftware) {
    EXPECT_FALSE(selectDevice({}, GpuSelectionPolicy::automatic()).has_value());
}

TEST(GpuSelection, ForceSoftwareAlwaysSelectsSoftware) {
    const std::vector<GpuDeviceInfo> devices{discreteNvidia(), integratedIntel()};
    EXPECT_FALSE(selectDevice(devices, GpuSelectionPolicy::forceSoftware()).has_value());
}

TEST(GpuSelection, AutoPrefersDiscreteOverIntegrated) {
    const std::vector<GpuDeviceInfo> devices{integratedIntel(0), discreteNvidia(1)};
    const auto chosen = selectDevice(devices, GpuSelectionPolicy::automatic());
    ASSERT_TRUE(chosen.has_value());
    EXPECT_EQ(*chosen, 1); // the discrete NVIDIA device
}

TEST(GpuSelection, HardwareEncodeWinsAmongSameClass) {
    auto encoder = makeDevice(0, "A", GpuVendor::AMD, GpuDeviceType::DiscreteGpu,
                              true, true, true, 4ull * 1024 * 1024 * 1024);
    auto decoderOnly = makeDevice(1, "B", GpuVendor::AMD, GpuDeviceType::DiscreteGpu,
                                  true, true, false, 4ull * 1024 * 1024 * 1024);
    const auto chosen = selectDevice({decoderOnly, encoder}, GpuSelectionPolicy::automatic());
    ASSERT_TRUE(chosen.has_value());
    EXPECT_EQ(*chosen, 0); // the hardware-encode-capable device
}

TEST(GpuSelection, PreferVendorBiasesButFallsBack) {
    const std::vector<GpuDeviceInfo> devices{discreteNvidia(0), integratedIntel(1)};
    // Prefer Intel even though NVIDIA scores higher on the merits.
    auto chosen = selectDevice(devices, GpuSelectionPolicy::preferVendor(GpuVendor::Intel));
    ASSERT_TRUE(chosen.has_value());
    EXPECT_EQ(*chosen, 1);

    // Prefer AMD (absent) -> falls back to the best available (NVIDIA).
    chosen = selectDevice(devices, GpuSelectionPolicy::preferVendor(GpuVendor::AMD));
    ASSERT_TRUE(chosen.has_value());
    EXPECT_EQ(*chosen, 0);
}

TEST(GpuSelection, ForceIndexHonoredOrDegrades) {
    const std::vector<GpuDeviceInfo> devices{discreteNvidia(0), integratedIntel(1)};
    auto chosen = selectDevice(devices, GpuSelectionPolicy::forceIndex(1));
    ASSERT_TRUE(chosen.has_value());
    EXPECT_EQ(*chosen, 1);

    // Out-of-range forced index -> software fallback (never throws).
    EXPECT_FALSE(selectDevice(devices, GpuSelectionPolicy::forceIndex(9)).has_value());
}

TEST(GpuSelection, ComputeIncapableDeviceIsNeverSelected) {
    auto noCompute = makeDevice(0, "NoCompute", GpuVendor::NVIDIA,
                                GpuDeviceType::DiscreteGpu, /*compute=*/false,
                                true, true, 8ull * 1024 * 1024 * 1024);
    EXPECT_FALSE(selectDevice({noCompute}, GpuSelectionPolicy::automatic()).has_value());
}

// --- GpuContext creation ----------------------------------------------------

TEST(GpuContext, NoDevicesDegradesToSoftwareWithNotice) {
    auto ctx = GpuContext::createWith(GpuSelectionPolicy::automatic(),
                                      fixedList({}), nullptr);
    ASSERT_TRUE(ctx.isOk());
    EXPECT_TRUE(ctx.value().isSoftwareFallback());
    EXPECT_EQ(ctx.value().selectedDeviceIndex(), -1);
    EXPECT_EQ(ctx.value().capabilities().vendorId, GpuVendor::Software);
    EXPECT_FALSE(ctx.value().capabilities().supportsCompute);
    EXPECT_TRUE(ctx.value().unavailableNotice().has_value());
}

TEST(GpuContext, ForceSoftwareHasNoUnavailableNotice) {
    auto ctx = GpuContext::createWith(GpuSelectionPolicy::forceSoftware(),
                                      fixedList({discreteNvidia()}), nullptr);
    ASSERT_TRUE(ctx.isOk());
    EXPECT_TRUE(ctx.value().isSoftwareFallback());
    EXPECT_FALSE(ctx.value().unavailableNotice().has_value());
}

TEST(GpuContext, SelectsDiscreteAndExposesCaps) {
    auto ctx = GpuContext::createWith(GpuSelectionPolicy::automatic(),
                                      fixedList({integratedIntel(0), discreteNvidia(1)}),
                                      nullptr);
    ASSERT_TRUE(ctx.isOk());
    const auto& c = ctx.value();
    EXPECT_FALSE(c.isSoftwareFallback());
    EXPECT_EQ(c.selectedDeviceIndex(), 1);
    EXPECT_EQ(c.capabilities().vendorId, GpuVendor::NVIDIA);
    EXPECT_TRUE(c.capabilities().supportsCompute);
    EXPECT_EQ(c.availableDevices().size(), 2u);
    EXPECT_FALSE(c.unavailableNotice().has_value());
}

TEST(GpuContext, DetectionBudgetExceededDegradesToSoftware) {
    GpuContextConfig cfg;
    cfg.detectionBudget = std::chrono::milliseconds{5};
    // Enumerator that sleeps beyond the budget.
    PhysicalDeviceEnumerator slow = []() {
        std::this_thread::sleep_for(std::chrono::milliseconds{40});
        return std::vector<GpuDeviceInfo>{discreteNvidia()};
    };
    auto ctx = GpuContext::createWith(GpuSelectionPolicy::automatic(), slow, nullptr, cfg);
    ASSERT_TRUE(ctx.isOk());
    EXPECT_TRUE(ctx.value().isSoftwareFallback());
    EXPECT_TRUE(ctx.value().unavailableNotice().has_value());
}

// --- Persistence ------------------------------------------------------------

TEST(GpuSelectionStore, RoundTripsSelection) {
    const std::string path = tempConfigPath("roundtrip");
    std::filesystem::remove(path);
    GpuSelectionStore store(path);

    // No file yet -> empty (not an error).
    auto loaded = store.load();
    ASSERT_TRUE(loaded.isOk());
    EXPECT_FALSE(loaded.value().has_value());

    const auto rec = GpuSelectionStore::fromDevice(discreteNvidia(1));
    ASSERT_TRUE(store.save(rec).isOk());

    auto reloaded = store.load();
    ASSERT_TRUE(reloaded.isOk());
    ASSERT_TRUE(reloaded.value().has_value());
    EXPECT_EQ(*reloaded.value(), rec);

    std::filesystem::remove(path);
}

TEST(GpuContext, PersistedSelectionReappliedAcrossRestart) {
    const std::string path = tempConfigPath("persist");
    std::filesystem::remove(path);
    GpuSelectionStore store(path);

    const std::vector<GpuDeviceInfo> devices{discreteNvidia(0), integratedIntel(1)};

    // "First run": user forces the integrated GPU (index 1); it is persisted.
    auto first = GpuContext::createWith(GpuSelectionPolicy::forceIndex(1),
                                        fixedList(devices), &store);
    ASSERT_TRUE(first.isOk());
    EXPECT_EQ(first.value().selectedDeviceIndex(), 1);

    // "Restart": Auto policy should honor the persisted choice (index 1) even
    // though the discrete device would otherwise score higher.
    auto second = GpuContext::createWith(GpuSelectionPolicy::automatic(),
                                         fixedList(devices), &store);
    ASSERT_TRUE(second.isOk());
    EXPECT_FALSE(second.value().isSoftwareFallback());
    EXPECT_EQ(second.value().selectedDeviceIndex(), 1);

    std::filesystem::remove(path);
}

TEST(GpuContext, PersistedSoftwareChoiceHonoredOnAuto) {
    const std::string path = tempConfigPath("persistsw");
    std::filesystem::remove(path);
    GpuSelectionStore store(path);
    ASSERT_TRUE(store.save(GpuSelectionStore::softwareSelection()).isOk());

    auto ctx = GpuContext::createWith(GpuSelectionPolicy::automatic(),
                                      fixedList({discreteNvidia()}), &store);
    ASSERT_TRUE(ctx.isOk());
    EXPECT_TRUE(ctx.value().isSoftwareFallback());

    std::filesystem::remove(path);
}

} // namespace
} // namespace palmier::gpu
