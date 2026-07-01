// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/gpu_codec_bridge_test.cpp — unit tests for the HW decode/encode bridge:
// backend routing per detected capability and the retry-once-on-CPU-then-log
// software fallback (task 7.5; Requirements 10.2, 10.5).
//
// The bridge holds no media state and takes the actual decode/encode work as a
// caller-supplied callable, so these tests drive it with synthetic GpuCaps,
// synthetic BridgeAvailability, and mock operations — no GPU, no FFmpeg, and no
// vendor SDK required.

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include "core/Error.hpp"
#include "core/Result.hpp"
#include "gpu/CodecBridge.hpp"
#include "gpu/GpuTypes.hpp"

namespace palmier::gpu {
namespace {

// --- Synthetic capability builders -----------------------------------------

GpuCaps vendorCaps(GpuVendor vendor, bool hwDecode, bool hwEncode,
                   std::set<CodecId> decodeCodecs, std::set<CodecId> encodeCodecs) {
    GpuCaps c;
    c.vendorId = vendor;
    c.vendor = std::string{vendorName(vendor)};
    c.supportsCompute = true;
    c.hwDecode = hwDecode;
    c.hwEncode = hwEncode;
    c.decodeCodecs = std::move(decodeCodecs);
    c.encodeCodecs = std::move(encodeCodecs);
    c.vramBytes = 4ull * 1024 * 1024 * 1024;
    return c;
}

GpuCaps nvidiaAllCodecs() {
    return vendorCaps(GpuVendor::NVIDIA, true, true,
                      {CodecId::H264, CodecId::HEVC, CodecId::AV1},
                      {CodecId::H264, CodecId::HEVC});
}

// A CodecOpFn that always succeeds and records every route it was invoked with.
struct RecordingOp {
    std::vector<CodecRoute>* calls;
    Result<void> operator()(const CodecRoute& r) const {
        calls->push_back(r);
        return ok();
    }
};

// --- Routing: hardware selection per vendor --------------------------------

TEST(CodecBridgeRoute, NvidiaRoutesDecodeToNvdecEncodeToNvenc) {
    CodecBridge bridge(nvidiaAllCodecs(), BridgeAvailability::all());

    const auto dec = bridge.route(CodecId::H264, CodecOperation::Decode);
    EXPECT_EQ(dec.backend, CodecBackend::Nvdec);
    EXPECT_TRUE(dec.hardware);

    const auto enc = bridge.route(CodecId::H264, CodecOperation::Encode);
    EXPECT_EQ(enc.backend, CodecBackend::Nvenc);
    EXPECT_TRUE(enc.hardware);
}

TEST(CodecBridgeRoute, AmdRoutesToVaapiForBothOperations) {
    CodecBridge bridge(vendorCaps(GpuVendor::AMD, true, true,
                                  {CodecId::H264}, {CodecId::H264}),
                       BridgeAvailability::all());

    EXPECT_EQ(bridge.route(CodecId::H264, CodecOperation::Decode).backend,
              CodecBackend::Vaapi);
    EXPECT_EQ(bridge.route(CodecId::H264, CodecOperation::Encode).backend,
              CodecBackend::Vaapi);
}

TEST(CodecBridgeRoute, IntelPrefersQuickSyncThenVaapi) {
    const auto caps = vendorCaps(GpuVendor::Intel, true, true,
                                 {CodecId::H264}, {CodecId::H264});

    // Both available -> Quick Sync preferred.
    CodecBridge qsv(caps, BridgeAvailability::all());
    EXPECT_EQ(qsv.route(CodecId::H264, CodecOperation::Decode).backend,
              CodecBackend::QuickSync);

    // Only VAAPI compiled in -> VAAPI.
    BridgeAvailability vaapiOnly;
    vaapiOnly.vaapi = true;
    CodecBridge va(caps, vaapiOnly);
    EXPECT_EQ(va.route(CodecId::H264, CodecOperation::Decode).backend,
              CodecBackend::Vaapi);
}

// --- Routing: software fallback conditions ---------------------------------

TEST(CodecBridgeRoute, SoftwareContextAlwaysRoutesToSoftware) {
    CodecBridge bridge(GpuCaps::software(), BridgeAvailability::all());
    const auto r = bridge.route(CodecId::H264, CodecOperation::Decode);
    EXPECT_EQ(r.backend, CodecBackend::FFmpegSoftware);
    EXPECT_FALSE(r.hardware);
}

TEST(CodecBridgeRoute, UnsupportedCodecFallsBackToSoftware) {
    // NVIDIA device that reports HW decode but does not list AV1 among its
    // decode codecs (design: "HW decode unsupported for codec -> SW decode").
    CodecBridge bridge(nvidiaAllCodecs(), BridgeAvailability::all());
    const auto r = bridge.route(CodecId::VP9, CodecOperation::Decode);
    EXPECT_EQ(r.backend, CodecBackend::FFmpegSoftware);
    EXPECT_FALSE(r.hardware);
}

TEST(CodecBridgeRoute, NoHardwareCapabilityFallsBackToSoftware) {
    // hwEncode == false -> encode must go to software even though the codec is
    // listed and the vendor backend is compiled in.
    auto caps = vendorCaps(GpuVendor::NVIDIA, /*hwDecode=*/true, /*hwEncode=*/false,
                           {CodecId::H264}, {CodecId::H264});
    CodecBridge bridge(caps, BridgeAvailability::all());
    EXPECT_EQ(bridge.route(CodecId::H264, CodecOperation::Encode).backend,
              CodecBackend::FFmpegSoftware);
    // Decode still uses hardware.
    EXPECT_EQ(bridge.route(CodecId::H264, CodecOperation::Decode).backend,
              CodecBackend::Nvdec);
}

TEST(CodecBridgeRoute, BackendNotCompiledInFallsBackToSoftware) {
    // Capable NVIDIA device, but this build has no NVIDIA backend compiled in.
    CodecBridge bridge(nvidiaAllCodecs(), BridgeAvailability::softwareOnly());
    EXPECT_EQ(bridge.route(CodecId::H264, CodecOperation::Decode).backend,
              CodecBackend::FFmpegSoftware);
}

TEST(CodecBridgeRoute, SoftwareEncodeCarriesFfmpegEncoderName) {
    CodecBridge bridge(GpuCaps::software(), BridgeAvailability::all());
    EXPECT_EQ(bridge.route(CodecId::H264, CodecOperation::Encode).softwareEncoder,
              "libx264");
    EXPECT_EQ(bridge.route(CodecId::HEVC, CodecOperation::Encode).softwareEncoder,
              "libx265");
    EXPECT_EQ(bridge.route(CodecId::AV1, CodecOperation::Encode).softwareEncoder,
              "libsvtav1");
    // Decode routes carry no encoder name.
    EXPECT_TRUE(bridge.route(CodecId::H264, CodecOperation::Decode).softwareEncoder.empty());
}

TEST(SoftwareEncoderName, MapsCodecsToLibraries) {
    EXPECT_EQ(softwareEncoderName(CodecId::H264), "libx264");
    EXPECT_EQ(softwareEncoderName(CodecId::HEVC), "libx265");
    EXPECT_EQ(softwareEncoderName(CodecId::AV1), "libsvtav1");
    EXPECT_EQ(softwareEncoderName(CodecId::VP9), "libvpx-vp9");
    EXPECT_TRUE(softwareEncoderName(CodecId::Unknown).empty());
}

// --- Fallback execution: retry-once-on-CPU-then-log (Requirement 10.5) ------

TEST(CodecBridgeExecute, HardwareSuccessRunsOnceNoRetry) {
    CodecBridge bridge(nvidiaAllCodecs(), BridgeAvailability::all());
    std::vector<CodecRoute> calls;
    auto exec = bridge.execute(CodecId::H264, CodecOperation::Decode, RecordingOp{&calls});

    EXPECT_TRUE(exec.result.isOk());
    EXPECT_FALSE(exec.retriedOnCpu);
    EXPECT_FALSE(exec.hardwareError.has_value());
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].backend, CodecBackend::Nvdec);
    EXPECT_TRUE(bridge.log().empty());
}

