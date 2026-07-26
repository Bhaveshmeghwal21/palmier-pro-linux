// SPDX-License-Identifier: GPL-3.0-or-later
//
// media/EncoderSelector.hpp — decide, exactly once per export, which encoder
// runs: a vendor hardware encoder or the FFmpeg software encoder.
//
// This is design.md D8 (task 9.1 of the end-to-end-editor-integration spec). It
// sits above gpu::CodecBridge and below MediaEncoder: the bridge answers "which
// backend *could* run this codec", MediaEncoder does the actual encoding, and the
// selector owns the *policy* — the request/compiled-in/capability gate, the
// bounded capability probe, and the retry-once-then-fall-back rule — plus the
// single value that reports the outcome.
//
// The single-selection invariant (Requirement 8.8) is STRUCTURAL, not checked.
// EncoderSelection has no public constructor, no setters and no mutable state:
// the only ways to obtain one are
//
//     EncoderSelection::hardware(codec, encoderName, backend, parameters)
//     EncoderSelection::software(codec, reason, parameters)
//
// `hardware` always sets hardware = true and softwareFallback = false; `software`
// always sets hardware = false. There is therefore no expressible program — not
// merely no program we happen to have written — in which one selection reports
// both hardware use and a software fallback, and no selection carries two
// encoder names. A contradictory value is unconstructible rather than rejected at
// run time.
//
// Selection order (Requirements 8.2, 8.3, 8.4):
//
//   1. Software IMMEDIATELY when hardware was not requested, when no hardware
//      encode path is compiled in for the device's vendor, or when the codec is
//      absent from GpuCaps::encodeCodecs (equally: the device reports no
//      hardware encode at all, or it is the software/unknown vendor). No probe
//      is run in this case, so a host with no GPU never pays the probe cost.
//   2. Otherwise the capability probe, run on a DETACHED thread and awaited with
//      a 3000 ms deadline (kProbeDeadline). A wedged vendor driver must not be
//      able to block an export, which is why the thread is detached rather than
//      joined: the deadline expires, the selection completes, and the orphaned
//      probe finishes into a shared promise nobody reads. A timeout is treated
//      as "no compatible device" and selects software (Requirement 8.4).
//   3. On a positive probe, the vendor encoder — and if hardware initialization
//      then fails before the first frame, initialization is retried EXACTLY once
//      before selecting the software encoder for the same codec with the
//      requested resolution, frame rate and bit rate unchanged and a reason
//      naming hardware encoder initialization failure (Requirement 8.3).
//
// Testability without a GPU and without sleeping. Three seams, all injectable
// through Options:
//
//   * `probe`       — the capability probe itself (default: read GpuCaps).
//   * `awaiter`     — how the probe is awaited. The default is the real detached
//                     thread + std::future::wait_for(deadline). A test can
//                     inject a VIRTUAL-TIME awaiter that compares a generated
//                     probe delay against the deadline arithmetically, so the
//                     3000 ms boundary is exercised on both sides without any
//                     test waiting 3000 ms (or waiting at all).
//   * `hardwareInit`— hardware encoder initialization (default: succeed, since
//                     the real initialization is MediaEncoder::create upstream).
//                     Injecting a first-attempt failure is what makes the
//                     retry-exactly-once rule observable.
//
// SelectionOutcome reports how many hardware initialization attempts were made,
// so "retries exactly once" is asserted as an equality rather than inferred.

#ifndef PALMIER_MEDIA_ENCODERSELECTOR_HPP
#define PALMIER_MEDIA_ENCODERSELECTOR_HPP

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

#include "core/FrameRate.hpp"
#include "core/Resolution.hpp"
#include "core/Result.hpp"
#include "gpu/CodecBridge.hpp"
#include "gpu/GpuTypes.hpp"

namespace palmier::media {

// ---------------------------------------------------------------------------
// EncodeParameters — the output parameters a fallback must preserve
// ---------------------------------------------------------------------------

/// Resolution, frame rate and bit rate as requested. Requirement 8.3 requires a
/// software fallback to keep all three unchanged, so the selection carries them
/// and the property test compares them field-for-field against the request.
struct EncodeParameters {
    Resolution   resolution{Resolution::hd1080()};
    FrameRate    frameRate{FrameRate::fps30()};
    std::int64_t bitrateBitsPerSecond{0};

