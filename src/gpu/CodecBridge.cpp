// SPDX-License-Identifier: GPL-3.0-or-later
//
// gpu/CodecBridge.cpp — implementation of the HW codec routing + SW fallback.
//
// See CodecBridge.hpp for the design rationale. The routing decision is a pure
// function of the device's probed GpuCaps and the compiled-in backends
// (BridgeAvailability); the fallback executor applies the retry-once-on-CPU
// policy required by Requirement 10.5 and design's Error Handling table
// ("export encoder failure -> retry with software encoder"; "GPU device lost ->
// drop to software; preserve project state").

#include "gpu/CodecBridge.hpp"

#include <string>
#include <utility>

namespace palmier::gpu {

namespace {

[[nodiscard]] std::string_view operationName(CodecOperation op) noexcept {
    return op == CodecOperation::Encode ? "encode" : "decode";
}

[[nodiscard]] std::string_view codecName(CodecId codec) noexcept {
    switch (codec) {
        case CodecId::H264:    return "H.264";
        case CodecId::HEVC:    return "HEVC";
        case CodecId::AV1:     return "AV1";
        case CodecId::VP9:     return "VP9";
        case CodecId::MPEG2:   return "MPEG-2";
        case CodecId::Unknown: return "unknown";
    }
    return "unknown";
}

} // namespace

std::string_view softwareEncoderName(CodecId codec) noexcept {
    switch (codec) {
        case CodecId::H264:  return "libx264";
        case CodecId::HEVC:  return "libx265";
        case CodecId::AV1:   return "libsvtav1";
        case CodecId::VP9:   return "libvpx-vp9";
        case CodecId::MPEG2: return "mpeg2video";
        case CodecId::Unknown:
            return "";
    }
    return "";
}

BridgeAvailability BridgeAvailability::fromBuildConfig() noexcept {
    BridgeAvailability a;
    a.ffmpegSoftware = true; // Always compiled in — the graceful-degradation path.
#ifdef PALMIER_HAVE_NVENC
    a.nvdec = true;
    a.nvenc = true;
#endif
#ifdef PALMIER_HAVE_VAAPI
    a.vaapi = true;
#endif
#ifdef PALMIER_HAVE_QSV
    a.quickSync = true;
#endif
    return a;
}

CodecBridge::CodecBridge(GpuCaps caps, BridgeAvailability availability)
    : caps_(std::move(caps)), avail_(availability) {}

CodecBackend CodecBridge::hardwareBackendFor(CodecOperation operation) const {
    switch (caps_.vendorId) {
        case GpuVendor::NVIDIA:
            // NVIDIA: NVDEC for decode, NVENC for encode (design vendor table).
            if (operation == CodecOperation::Decode) {
                return avail_.nvdec ? CodecBackend::Nvdec : CodecBackend::None;
            }
            return avail_.nvenc ? CodecBackend::Nvenc : CodecBackend::None;

        case GpuVendor::AMD:
            // AMD: VAAPI for both decode and encode (VCN).
            return avail_.vaapi ? CodecBackend::Vaapi : CodecBackend::None;

        case GpuVendor::Intel:
            // Intel: prefer Quick Sync when compiled in, else VAAPI
            // (design table: Intel = "VAAPI / Quick Sync").
            if (avail_.quickSync) return CodecBackend::QuickSync;
            if (avail_.vaapi)     return CodecBackend::Vaapi;
            return CodecBackend::None;

        case GpuVendor::Software:
        case GpuVendor::Unknown:
            return CodecBackend::None;
    }
    return CodecBackend::None;
}

CodecRoute CodecBridge::softwareRoute(CodecId codec, CodecOperation operation) const {
    CodecRoute r;
    r.codec = codec;
    r.operation = operation;
    r.backend = CodecBackend::FFmpegSoftware;
    r.hardware = false;
    if (operation == CodecOperation::Encode) {
        r.softwareEncoder = std::string(softwareEncoderName(codec));
    }
    r.detail = "FFmpeg software";
    return r;
}

CodecRoute CodecBridge::route(CodecId codec, CodecOperation operation) const {
    const bool wantEncode = (operation == CodecOperation::Encode);
    const bool hwCapable = wantEncode ? caps_.hwEncode : caps_.hwDecode;
    const std::set<CodecId>& codecSet =
        wantEncode ? caps_.encodeCodecs : caps_.decodeCodecs;
    const bool codecSupported = codecSet.find(codec) != codecSet.end();
    const CodecBackend hwBackend = hardwareBackendFor(operation);

    // Route to hardware only when: the device is a real vendor GPU, the device
    // reports hardware (de/en)code, the specific codec is hardware-supported,
    // and a matching backend is compiled in. Otherwise fall back to software.
    if (caps_.vendorId != GpuVendor::Software &&
        caps_.vendorId != GpuVendor::Unknown &&
        hwCapable && codecSupported && hwBackend != CodecBackend::None) {
        CodecRoute r;
        r.codec = codec;
        r.operation = operation;
        r.backend = hwBackend;
        r.hardware = true;
        r.detail = std::string(vendorName(caps_.vendorId)) + " " +
                   std::string(backendName(hwBackend));
        return r;
    }

    // Explain, for logs/UI, why we fell back to software.
    CodecRoute sw = softwareRoute(codec, operation);
    std::string reason;
    if (caps_.vendorId == GpuVendor::Software || caps_.vendorId == GpuVendor::Unknown) {
        reason = "no hardware backend (software context)";
    } else if (!hwCapable) {
        reason = "device lacks hardware " + std::string(operationName(operation));
    } else if (!codecSupported) {
        reason = std::string(codecName(codec)) + " not hardware-supported";
    } else {
        reason = "hardware backend not built for this vendor";
    }
    sw.detail = "FFmpeg software (" + reason + ")";
    return sw;
}

CodecExecution CodecBridge::execute(CodecId codec, CodecOperation operation,
                                    const CodecOpFn& op) {
    CodecExecution e;
    e.primaryRoute = route(codec, operation);
    e.route = e.primaryRoute;

    // First attempt on the routed backend.
    e.result = op(e.primaryRoute);
    if (e.result.isOk()) {
        return e;
    }

    // A hardware failure triggers the retry-once-on-CPU-then-log policy
    // (Requirement 10.5). A software primary failure is returned as-is — we are
    // already on the CPU path, so there is nothing to fall back to.
    if (isHardwareBackend(e.primaryRoute.backend)) {
        e.hardwareError = e.result.error();
        logLine("GPU " + std::string(operationName(operation)) + " failed on " +
                std::string(backendName(e.primaryRoute.backend)) + " for codec " +
                std::string(codecName(codec)) + ": " + e.result.error().toString() +
                "; retrying once on the CPU (FFmpeg software) path");

        const CodecRoute sw = softwareRoute(codec, operation);
        e.retriedOnCpu = true;
        e.route = sw;
        // The bridge holds no input state; `op` re-runs against the same inputs,
        // so no edit data is lost on the retry (Requirement 10.5).
        e.result = op(sw);
        if (e.result.isError()) {
            logLine("CPU fallback " + std::string(operationName(operation)) +
                    " also failed for codec " + std::string(codecName(codec)) +
                    ": " + e.result.error().toString());
        }
    }

    return e;
}

void CodecBridge::logLine(std::string line) {
    if (sink_) {
        sink_(line);
    }
    log_.push_back(std::move(line));
}

} // namespace palmier::gpu