TEST(CodecBridgeExecute, HardwareFailureRetriesOnceOnCpuAndLogs) {
    CodecBridge bridge(nvidiaAllCodecs(), BridgeAvailability::all());
    std::vector<CodecRoute> calls;
    // Fail on the hardware attempt, succeed on the software attempt.
    auto op = [&calls](const CodecRoute& r) -> Result<void> {
        calls.push_back(r);
        if (r.hardware) {
            return Error(ErrorCode::Internal, "VK_ERROR_DEVICE_LOST");
        }
        return ok();
    };

    auto exec = bridge.execute(CodecId::HEVC, CodecOperation::Encode, op);

    EXPECT_TRUE(exec.result.isOk());
    EXPECT_TRUE(exec.retriedOnCpu);
    ASSERT_TRUE(exec.hardwareError.has_value());
    EXPECT_EQ(exec.hardwareError->code(), ErrorCode::Internal);

    // Exactly two attempts: hardware first, then the CPU fallback.
    ASSERT_EQ(calls.size(), 2u);
    EXPECT_EQ(calls[0].backend, CodecBackend::Nvenc);
    EXPECT_EQ(calls[1].backend, CodecBackend::FFmpegSoftware);
    EXPECT_EQ(calls[1].softwareEncoder, "libx265"); // HEVC software encoder.

    // The failure was recorded in the log (Requirement 10.5).
    ASSERT_FALSE(bridge.log().empty());
    EXPECT_NE(bridge.log().front().find("retrying once on the CPU"), std::string::npos);

    // Final route reflects the backend that produced the result.
    EXPECT_EQ(exec.route.backend, CodecBackend::FFmpegSoftware);
}