    [[nodiscard]] friend bool operator==(const EncodeParameters& lhs,
                                         const EncodeParameters& rhs) noexcept {
        return lhs.resolution.width == rhs.resolution.width &&
               lhs.resolution.height == rhs.resolution.height &&
               lhs.frameRate.numerator() == rhs.frameRate.numerator() &&
               lhs.frameRate.denominator() == rhs.frameRate.denominator() &&
               lhs.bitrateBitsPerSecond == rhs.bitrateBitsPerSecond;
    }
};

// ---------------------------------------------------------------------------
// EncoderSelection — exactly one encoder, structurally consistent
// ---------------------------------------------------------------------------

/// The result of encoder selection: ONE encoder name, and flags that cannot
/// contradict each other because the only two ways to build the value each fix
/// them (see the file comment and Requirement 8.8).
class EncoderSelection {
public:
    /// The vendor hardware encoder for `codec` (e.g. "h264_nvenc").
    /// hardware = true, softwareFallback = false, reason empty — always.
    [[nodiscard]] static EncoderSelection hardware(gpu::CodecId codec,
                                                   std::string encoderName,
                                                   gpu::CodecBackend backend,
                                                   EncodeParameters parameters) {
        EncoderSelection s;
        s.codec_ = codec;
        s.encoderName_ = std::move(encoderName);
        s.backend_ = backend;
        s.hardware_ = true;
        s.parameters_ = parameters;
        return s;
    }

    /// The FFmpeg software encoder for `codec` ("libx264" | "libx265" |
    /// "libvpx-vp9" | …). hardware = false — always. `reason` explains why
    /// hardware was not used; an EMPTY reason means software was what the caller
    /// asked for, which is not a fallback, so softwareFallback = !reason.empty()
    /// exactly as design.md D8 specifies.
    [[nodiscard]] static EncoderSelection software(gpu::CodecId codec,
                                                   std::string reason,
                                                   EncodeParameters parameters) {
        EncoderSelection s;
        s.codec_ = codec;
        s.encoderName_ = std::string(gpu::softwareEncoderName(codec));
        s.backend_ = gpu::CodecBackend::FFmpegSoftware;
        s.hardware_ = false;
        s.fallbackReason_ = std::move(reason);
        s.parameters_ = parameters;
        return s;
    }

    [[nodiscard]] gpu::CodecId codec() const noexcept { return codec_; }
    /// The one and only selected encoder name.
    [[nodiscard]] const std::string& encoderName() const noexcept { return encoderName_; }
    [[nodiscard]] gpu::CodecBackend backend() const noexcept { return backend_; }
    [[nodiscard]] bool isHardware() const noexcept { return hardware_; }
    /// True only for a software selection that was NOT requested as such.
    [[nodiscard]] bool isSoftwareFallback() const noexcept {
        return !hardware_ && !fallbackReason_.empty();
    }
    [[nodiscard]] const std::string& fallbackReason() const noexcept { return fallbackReason_; }
    [[nodiscard]] const EncodeParameters& parameters() const noexcept { return parameters_; }

private:
    EncoderSelection() = default;

