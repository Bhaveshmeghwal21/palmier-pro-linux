// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/gpu_capability_probe_test.cpp — focused unit tests for capability
// probing and device selection with mocked device descriptors (task 7.8).
//
// gpu_context_test.cpp already exercises the headline paths (discrete > iGPU,
// hwEncode weighting, no-device / ForceSoftware fallback, ForceIndex,
// compute-incapable skipping, and persistence). This file augments that with
// the finer-grained scoring behaviour the design specifies (Requirements 10.1,
// 10.4, 10.6):
//
//   * VRAM tie-break ordering among otherwise-equal devices, and the VRAM cap
//     that stops a memory-heavy iGPU from leapfrogging a discrete device class.
//   * Deterministic tie-break by lower enumeration index.
//   * hardware-decode weighted above compute-only within the same class.
//   * Mixed multi-GPU scoring across three heterogeneous devices.
//   * PreferVendor tie-breaking: the best device *of* the preferred vendor is
//     chosen, and the vendor bias can beat a stronger non-preferred device.
//   * Direct scoreDevice() ordering, including the 0 score for a
//     compute-incapable device.
//   * Full GpuCaps field population surfaced through GpuContext.
//
// Everything runs against synthetic GpuDeviceInfo descriptors, so no Vulkan
// loader or physical GPU is required.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "gpu/GpuContext.hpp"
#include "gpu/GpuSelection.hpp"
#include "gpu/GpuTypes.hpp"

