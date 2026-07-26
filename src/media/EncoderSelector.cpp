// SPDX-License-Identifier: GPL-3.0-or-later
//
// media/EncoderSelector.cpp — implementation of design.md D8 (task 9.1).
//
// See EncoderSelector.hpp for the policy and the reasoning behind the detached
// probe thread and the three injectable seams.

#include "media/EncoderSelector.hpp"

#include <future>
#include <memory>
#include <thread>
#include <utility>

#include "core/Error.hpp"

namespace palmier::media {

std::string_view encoderSelectorCodecName(gpu::CodecId codec) noexcept {
    switch (codec) {
        case gpu::CodecId::H264:  return "H.264";
        case gpu::CodecId::HEVC:  return "HEVC";
        case gpu::CodecId::AV1:   return "AV1";
        case gpu::CodecId::VP9:   return "VP9";
        case gpu::CodecId::MPEG2: return "MPEG-2";
        case gpu::CodecId::Unknown: return "unknown";
    }
    return "unknown";
}

namespace {

/// Vendor suffix for the FFmpeg encoder name of a hardware backend.
[[nodiscard]] std::string_view backendSuffix(gpu::CodecBackend backend) noexcept {
    switch (backend) {
        case gpu::CodecBackend::Nvenc:     return "_nvenc";
        case gpu::CodecBackend::Vaapi:     return "_vaapi";
        case gpu::CodecBackend::QuickSync: return "_qsv";
        default:                           return "";
    }
}

/// FFmpeg codec prefix for a hardware encoder name ("h264", "hevc", "vp9").
[[nodiscard]] std::string_view codecPrefix(gpu::CodecId codec) noexcept {
    switch (codec) {
        case gpu::CodecId::H264: return "h264";
        case gpu::CodecId::HEVC: return "hevc";
        case gpu::CodecId::VP9:  return "vp9";
        case gpu::CodecId::AV1:  return "av1";
        case gpu::CodecId::MPEG2:
        case gpu::CodecId::Unknown:
            return "";
    }
    return "";
}

} // namespace

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

std::string EncoderSelector::vendorEncoderName(gpu::CodecId codec,
                                               gpu::CodecBackend backend) {
    const std::string_view prefix = codecPrefix(codec);
    const std::string_view suffix = backendSuffix(backend);
    if (prefix.empty() || suffix.empty()) {
        return {};
    }
    return std::string(prefix) + std::string(suffix);
}

CapabilityProbe EncoderSelector::capsCapabilityProbe() {
    return [](gpu::CodecId codec, const gpu::GpuCaps& caps) {
        return caps.hwEncode && caps.encodeCodecs.find(codec) != caps.encodeCodecs.end();
    };
}

ProbeAwaiter EncoderSelector::detachedThreadAwaiter() {
    return [](const CapabilityProbe& probe,
              gpu::CodecId codec,
              const gpu::GpuCaps& caps,
              std::chrono::milliseconds deadline) {
        // The promise is shared so the orphaned probe thread keeps it alive after
        // this function returns on a timeout: the thread is DETACHED precisely so
        // a wedged vendor driver cannot block the caller (design.md D8).
        auto promise = std::make_shared<std::promise<bool>>();
        std::future<bool> future = promise->get_future();
        // `caps` is copied into the thread: the caller's value may be gone by the
        // time a late probe finally runs.
        std::thread worker([promise, probe, codec, capsCopy = caps]() mutable {
            bool supported = false;
            try {
                supported = probe ? probe(codec, capsCopy) : false;
            } catch (...) {
                supported = false;
            }
            try {
                promise->set_value(supported);
            } catch (...) {
                // Nobody is listening any more (deadline expired); nothing to do.
            }
        });
        worker.detach();

        if (future.wait_for(deadline) != std::future_status::ready) {
            return ProbeOutcome::TimedOut;
        }
        return future.get() ? ProbeOutcome::Supported : ProbeOutcome::Unsupported;
    };
}

gpu::CodecBackend EncoderSelector::hardwareEncodeBackend(
    const gpu::GpuCaps& caps, const gpu::BridgeAvailability& availability) {
    switch (caps.vendorId) {
        case gpu::GpuVendor::NVIDIA:
            return availability.nvenc ? gpu::CodecBackend::Nvenc : gpu::CodecBackend::None;
        case gpu::GpuVendor::Intel:
            if (availability.quickSync) return gpu::CodecBackend::QuickSync;
            if (availability.vaapi)     return gpu::CodecBackend::Vaapi;
            return gpu::CodecBackend::None;
        case gpu::GpuVendor::AMD:
            return availability.vaapi ? gpu::CodecBackend::Vaapi : gpu::CodecBackend::None;
        case gpu::GpuVendor::Software:
        case gpu::GpuVendor::Unknown:
            return gpu::CodecBackend::None;
    }
    return gpu::CodecBackend::None;
}

// ---------------------------------------------------------------------------
// EncoderSelector
// ---------------------------------------------------------------------------

EncoderSelector::EncoderSelector(Options options) : options_(std::move(options)) {
    if (!options_.probe)   options_.probe = capsCapabilityProbe();
    if (!options_.awaiter) options_.awaiter = detachedThreadAwaiter();
    if (!options_.hardwareInit) {
        options_.hardwareInit = [](const EncoderSelection&) { return ok(); };
    }
    if (options_.probeDeadline <= std::chrono::milliseconds::zero()) {
        options_.probeDeadline = kProbeDeadline;
    }
}

Result<SelectionOutcome> EncoderSelector::select(
    const EncoderSelectionRequest& request) const {
    const std::string codecLabel{encoderSelectorCodecName(request.codec)};

    // A codec with no encoder at all cannot yield a selection of any kind.
    if (request.codec == gpu::CodecId::Unknown ||
        gpu::softwareEncoderName(request.codec).empty()) {
        return invalidArgument("no encoder exists for codec " + codecLabel);
    }

    const auto softwareOutcome = [&](std::string reason, ProbeOutcome probe) {
        SelectionOutcome outcome{
            EncoderSelection::software(request.codec, std::move(reason), request.parameters),
            probe,
            0};
        return Result<SelectionOutcome>(std::move(outcome));
    };

    // --- Step 1: the reasons software is selected immediately, with no probe --

    if (!request.preferHardware) {
        // The caller asked for software; nothing "fell back", so the reason —
        // and therefore the software-fallback flag — stays empty/false.
        return softwareOutcome({}, ProbeOutcome::NotRun);
    }

    const gpu::CodecBackend backend =
        hardwareEncodeBackend(request.caps, request.availability);
    if (backend == gpu::CodecBackend::None) {
        return softwareOutcome("no hardware encode path is compiled in for vendor " +
                                   std::string(gpu::vendorName(request.caps.vendorId)),
                               ProbeOutcome::NotRun);
    }

    const std::string encoderName = vendorEncoderName(request.codec, backend);
    if (encoderName.empty()) {
        return softwareOutcome(codecLabel + " has no " +
                                   std::string(gpu::backendName(backend)) +
                                   " hardware encoder",
                               ProbeOutcome::NotRun);
    }

    if (!request.caps.hwEncode ||
        request.caps.encodeCodecs.find(request.codec) == request.caps.encodeCodecs.end()) {
        return softwareOutcome("the selected device reports no hardware encode support for " +
                                   codecLabel,
                               ProbeOutcome::NotRun);
    }

    // --- Step 2: the bounded capability probe --------------------------------

    const ProbeOutcome probe =
        options_.awaiter(options_.probe, request.codec, request.caps, options_.probeDeadline);

    if (probe == ProbeOutcome::TimedOut) {
        // A timeout is treated as "no compatible device" (Requirement 8.4).
        return softwareOutcome("the hardware capability probe for " + codecLabel +
                                   " did not return within " +
                                   std::to_string(options_.probeDeadline.count()) +
                                   " ms and is treated as no compatible device",
                               probe);
    }
    if (probe != ProbeOutcome::Supported) {
        return softwareOutcome("the hardware capability probe reported no " + codecLabel +
                                   "-compatible device",
                               probe);
    }

    // --- Step 3: the vendor encoder, retrying initialization exactly once ----

    const EncoderSelection candidate = EncoderSelection::hardware(
        request.codec, encoderName, backend, request.parameters);

    std::string lastError;
    for (int attempt = 1; attempt <= kMaxHardwareInitAttempts; ++attempt) {
        Result<void> init = options_.hardwareInit(candidate);
        if (init.isOk()) {
            return SelectionOutcome{candidate, probe, attempt};
        }
        lastError = init.error().toString();
    }

    // Both attempts failed: the software encoder for the SAME codec, with
    // resolution, frame rate and bit rate carried over untouched, and a reason
    // naming hardware encoder initialization failure (Requirement 8.3).
    SelectionOutcome outcome{
        EncoderSelection::software(request.codec,
                                   "hardware encoder initialization failed for " +
                                       encoderName + " after one retry: " + lastError,
                                   request.parameters),
        probe,
        kMaxHardwareInitAttempts};
    return outcome;
}

} // namespace palmier::media