    gpu::CodecId      codec_{gpu::CodecId::Unknown};
    std::string       encoderName_{};
    gpu::CodecBackend backend_{gpu::CodecBackend::FFmpegSoftware};
    bool              hardware_{false};
    std::string       fallbackReason_{};
    EncodeParameters  parameters_{};
};

// ---------------------------------------------------------------------------
// Request / outcome
// ---------------------------------------------------------------------------

/// What to select an encoder for. `caps` and `availability` are values, so the
/// whole policy is exercisable with synthetic device capabilities and synthetic
/// compiled-in states on a host with neither a GPU nor a vendor SDK.
struct EncoderSelectionRequest {
    gpu::CodecId            codec{gpu::CodecId::H264};
    bool                    preferHardware{true};
    EncodeParameters        parameters{};
    gpu::GpuCaps            caps{gpu::GpuCaps::software()};
    gpu::BridgeAvailability availability{gpu::BridgeAvailability::fromBuildConfig()};
};

/// How the capability probe resolved.
enum class ProbeOutcome {
    NotRun,       ///< Software was selected before any probe was needed.
    Supported,    ///< A compatible device was reported.
    Unsupported,  ///< The probe returned "no compatible device".
    TimedOut,     ///< The probe did not answer within the deadline (Requirement 8.4).
};

/// The selection plus the observable facts about how it was reached.
struct SelectionOutcome {
    EncoderSelection selection;
    ProbeOutcome     probe{ProbeOutcome::NotRun};
    /// Hardware initialization attempts made: 0 when no hardware was tried,
    /// 1 when the first attempt succeeded, 2 when it failed and the single
    /// permitted retry ran. Never more than 2 (Requirement 8.3).
    int              hardwareInitAttempts{0};
    /// hardwareInitAttempts - 1 when at least one attempt was made, else 0.
    [[nodiscard]] int hardwareInitRetries() const noexcept {
        return hardwareInitAttempts > 0 ? hardwareInitAttempts - 1 : 0;
    }
};

// ---------------------------------------------------------------------------
// Seams
// ---------------------------------------------------------------------------

/// The device-capability probe: "does this device/driver really support encoding
/// `codec`?". Runs on a detached thread under a deadline.
using CapabilityProbe = std::function<bool(gpu::CodecId, const gpu::GpuCaps&)>;

/// How a probe is awaited. The production awaiter detaches a thread and waits on
/// a future for `deadline`; a test may inject a virtual-time awaiter so the
/// deadline boundary is exercised without real waiting.
using ProbeAwaiter = std::function<ProbeOutcome(const CapabilityProbe&,
                                                gpu::CodecId,
                                                const gpu::GpuCaps&,
                                                std::chrono::milliseconds /*deadline*/)>;

/// Hardware encoder initialization for a candidate selection. In production this
/// is MediaEncoder::create's hardware build; injecting a failing implementation
/// is how the retry-exactly-once rule is made observable.
using HardwareInitFn = std::function<Result<void>(const EncoderSelection&)>;

// ---------------------------------------------------------------------------
// EncoderSelector
// ---------------------------------------------------------------------------

/// The three injectable seams plus the probe deadline.
///
/// Declared at namespace scope (rather than nested in EncoderSelector) because a
/// nested class's default member initializers may not be used in a default
/// argument of the still-incomplete enclosing class; EncoderSelector::Options is
/// an alias for it, so callers may spell it either way.
struct EncoderSelectorOptions {
    /// Defaults to EncoderSelector::kProbeDeadline (3000 ms). Spelled
    /// numerically because EncoderSelector is not yet complete here; a unit test
    /// asserts the two agree, so they cannot drift.
    std::chrono::milliseconds probeDeadline{3000};
    CapabilityProbe           probe{};        ///< empty → capsCapabilityProbe()
    ProbeAwaiter              awaiter{};      ///< empty → detachedThreadAwaiter()
    HardwareInitFn            hardwareInit{}; ///< empty → always succeeds
};

class EncoderSelector {
public:
    using Options = EncoderSelectorOptions;

    /// The deadline Requirement 8.4 fixes for the capability probe.
    static constexpr std::chrono::milliseconds kProbeDeadline{3000};

    /// Hardware initialization is attempted at most twice: the first attempt
    /// plus exactly one retry (Requirement 8.3).
    static constexpr int kMaxHardwareInitAttempts = 2;

    explicit EncoderSelector(Options options = {});

    /// Select exactly one encoder for `request`. Fails only when the codec has
    /// no encoder at all (gpu::CodecId::Unknown) — every other input yields a
    /// selection, because software encoding is always available.
    [[nodiscard]] Result<SelectionOutcome> select(const EncoderSelectionRequest& request) const;

    /// The FFmpeg encoder name for a vendor hardware backend and codec
    /// ("h264_nvenc", "hevc_vaapi", "vp9_qsv", …); empty when the pair has no
    /// hardware encoder.
    [[nodiscard]] static std::string vendorEncoderName(gpu::CodecId codec,
                                                       gpu::CodecBackend backend);

    /// The production awaiter: run `probe` on a detached thread and wait for the
    /// deadline. Exposed so tests can drive the real thing with a short injected
    /// deadline and a probe that never answers.
    [[nodiscard]] static ProbeAwaiter detachedThreadAwaiter();

    /// The default probe: the device reports hardware encode and lists the codec.
    [[nodiscard]] static CapabilityProbe capsCapabilityProbe();

    [[nodiscard]] std::chrono::milliseconds probeDeadline() const noexcept {
        return options_.probeDeadline;
    }

private:
    /// The compiled-in hardware encode backend for this device's vendor, or
    /// CodecBackend::None when none applies.
    [[nodiscard]] static gpu::CodecBackend hardwareEncodeBackend(
        const gpu::GpuCaps& caps, const gpu::BridgeAvailability& availability);

    Options options_;
};

/// Human-readable codec name for reasons and log lines.
[[nodiscard]] std::string_view encoderSelectorCodecName(gpu::CodecId codec) noexcept;

} // namespace palmier::media

#endif // PALMIER_MEDIA_ENCODERSELECTOR_HPP