namespace palmier::gpu {
namespace {

constexpr std::size_t kMiB = 1024ull * 1024ull;
constexpr std::size_t kGiB = 1024ull * kMiB;

// --- Synthetic device builder ----------------------------------------------

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

PhysicalDeviceEnumerator fixedList(std::vector<GpuDeviceInfo> devices) {
    return [devices = std::move(devices)]() { return devices; };
}

// --- VRAM tie-break & cap ---------------------------------------------------

TEST(GpuCapabilityProbe, VramBreaksTiesAmongEqualDevices) {
    // Same class, same hw decode/encode, both under the 512 MiB VRAM cap:
    // the device with more VRAM must win.
    auto small = makeDevice(0, "Small", GpuVendor::AMD, GpuDeviceType::IntegratedGpu,
                            true, true, false, 256 * kMiB);
    auto large = makeDevice(1, "Large", GpuVendor::AMD, GpuDeviceType::IntegratedGpu,
                            true, true, false, 480 * kMiB);
    const auto chosen = selectDevice({small, large}, GpuSelectionPolicy::automatic());
    ASSERT_TRUE(chosen.has_value());
    EXPECT_EQ(*chosen, 1);
    EXPECT_GT(scoreDevice(large, GpuSelectionPolicy::automatic()),
              scoreDevice(small, GpuSelectionPolicy::automatic()));
}

TEST(GpuCapabilityProbe, VramCapCannotLeapfrogDeviceClass) {
    // A memory-heavy integrated GPU must not beat a discrete GPU on VRAM alone;
    // the device-class weight dominates and the VRAM contribution is capped.
    auto hugeIntegrated = makeDevice(0, "iGPU 32G", GpuVendor::Intel,
                                     GpuDeviceType::IntegratedGpu, true, true, false,
                                     32 * kGiB);
    auto smallDiscrete = makeDevice(1, "dGPU 1G", GpuVendor::NVIDIA,
                                    GpuDeviceType::DiscreteGpu, true, false, false,
                                    1 * kGiB);
    const auto chosen = selectDevice({hugeIntegrated, smallDiscrete},
                                     GpuSelectionPolicy::automatic());
    ASSERT_TRUE(chosen.has_value());
    EXPECT_EQ(*chosen, 1); // the discrete device
}

// --- Deterministic tie-break ------------------------------------------------

TEST(GpuCapabilityProbe, EqualScoreBreaksTieByLowerIndex) {
    // Two identical discrete devices: selection must be deterministic and pick
    // the lower enumeration index.
    auto a = makeDevice(2, "A", GpuVendor::AMD, GpuDeviceType::DiscreteGpu,
                        true, true, true, 4 * kGiB);
    auto b = makeDevice(5, "B", GpuVendor::AMD, GpuDeviceType::DiscreteGpu,
                        true, true, true, 4 * kGiB);
    // Provide them out of index order to ensure ordering isn't positional.
    const auto chosen = selectDevice({b, a}, GpuSelectionPolicy::automatic());
    ASSERT_TRUE(chosen.has_value());
    EXPECT_EQ(*chosen, 2);
    EXPECT_EQ(scoreDevice(a, GpuSelectionPolicy::automatic()),
              scoreDevice(b, GpuSelectionPolicy::automatic()));
}

// --- Decode vs compute-only weighting --------------------------------------

TEST(GpuCapabilityProbe, HardwareDecodeBeatsComputeOnlySameClass) {
    auto computeOnly = makeDevice(0, "ComputeOnly", GpuVendor::AMD,
                                  GpuDeviceType::DiscreteGpu, true, false, false,
                                  4 * kGiB);
    auto decoder = makeDevice(1, "Decoder", GpuVendor::AMD,
                              GpuDeviceType::DiscreteGpu, true, true, false,
                              4 * kGiB);
    const auto chosen = selectDevice({computeOnly, decoder},
                                     GpuSelectionPolicy::automatic());
    ASSERT_TRUE(chosen.has_value());
    EXPECT_EQ(*chosen, 1);
}

// --- Mixed multi-GPU scoring ------------------------------------------------

TEST(GpuCapabilityProbe, MixedThreeDeviceScoringPicksEncoderDiscrete) {
    auto integrated = makeDevice(0, "iGPU", GpuVendor::Intel,
                                 GpuDeviceType::IntegratedGpu, true, true, true,
                                 512 * kMiB);
    auto discreteDecode = makeDevice(1, "dGPU decode", GpuVendor::AMD,
                                     GpuDeviceType::DiscreteGpu, true, true, false,
                                     6 * kGiB);
    auto discreteEncode = makeDevice(2, "dGPU encode", GpuVendor::NVIDIA,
                                     GpuDeviceType::DiscreteGpu, true, true, true,
                                     8 * kGiB);
    const auto chosen = selectDevice({integrated, discreteDecode, discreteEncode},
                                     GpuSelectionPolicy::automatic());
    ASSERT_TRUE(chosen.has_value());
    EXPECT_EQ(*chosen, 2); // discrete + hardware encode is the strongest.
}

// --- PreferVendor tie-breaking ----------------------------------------------

TEST(GpuCapabilityProbe, PreferVendorPicksBestDeviceOfThatVendor) {
    // Two devices of the preferred vendor plus a stronger other-vendor device:
    // the bias applies to both preferred devices, so the better preferred one
    // (discrete + encode) must be chosen over the weaker preferred iGPU.
    auto amdIntegrated = makeDevice(0, "AMD iGPU", GpuVendor::AMD,
                                    GpuDeviceType::IntegratedGpu, true, true, false,
                                    512 * kMiB);
    auto amdDiscrete = makeDevice(1, "AMD dGPU", GpuVendor::AMD,
                                  GpuDeviceType::DiscreteGpu, true, true, true,
                                  8 * kGiB);
    auto nvidiaDiscrete = makeDevice(2, "NVIDIA dGPU", GpuVendor::NVIDIA,
                                     GpuDeviceType::DiscreteGpu, true, true, true,
                                     12 * kGiB);
    const auto chosen = selectDevice({amdIntegrated, amdDiscrete, nvidiaDiscrete},
                                     GpuSelectionPolicy::preferVendor(GpuVendor::AMD));
    ASSERT_TRUE(chosen.has_value());
    EXPECT_EQ(*chosen, 1); // best AMD device wins over the stronger NVIDIA one.
}

TEST(GpuCapabilityProbe, PreferVendorBiasBeatsStrongerOtherVendor) {
    // A weak preferred iGPU should still be chosen over a strong non-preferred
    // discrete device because the vendor bias dominates the score.
    auto intelIntegrated = makeDevice(0, "Intel iGPU", GpuVendor::Intel,
                                      GpuDeviceType::IntegratedGpu, true, false, false,
                                      256 * kMiB);
    auto nvidiaDiscrete = makeDevice(1, "NVIDIA dGPU", GpuVendor::NVIDIA,
                                     GpuDeviceType::DiscreteGpu, true, true, true,
                                     16 * kGiB);
    const auto chosen = selectDevice({intelIntegrated, nvidiaDiscrete},
                                     GpuSelectionPolicy::preferVendor(GpuVendor::Intel));
    ASSERT_TRUE(chosen.has_value());
    EXPECT_EQ(*chosen, 0); // the preferred (Intel) device despite lower merit.
}

// --- Direct scoreDevice ordering --------------------------------------------

TEST(GpuCapabilityProbe, ScoreDeviceOrdersByCapabilityAndClass) {
    const auto policy = GpuSelectionPolicy::automatic();

    auto incapable = makeDevice(0, "NoCompute", GpuVendor::NVIDIA,
                                GpuDeviceType::DiscreteGpu, false, true, true, 8 * kGiB);
    auto integrated = makeDevice(1, "iGPU", GpuVendor::Intel,
                                 GpuDeviceType::IntegratedGpu, true, true, false, 512 * kMiB);
    auto discreteDecode = makeDevice(2, "dGPU decode", GpuVendor::AMD,
                                     GpuDeviceType::DiscreteGpu, true, true, false, 4 * kGiB);
    auto discreteEncode = makeDevice(3, "dGPU encode", GpuVendor::AMD,
                                     GpuDeviceType::DiscreteGpu, true, true, true, 4 * kGiB);

    // Compute-incapable devices are unusable and score exactly 0.
    EXPECT_EQ(scoreDevice(incapable, policy), 0);

    // discrete+encode > discrete+decode > integrated > unusable.
    EXPECT_GT(scoreDevice(discreteEncode, policy), scoreDevice(discreteDecode, policy));
    EXPECT_GT(scoreDevice(discreteDecode, policy), scoreDevice(integrated, policy));
    EXPECT_GT(scoreDevice(integrated, policy), scoreDevice(incapable, policy));
}

// --- GpuCaps field population through the context ---------------------------

TEST(GpuCapabilityProbe, ContextPopulatesAllCapabilityFields) {
    auto discrete = makeDevice(0, "NVIDIA RTX 4090", GpuVendor::NVIDIA,
                               GpuDeviceType::DiscreteGpu, true, true, true, 24 * kGiB);
    auto ctx = GpuContext::createWith(GpuSelectionPolicy::automatic(),
                                      fixedList({discrete}), nullptr);
    ASSERT_TRUE(ctx.isOk());
    const auto& c = ctx.value();
    ASSERT_FALSE(c.isSoftwareFallback());
    EXPECT_EQ(c.selectedDeviceIndex(), 0);
    EXPECT_EQ(c.deviceName(), "NVIDIA RTX 4090");

    const auto& caps = c.capabilities();
    EXPECT_EQ(caps.vendorId, GpuVendor::NVIDIA);
    EXPECT_EQ(caps.vendor, "NVIDIA");
    EXPECT_TRUE(caps.supportsCompute);
    EXPECT_TRUE(caps.hwDecode);
    EXPECT_TRUE(caps.hwEncode);
    EXPECT_EQ(caps.vramBytes, 24 * kGiB);

    ASSERT_EQ(c.availableDevices().size(), 1u);
    EXPECT_EQ(c.availableDevices().front().name, "NVIDIA RTX 4090");
    EXPECT_FALSE(c.unavailableNotice().has_value());
}

TEST(GpuCapabilityProbe, SoftwareFallbackExposesSoftwareCaps) {
    auto ctx = GpuContext::createWith(GpuSelectionPolicy::automatic(),
                                      fixedList({}), nullptr);
    ASSERT_TRUE(ctx.isOk());
    const auto& caps = ctx.value().capabilities();
    EXPECT_EQ(caps.vendorId, GpuVendor::Software);
    EXPECT_EQ(caps.vendor, "software");
    EXPECT_FALSE(caps.supportsCompute);
    EXPECT_FALSE(caps.hwDecode);
    EXPECT_FALSE(caps.hwEncode);
    EXPECT_EQ(caps.vramBytes, 0u);
}

} // namespace
} // namespace palmier::gpu