TEST(CodecBridgeExecute, InputPreservedAcrossRetry) {
    // The bridge must not alter inputs between attempts: the codec/operation seen
    // by the CPU retry match the hardware attempt (Requirement 10.5 — no data lost).
    CodecBridge bridge(nvidiaAllCodecs(), BridgeAvailability::all());
    std::vector<CodecRoute> calls;
    auto op = [&calls](const CodecRoute& r) -> Result<void> {
        calls.push_back(r);
        return r.hardware ? Result<void>(Error(ErrorCode::Internal, "gpu lost")) : ok();
    };

    bridge.execute(CodecId::H264, CodecOperation::Decode, op);

    ASSERT_EQ(calls.size(), 2u);
    EXPECT_EQ(calls[0].codec, CodecId::H264);
    EXPECT_EQ(calls[0].operation, CodecOperation::Decode);
    EXPECT_EQ(calls[1].codec, CodecId::H264);
    EXPECT_EQ(calls[1].operation, CodecOperation::Decode);
}

TEST(CodecBridgeExecute, CpuRetryFailureIsReportedAndLoggedTwice) {
    CodecBridge bridge(nvidiaAllCodecs(), BridgeAvailability::all());
    std::vector<CodecRoute> calls;
    // Both attempts fail.
    auto op = [&calls](const CodecRoute& r) -> Result<void> {
        calls.push_back(r);
        return Error(ErrorCode::Io, "boom");
    };

    auto exec = bridge.execute(CodecId::H264, CodecOperation::Encode, op);

    EXPECT_TRUE(exec.result.isError());
    EXPECT_TRUE(exec.retriedOnCpu);
    EXPECT_EQ(calls.size(), 2u); // at most one CPU retry — never more.
    EXPECT_EQ(bridge.log().size(), 2u); // hardware failure + CPU failure.
}

TEST(CodecBridgeExecute, SoftwarePrimaryFailureIsNotRetried) {
    // On a software context the primary route is already the CPU path, so a
    // failure must be returned without any (pointless) retry.
    CodecBridge bridge(GpuCaps::software(), BridgeAvailability::all());
    int attempts = 0;
    auto op = [&attempts](const CodecRoute&) -> Result<void> {
        ++attempts;
        return Error(ErrorCode::Io, "disk full");
    };

    auto exec = bridge.execute(CodecId::H264, CodecOperation::Encode, op);

    EXPECT_TRUE(exec.result.isError());
    EXPECT_FALSE(exec.retriedOnCpu);
    EXPECT_EQ(attempts, 1);
    EXPECT_FALSE(exec.hardwareError.has_value());
}

TEST(CodecBridgeExecute, CustomLogSinkReceivesFailureLine) {
    CodecBridge bridge(nvidiaAllCodecs(), BridgeAvailability::all());
    std::vector<std::string> sink;
    bridge.setLogSink([&sink](std::string_view line) { sink.emplace_back(line); });

    auto op = [](const CodecRoute& r) -> Result<void> {
        return r.hardware ? Result<void>(Error(ErrorCode::Internal, "lost")) : ok();
    };
    bridge.execute(CodecId::H264, CodecOperation::Decode, op);

    ASSERT_FALSE(sink.empty());
    EXPECT_NE(sink.front().find("NVDEC"), std::string::npos);
}

} // namespace
} // namespace palmier::gpu
