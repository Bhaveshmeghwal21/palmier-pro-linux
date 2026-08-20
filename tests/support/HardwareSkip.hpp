// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/support/HardwareSkip.hpp — PALMIER_SKIP_WITHOUT_HW(codec, operation)
// (task 9.1 of the end-to-end-editor-integration spec; Requirement 15.5).
//
// A test that requires a REAL hardware decode or encode cannot pass on a host
// with no vendor SDK compiled in and no compatible device. Requirement 15.5 says
// such a test must be reported as SKIPPED with a recorded reason naming the
// absent SDK or the absent device — never as a failure, and never silently.
//
// This helper answers exactly that question, in two stages, mirroring the two
// ways hardware can be missing:
//
//   1. Compiled-in state — gpu::BridgeAvailability::fromBuildConfig(), i.e. the
//      PALMIER_HAVE_VAAPI / PALMIER_HAVE_NVENC / PALMIER_HAVE_QSV defines. When
//      no vendor path is compiled in for the operation, the reason names the
//      missing defines ("no vendor hardware encode path is compiled in
//      (PALMIER_HAVE_NVENC, PALMIER_HAVE_VAAPI, PALMIER_HAVE_QSV all undefined)").
//   2. Live device — the capabilities of the GPU context this host actually
//      selects (gpu::GpuContext::create(automatic()), which degrades to the
//      software fallback rather than failing when there is no GPU). When the
//      device does not report the codec for that operation, the reason names the
//      device and the codec ("no VAAPI-capable device reported for H.264 encode
//      on \"Software (CPU)\"").
//
// The live capabilities are queried ONCE per test binary and cached, so using
// the macro in many tests costs one context creation, not one per test.
//
// Note what this header is NOT for: the property tests that *reason about*
// hardware selection (Properties 40-42) drive synthetic gpu::GpuCaps and
// gpu::BridgeAvailability values through media::EncoderSelector and therefore
// always run, on every host, GPU or not. Only tests that need a real vendor
// encoder to actually produce bytes (task 9.8, the L4 validation lane) guard
// themselves with this macro.

#ifndef PALMIER_TESTS_SUPPORT_HARDWARESKIP_HPP
#define PALMIER_TESTS_SUPPORT_HARDWARESKIP_HPP

#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "gpu/CodecBridge.hpp"
#include "gpu/GpuContext.hpp"
#include "gpu/GpuTypes.hpp"

namespace palmier::test_support {

/// Human-readable codec name for skip reasons.
[[nodiscard]] inline std::string hardwareCodecName(gpu::CodecId codec) {
    switch (codec) {
        case gpu::CodecId::H264:  return "H.264";
        case gpu::CodecId::HEVC:  return "HEVC";
        case gpu::CodecId::AV1:   return "AV1";
        case gpu::CodecId::VP9:   return "VP9";
        case gpu::CodecId::MPEG2: return "MPEG-2";
        case gpu::CodecId::Unknown: break;
    }
    return "unknown codec";
}

[[nodiscard]] inline std::string hardwareOperationName(gpu::CodecOperation operation) {
    return operation == gpu::CodecOperation::Encode ? "encode" : "decode";
}

/// The capabilities of the device this host selects, queried once per binary.
/// GpuContext::create never fails for "no GPU" — it degrades to the software
/// fallback — so this is safe on a machine with no GPU and no Vulkan loader.
[[nodiscard]] inline const gpu::GpuCaps& liveGpuCaps() {
    static const gpu::GpuCaps caps = [] {
        Result<gpu::GpuContext> context =
            gpu::GpuContext::create(gpu::GpuSelectionPolicy::automatic());
        if (context.isOk()) {
            return context.value().capabilities();
        }
        return gpu::GpuCaps::software();
    }();
    return caps;
}

/// Name of the device this host selects, queried once per binary.
[[nodiscard]] inline const std::string& liveGpuDeviceName() {
    static const std::string name = [] {
        Result<gpu::GpuContext> context =
            gpu::GpuContext::create(gpu::GpuSelectionPolicy::automatic());
        if (context.isOk()) {
            return context.value().deviceName();
        }
        return std::string{"Software (CPU)"};
    }();
    return name;
}

/// Why a (codec, operation) cannot run on real hardware here, or std::nullopt
/// when it can. The reason always names either the absent SDK defines or the
/// absent device (Requirement 15.5).
[[nodiscard]] inline std::optional<std::string> hardwareSkipReason(
    gpu::CodecId codec,
    gpu::CodecOperation operation,
    const gpu::BridgeAvailability& availability = gpu::BridgeAvailability::fromBuildConfig(),
    const gpu::GpuCaps& caps = liveGpuCaps()) {
    const std::string codecLabel = hardwareCodecName(codec);
    const std::string opLabel = hardwareOperationName(operation);
    const bool encode = operation == gpu::CodecOperation::Encode;

    // Stage 1 — is any vendor path compiled in for this operation?
    const bool anyVendorPath = encode ? (availability.nvenc || availability.vaapi ||
                                         availability.quickSync)
                                      : (availability.nvdec || availability.vaapi ||
                                         availability.quickSync);
    if (!anyVendorPath) {
        return "no vendor hardware " + opLabel +
               " path is compiled in (PALMIER_HAVE_NVENC, PALMIER_HAVE_VAAPI and "
               "PALMIER_HAVE_QSV are all undefined), so " +
               codecLabel + " hardware " + opLabel + " cannot be exercised on this build";
    }

    // Stage 2 — does the selected device report this codec for this operation?
    const bool deviceHwCapable = encode ? caps.hwEncode : caps.hwDecode;
    if (!deviceHwCapable) {
        return "no hardware-" + opLabel + "-capable device reported for " + codecLabel +
               " on \"" + liveGpuDeviceName() + "\" (vendor " + caps.vendor + ")";
    }

    const auto& codecSet = encode ? caps.encodeCodecs : caps.decodeCodecs;
    if (codecSet.find(codec) == codecSet.end()) {
        return "the selected device \"" + liveGpuDeviceName() + "\" (vendor " + caps.vendor +
               ") reports no " + codecLabel + " hardware " + opLabel + " support";
    }

    return std::nullopt;
}

} // namespace palmier::test_support

/// Skip the current test, with a reason naming the missing SDK or device, unless
/// this host can really run `codec` through hardware `operation`
/// (gpu::CodecOperation::Encode / ::Decode). Requirement 15.5.
#define PALMIER_SKIP_WITHOUT_HW(codec, operation)                                    \
    do {                                                                             \
        const std::optional<std::string> palmierHwSkipReason =                        \
            ::palmier::test_support::hardwareSkipReason((codec), (operation));        \
        if (palmierHwSkipReason.has_value()) {                                        \
            GTEST_SKIP() << *palmierHwSkipReason;                                     \
        }                                                                             \
    } while (false)

#endif // PALMIER_TESTS_SUPPORT_HARDWARESKIP_HPP
