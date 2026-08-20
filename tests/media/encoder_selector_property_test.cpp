// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/media/encoder_selector_property_test.cpp — Properties 40, 41 and 42 for
// media::EncoderSelector (task 9.2 of the end-to-end-editor-integration spec;
// Requirements 8.2, 8.3, 8.4, 8.8).
//
//   * Property 40 — exactly one encoder selection with consistent flags.
//   * Property 41 — hardware init failure retries once then falls back with
//                   parameters intact.
//   * Property 42 — software selection uses the documented encoder for each codec.
//
// These properties ALWAYS RUN. They never skip, on any host, with or without a
// GPU or a vendor SDK. That is possible because the selector takes the device
// capabilities and the compiled-in vendor paths as VALUES — gpu::GpuCaps and
// gpu::BridgeAvailability — so the generator synthesizes all 3 codecs x 8
// compiled-in states x arbitrary capability sets directly. Nothing here consults
// gpu::BridgeAvailability::fromBuildConfig() or a live device, so a host with
// "disabled (SDK not found)" for VAAPI, NVENC and QSV — this sandbox, and the
// SDK-free CI configure job — exercises the full policy including every hardware
// branch. tests/support/HardwareSkip.hpp exists for the *other* kind of test:
// one that needs a real vendor encoder to emit real bytes (task 9.8).
//
// Nothing here sleeps, and no test waits out the 3000 ms probe deadline:
//
//   * the properties inject a VIRTUAL-TIME awaiter which compares a generated
//     probe delay against the deadline arithmetically, so delays straddling
//     3000 ms (Property 42's generator) cost zero wall-clock time and the
//     boundary is exact rather than raced;
//   * the REAL detached-thread awaiter is covered separately by two unit tests
//     that inject a SHORT deadline (50 ms) — one with a probe blocked on a gate
//     the test owns, proving a wedged driver yields TimedOut promptly, and one
//     with an immediate probe under the full 3000 ms deadline, proving a fast
//     probe is not made to wait for it. Both are bounded, so a regression fails
//     rather than hangs.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include "core/FrameRate.hpp"
#include "core/Resolution.hpp"
#include "core/Result.hpp"
#include "gpu/CodecBridge.hpp"
#include "gpu/GpuTypes.hpp"
#include "media/EncoderSelector.hpp"
#include "support/HardwareSkip.hpp"

namespace palmier::media {
namespace {

using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// The type-level single-selection invariant (Requirement 8.8)
// ---------------------------------------------------------------------------
//
// EncoderSelection cannot be built except through its two named factories, so a
// value reporting BOTH hardware use and a software fallback is not merely absent
// from the implementation — it is inexpressible. These static assertions are the
// compile-time half of Property 40.

static_assert(!std::is_default_constructible_v<EncoderSelection>,
              "EncoderSelection must not be default-constructible: every value has to come "
              "from hardware() or software()");
static_assert(!std::is_aggregate_v<EncoderSelection>,
              "EncoderSelection must not be an aggregate: brace-initialization would let a "
              "caller set both flags");
static_assert(!std::is_constructible_v<EncoderSelection, gpu::CodecId, std::string, bool, bool,
                                       std::string>,
              "EncoderSelection must expose no field-wise constructor");

// ---------------------------------------------------------------------------
// The three supported codecs and the eight compiled-in states
// ---------------------------------------------------------------------------

constexpr gpu::CodecId kSupportedCodecs[] = {
    gpu::CodecId::H264,
    gpu::CodecId::HEVC,
    gpu::CodecId::VP9,
};

/// The documented software encoder for each supported codec (Requirement 8.4).
[[nodiscard]] std::string documentedSoftwareEncoder(gpu::CodecId codec) {
    switch (codec) {
        case gpu::CodecId::H264: return "libx264";
        case gpu::CodecId::HEVC: return "libx265";
        case gpu::CodecId::VP9:  return "libvpx-vp9";
        default:                 return {};
    }
}

/// The eight compiled-in states of the VAAPI, NVENC and QSV paths, as a bitmask
/// 0..7. The software path is always compiled in.
[[nodiscard]] gpu::BridgeAvailability availabilityFromMask(int mask) {
    gpu::BridgeAvailability a;
    a.vaapi = (mask & 0b001) != 0;
    a.nvenc = (mask & 0b010) != 0;
    a.nvdec = a.nvenc;
    a.quickSync = (mask & 0b100) != 0;
    a.ffmpegSoftware = true;
    return a;
}

/// The backend the selector must pick for a vendor, given the compiled-in state;
/// CodecBackend::None when no vendor path applies. Mirrors the vendor mapping
/// table in design.md rather than reimplementing selector logic: NVIDIA needs
/// NVENC, AMD needs VAAPI, Intel prefers Quick Sync and accepts VAAPI.
[[nodiscard]] gpu::CodecBackend expectedBackend(gpu::GpuVendor vendor,
                                                const gpu::BridgeAvailability& a) {
    switch (vendor) {
        case gpu::GpuVendor::NVIDIA:
            return a.nvenc ? gpu::CodecBackend::Nvenc : gpu::CodecBackend::None;
        case gpu::GpuVendor::AMD:
            return a.vaapi ? gpu::CodecBackend::Vaapi : gpu::CodecBackend::None;
        case gpu::GpuVendor::Intel:
            if (a.quickSync) return gpu::CodecBackend::QuickSync;
            if (a.vaapi)     return gpu::CodecBackend::Vaapi;
            return gpu::CodecBackend::None;
        case gpu::GpuVendor::Software:
        case gpu::GpuVendor::Unknown:
            return gpu::CodecBackend::None;
    }
    return gpu::CodecBackend::None;
}

// ---------------------------------------------------------------------------
// Awaiters and probes used by the properties (no sleeping, no threads)
// ---------------------------------------------------------------------------

/// A VIRTUAL-TIME awaiter: the probe is modelled as taking `delay`, and the
/// deadline decides the outcome arithmetically. This is how the 3000 ms boundary
/// of Requirement 8.4 is exercised from both sides at zero wall-clock cost.
[[nodiscard]] ProbeAwaiter virtualTimeAwaiter(std::chrono::milliseconds delay,
                                              std::shared_ptr<std::atomic<int>> probeCalls = {}) {
    return [delay, probeCalls](const CapabilityProbe& probe,
                               gpu::CodecId codec,
                               const gpu::GpuCaps& caps,
                               std::chrono::milliseconds deadline) {
        if (delay > deadline) {
            // The probe never answers in time; the selector must treat this as
            // "no compatible device" without waiting for the orphaned probe.
            return ProbeOutcome::TimedOut;
        }
        if (probeCalls) probeCalls->fetch_add(1);
        return probe(codec, caps) ? ProbeOutcome::Supported : ProbeOutcome::Unsupported;
    };
}

/// Records how many times hardware initialization was attempted and fails the
/// first `failuresToInject` attempts.
struct InitLedger {
    std::atomic<int> attempts{0};
    int              failuresToInject{0};

    [[nodiscard]] HardwareInitFn fn() {
        return [this](const EncoderSelection& candidate) -> Result<void> {
            const int attempt = attempts.fetch_add(1) + 1;
            if (attempt <= failuresToInject) {
                return makeError(ErrorCode::Internal,
                                 "synthetic init failure #" + std::to_string(attempt) +
                                     " for " + candidate.encoderName());
            }
            return ok();
        };
    }
};

// ---------------------------------------------------------------------------
// Generators
// ---------------------------------------------------------------------------

[[nodiscard]] rc::Gen<gpu::CodecId> genSupportedCodec() {
    return rc::gen::element(kSupportedCodecs[0], kSupportedCodecs[1], kSupportedCodecs[2]);
}

[[nodiscard]] rc::Gen<gpu::GpuVendor> genVendor() {
    return rc::gen::element(gpu::GpuVendor::NVIDIA, gpu::GpuVendor::AMD, gpu::GpuVendor::Intel,
                            gpu::GpuVendor::Software, gpu::GpuVendor::Unknown);
}

/// Synthetic device capabilities: a vendor, an hwEncode flag and an encode-codec
/// set drawn from the three supported codecs. No live device is consulted.
[[nodiscard]] rc::Gen<gpu::GpuCaps> genCaps() {
    return rc::gen::map(
        rc::gen::tuple(genVendor(),
                       rc::gen::arbitrary<bool>(),
                       rc::gen::arbitrary<bool>(),
                       rc::gen::arbitrary<bool>(),
                       rc::gen::arbitrary<bool>()),
        [](const std::tuple<gpu::GpuVendor, bool, bool, bool, bool>& t) {
            gpu::GpuCaps caps;
            caps.vendorId = std::get<0>(t);
            caps.vendor = std::string(gpu::vendorName(caps.vendorId));
            caps.hwEncode = std::get<1>(t);
            caps.hwDecode = caps.hwEncode;
            if (std::get<2>(t)) caps.encodeCodecs.insert(gpu::CodecId::H264);
            if (std::get<3>(t)) caps.encodeCodecs.insert(gpu::CodecId::HEVC);
            if (std::get<4>(t)) caps.encodeCodecs.insert(gpu::CodecId::VP9);
            caps.decodeCodecs = caps.encodeCodecs;
            return caps;
        });
}

/// Requested output parameters: valid resolutions, frame rates and bit rates.
[[nodiscard]] rc::Gen<EncodeParameters> genParameters() {
    return rc::gen::map(
        rc::gen::tuple(rc::gen::inRange(2, 3841),      // width  (even-ised below)
                       rc::gen::inRange(2, 2161),      // height
                       rc::gen::element<std::int64_t>(24, 25, 30, 48, 50, 60, 120),
                       rc::gen::element<std::int64_t>(1, 1001),
                       rc::gen::inRange<std::int64_t>(0, 200'000'001)),
        [](const std::tuple<int, int, std::int64_t, std::int64_t, std::int64_t>& t) {
            EncodeParameters p;
            p.resolution = Resolution{static_cast<std::uint32_t>(std::get<0>(t)),
                                      static_cast<std::uint32_t>(std::get<1>(t))};
            const std::int64_t den = std::get<3>(t);
            const std::int64_t num = den == 1001 ? std::get<2>(t) * 1000 : std::get<2>(t);
            p.frameRate = FrameRate{num, den};
            p.bitrateBitsPerSecond = std::get<4>(t);
            return p;
        });
}

/// A device that genuinely supports `codec` for hardware encode, on `vendor`.
[[nodiscard]] gpu::GpuCaps capableCaps(gpu::GpuVendor vendor, gpu::CodecId codec) {
    gpu::GpuCaps caps;
    caps.vendorId = vendor;
    caps.vendor = std::string(gpu::vendorName(vendor));
    caps.hwEncode = true;
    caps.hwDecode = true;
    caps.encodeCodecs.insert(codec);
    caps.decodeCodecs.insert(codec);
    return caps;
}

/// Every vendor path compiled in.
[[nodiscard]] gpu::BridgeAvailability allVendorPaths() { return availabilityFromMask(0b111); }

} // namespace

// ===========================================================================
// Property 40
// ===========================================================================

// Feature: end-to-end-editor-integration, Property 40: Exactly one encoder selection with
// consistent flags — for all combinations of the three supported codecs (H.264, HEVC, VP9) and the
// eight compiled-in states of the VAAPI, NVENC and QSV paths, and any device capability set, the
// Encoder_Selector returns exactly one encoder selection; the software-fallback flag is true only
// when the returned selection is a software encoder; hardware use and software fallback are never
// both reported; and when a supported hardware path and a capable device are present the selection
// is the vendor encoder with the fallback flag false.
//
// **Validates: Requirements 8.2, 8.8**
RC_GTEST_PROP(EncoderSelectorProperties,
              ExactlyOneEncoderSelectionWithConsistentFlags,
              ()) {
    const auto codec = *genSupportedCodec();
    const int mask = *rc::gen::inRange(0, 8);
    const auto caps = *genCaps();
    const auto parameters = *genParameters();
    const bool preferHardware = *rc::gen::arbitrary<bool>();

    const gpu::BridgeAvailability availability = availabilityFromMask(mask);

    // A truthful probe: it answers exactly what the synthetic device reports.
    // The awaiter is virtual-time with a zero delay, so the probe always answers
    // in time and no wall-clock time passes.
    EncoderSelector::Options options;
    options.probe = EncoderSelector::capsCapabilityProbe();
    options.awaiter = virtualTimeAwaiter(0ms);
    const EncoderSelector selector{options};

    EncoderSelectionRequest request;
    request.codec = codec;
    request.preferHardware = preferHardware;
    request.parameters = parameters;
    request.caps = caps;
    request.availability = availability;

    const Result<SelectionOutcome> result = selector.select(request);
    RC_ASSERT(result.isOk());
    const EncoderSelection& selection = result.value().selection;

    // --- Exactly one selection, and the flags cannot contradict -------------
    RC_ASSERT(!selection.encoderName().empty());
    RC_ASSERT(selection.codec() == codec);
    RC_ASSERT(!(selection.isHardware() && selection.isSoftwareFallback()));
    // The fallback flag is true only for a software selection.
    if (selection.isSoftwareFallback()) {
        RC_ASSERT(!selection.isHardware());
        RC_ASSERT(!selection.fallbackReason().empty());
    }
    // A hardware selection never carries a fallback reason.
    if (selection.isHardware()) {
        RC_ASSERT(selection.fallbackReason().empty());
        RC_ASSERT(!selection.isSoftwareFallback());
        RC_ASSERT(gpu::isHardwareBackend(selection.backend()));
        RC_ASSERT(selection.encoderName() ==
                  EncoderSelector::vendorEncoderName(codec, selection.backend()));
    } else {
        RC_ASSERT(selection.backend() == gpu::CodecBackend::FFmpegSoftware);
        RC_ASSERT(selection.encoderName() == documentedSoftwareEncoder(codec));
    }

    // Requested parameters always survive selection untouched.
    RC_ASSERT(selection.parameters() == parameters);

    // --- The hardware branch is forced when everything is in place ----------
    const gpu::CodecBackend backend = expectedBackend(caps.vendorId, availability);
    const bool deviceCapable =
        caps.hwEncode && caps.encodeCodecs.find(codec) != caps.encodeCodecs.end();
    const bool hardwareEncoderExists =
        !EncoderSelector::vendorEncoderName(codec, backend).empty();

    if (preferHardware && backend != gpu::CodecBackend::None && deviceCapable &&
        hardwareEncoderExists) {
        RC_ASSERT(selection.isHardware());
        RC_ASSERT(!selection.isSoftwareFallback());
        RC_ASSERT(selection.backend() == backend);
        RC_ASSERT(result.value().probe == ProbeOutcome::Supported);
    } else {
        // Otherwise software, and a reason unless software was what was asked for.
        RC_ASSERT(!selection.isHardware());
        RC_ASSERT(selection.isSoftwareFallback() == preferHardware);
    }
}

// ===========================================================================
// Property 41
// ===========================================================================

// Feature: end-to-end-editor-integration, Property 41: Hardware init failure retries once then
// falls back with parameters intact — for any codec and any hardware initialization failure
// pattern occurring before the first encoded frame, the selector attempts hardware initialization
// at most twice, then selects the software encoder for the same codec with the requested
// resolution, frame rate and bit rate unchanged, completes with a frame count equal to the
// requested range, and reports the software encoder name with the fallback flag true and a
// non-empty reason indicating hardware encoder initialization failure.
//
// **Validates: Requirements 8.3**
RC_GTEST_PROP(EncoderSelectorProperties,
              HardwareInitFailureRetriesOnceThenFallsBackWithParametersIntact,
              ()) {
    const auto codec = *genSupportedCodec();
    const auto vendor = *rc::gen::element(gpu::GpuVendor::NVIDIA, gpu::GpuVendor::AMD,
                                          gpu::GpuVendor::Intel);
    const auto parameters = *genParameters();
    // The two failure patterns of the design's generator: fail once then succeed,
    // or fail twice.
    const int failuresToInject = *rc::gen::element(1, 2);
    // The requested range, in whole frames, used for the frame-count check.
    const auto requestedFrames = *rc::gen::inRange<std::int64_t>(1, 1801);

    InitLedger ledger;
    ledger.failuresToInject = failuresToInject;

    EncoderSelector::Options options;
    options.probe = EncoderSelector::capsCapabilityProbe();
    options.awaiter = virtualTimeAwaiter(0ms);
    options.hardwareInit = ledger.fn();
    const EncoderSelector selector{options};

    EncoderSelectionRequest request;
    request.codec = codec;
    request.preferHardware = true;
    request.parameters = parameters;
    request.caps = capableCaps(vendor, codec);
    request.availability = allVendorPaths();

    const Result<SelectionOutcome> result = selector.select(request);
    RC_ASSERT(result.isOk());
    const SelectionOutcome& outcome = result.value();
    const EncoderSelection& selection = outcome.selection;

    // --- Initialization was attempted EXACTLY twice: once plus one retry -----
    // Both generated patterns fail the first attempt, so the retry must have run
    // exactly once — asserted as an equality, not as "at least once", so a second
    // retry (or none) is a counterexample.
    RC_ASSERT(ledger.attempts.load() == 2);
    RC_ASSERT(outcome.hardwareInitAttempts == 2);
    RC_ASSERT(outcome.hardwareInitRetries() == 1);
    RC_ASSERT(outcome.hardwareInitAttempts <= EncoderSelector::kMaxHardwareInitAttempts);

    // The requested resolution, frame rate and bit rate survive either outcome.
    RC_ASSERT(selection.parameters() == parameters);
    RC_ASSERT(selection.codec() == codec);

    // The frame count over the requested range is unchanged, because the frame
    // rate is: counting the range with the SELECTED rate gives exactly what
    // counting it with the REQUESTED rate gives. (Comparing the two counts,
    // rather than comparing against `requestedFrames`, keeps the assertion exact
    // for the fractional NTSC rates 24000/1001 and friends, where whole-frame
    // duration arithmetic truncates.)
    const Duration range = parameters.frameRate.durationForFrames(requestedFrames);
    RC_ASSERT(selection.parameters().frameRate.framesForDuration(range) ==
              parameters.frameRate.framesForDuration(range));

    if (failuresToInject == 1) {
        // Fail-once-then-succeed: the retry succeeded, so hardware is used and
        // nothing fell back.
        RC_ASSERT(selection.isHardware());
        RC_ASSERT(!selection.isSoftwareFallback());
        RC_ASSERT(selection.fallbackReason().empty());
        RC_ASSERT(selection.encoderName() ==
                  EncoderSelector::vendorEncoderName(codec, selection.backend()));
    } else {
        // Fail-twice: the software encoder for the SAME codec, fallback flag true,
        // and a reason naming hardware encoder initialization failure.
        RC_ASSERT(!selection.isHardware());
        RC_ASSERT(selection.isSoftwareFallback());
        RC_ASSERT(selection.encoderName() == documentedSoftwareEncoder(codec));
        const std::string& reason = selection.fallbackReason();
        RC_ASSERT(!reason.empty());
        RC_ASSERT(reason.find("hardware encoder initialization failed") != std::string::npos);
    }
}

// ===========================================================================
// Property 42
// ===========================================================================

// Feature: end-to-end-editor-integration, Property 42: Software selection uses the documented
// encoder for each codec — for any condition that forces software encoding (no hardware path
// compiled in, no codec-compatible device reported, or a capability probe that does not return
// within 3000 ms), the selection is `libx264` for H.264, `libx265` for HEVC and `libvpx-vp9` for
// VP9, and the export completes.
//
// **Validates: Requirements 8.4**
RC_GTEST_PROP(EncoderSelectorProperties,
              SoftwareSelectionUsesTheDocumentedEncoderForEachCodec,
              ()) {
    const auto codec = *genSupportedCodec();
    const auto parameters = *genParameters();
    // The three forcing conditions of the design's generator.
    const int condition = *rc::gen::inRange(0, 3);
    // Probe delays straddling the 3000 ms deadline, driven in virtual time.
    const auto delayMs = *rc::gen::inRange<std::int64_t>(0, 6001);
    const auto vendor = *rc::gen::element(gpu::GpuVendor::NVIDIA, gpu::GpuVendor::AMD,
                                          gpu::GpuVendor::Intel);

    EncoderSelectionRequest request;
    request.codec = codec;
    request.preferHardware = true;
    request.parameters = parameters;

    switch (condition) {
        case 0:
            // No hardware path compiled in, but a fully capable device.
            request.caps = capableCaps(vendor, codec);
            request.availability = availabilityFromMask(0);
            break;
        case 1: {
            // Every path compiled in, but the device reports no compatible codec.
            gpu::GpuCaps caps = capableCaps(vendor, codec);
            caps.encodeCodecs.clear();
            caps.hwEncode = false;
            request.caps = caps;
            request.availability = allVendorPaths();
            break;
        }
        default:
            // Every path compiled in and a capable device: only the probe delay
            // decides, so this case straddles the 3000 ms deadline.
            request.caps = capableCaps(vendor, codec);
            request.availability = allVendorPaths();
            break;
    }

    EncoderSelector::Options options;
    options.probe = EncoderSelector::capsCapabilityProbe();
    options.awaiter = virtualTimeAwaiter(std::chrono::milliseconds{delayMs});
    const EncoderSelector selector{options};
    RC_ASSERT(selector.probeDeadline() == EncoderSelector::kProbeDeadline);

    const Result<SelectionOutcome> result = selector.select(request);
    RC_ASSERT(result.isOk());
    const SelectionOutcome& outcome = result.value();
    const EncoderSelection& selection = outcome.selection;

    const bool probeTimedOut =
        std::chrono::milliseconds{delayMs} > EncoderSelector::kProbeDeadline;
    const bool forcedToSoftware = condition != 2 || probeTimedOut;

    if (forcedToSoftware) {
        RC_ASSERT(!selection.isHardware());
        RC_ASSERT(selection.encoderName() == documentedSoftwareEncoder(codec));
        RC_ASSERT(selection.isSoftwareFallback());
        RC_ASSERT(selection.parameters() == parameters);
        // A selection was produced, so the export can proceed: the encoder name
        // is non-empty and the backend is the always-available software one.
        RC_ASSERT(selection.backend() == gpu::CodecBackend::FFmpegSoftware);
        if (condition == 2) {
            // The timeout is treated as "no compatible device" and is named.
            RC_ASSERT(outcome.probe == ProbeOutcome::TimedOut);
            RC_ASSERT(selection.fallbackReason().find("did not return within") !=
                      std::string::npos);
        }
    } else {
        // The other side of the straddle: a probe that answers inside the
        // deadline on a capable device selects the vendor encoder.
        RC_ASSERT(selection.isHardware());
        RC_ASSERT(outcome.probe == ProbeOutcome::Supported);
    }
}

// ===========================================================================
// Unit tests — the deadline constant, the real awaiter, and the exhaustive
// 3 codecs x 8 compiled-in states enumeration design.md D8 calls for.
// ===========================================================================

TEST(EncoderSelector, ProbeDeadlineIsThreeThousandMilliseconds) {
    // Requirement 8.4 fixes the deadline; the default Options must carry it.
    EXPECT_EQ(EncoderSelector::kProbeDeadline, 3000ms);
    EXPECT_EQ(EncoderSelector{}.probeDeadline(), EncoderSelector::kProbeDeadline);
    EXPECT_EQ(EncoderSelector::kMaxHardwareInitAttempts, 2);
}

TEST(EncoderSelector, EnumeratesEveryCodecAndCompiledInStateWithOneConsistentSelection) {
    // The exhaustive companion to Property 40: 3 codecs x 8 compiled-in states x
    // 3 vendors, all on synthetic values, with no probe delay.
    EncoderSelector::Options options;
    options.probe = EncoderSelector::capsCapabilityProbe();
    options.awaiter = virtualTimeAwaiter(0ms);
    const EncoderSelector selector{options};

    for (const gpu::CodecId codec : kSupportedCodecs) {
        for (int mask = 0; mask < 8; ++mask) {
            for (const gpu::GpuVendor vendor :
                 {gpu::GpuVendor::NVIDIA, gpu::GpuVendor::AMD, gpu::GpuVendor::Intel}) {
                EncoderSelectionRequest request;
                request.codec = codec;
                request.caps = capableCaps(vendor, codec);
                request.availability = availabilityFromMask(mask);

                const Result<SelectionOutcome> result = selector.select(request);
                ASSERT_TRUE(result.isOk());
                const EncoderSelection& selection = result.value().selection;

                EXPECT_FALSE(selection.encoderName().empty());
                EXPECT_FALSE(selection.isHardware() && selection.isSoftwareFallback());

                const gpu::CodecBackend backend =
                    expectedBackend(vendor, availabilityFromMask(mask));
                if (backend == gpu::CodecBackend::None) {
                    EXPECT_FALSE(selection.isHardware());
                    EXPECT_EQ(selection.encoderName(), documentedSoftwareEncoder(codec));
                    EXPECT_TRUE(selection.isSoftwareFallback());
                } else {
                    EXPECT_TRUE(selection.isHardware());
                    EXPECT_EQ(selection.backend(), backend);
                    EXPECT_FALSE(selection.isSoftwareFallback());
                    EXPECT_TRUE(selection.fallbackReason().empty());
                }
            }
        }
    }
}

TEST(EncoderSelector, SoftwareIsSelectedWithoutAProbeWhenHardwareIsNotRequested) {
    // Step 1 of the selection order: no probe is run at all, and a caller-asked-for
    // software encode is not reported as a fallback.
    auto probeCalls = std::make_shared<std::atomic<int>>(0);
    EncoderSelector::Options options;
    options.probe = [probeCalls](gpu::CodecId, const gpu::GpuCaps&) {
        probeCalls->fetch_add(1);
        return true;
    };
    options.awaiter = virtualTimeAwaiter(0ms, probeCalls);
    const EncoderSelector selector{options};

    EncoderSelectionRequest request;
    request.codec = gpu::CodecId::H264;
    request.preferHardware = false;
    request.caps = capableCaps(gpu::GpuVendor::NVIDIA, gpu::CodecId::H264);
    request.availability = allVendorPaths();

    const Result<SelectionOutcome> result = selector.select(request);
    ASSERT_TRUE(result.isOk());
    EXPECT_EQ(result.value().probe, ProbeOutcome::NotRun);
    EXPECT_EQ(probeCalls->load(), 0);
    EXPECT_EQ(result.value().selection.encoderName(), "libx264");
    EXPECT_FALSE(result.value().selection.isHardware());
    EXPECT_FALSE(result.value().selection.isSoftwareFallback());
    EXPECT_EQ(result.value().hardwareInitAttempts, 0);
}

TEST(EncoderSelector, UnknownCodecHasNoSelection) {
    EncoderSelectionRequest request;
    request.codec = gpu::CodecId::Unknown;
    const Result<SelectionOutcome> result = EncoderSelector{}.select(request);
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(EncoderSelector, RealAwaiterTimesOutPromptlyWhenTheProbeNeverAnswers) {
    // The PRODUCTION awaiter — detached thread + future::wait_for — driven with a
    // SHORT injected deadline so the test does not wait out 3000 ms. The probe
    // blocks on a gate the test owns rather than on a sleep, so the timeout is
    // certain rather than raced; the gate is opened at the end so the detached
    // thread finishes.
    struct Gate {
        std::mutex m;
        std::condition_variable cv;
        bool open{false};
        std::atomic<bool> entered{false};
    };
    auto gate = std::make_shared<Gate>();

    EncoderSelector::Options options;
    options.probeDeadline = 50ms;
    options.awaiter = EncoderSelector::detachedThreadAwaiter();
    options.probe = [gate](gpu::CodecId, const gpu::GpuCaps&) {
        gate->entered.store(true);
        std::unique_lock<std::mutex> lock(gate->m);
        // Bounded so a leaked thread cannot outlive the binary indefinitely.
        gate->cv.wait_for(lock, 60s, [&] { return gate->open; });
        return true;
    };
    const EncoderSelector selector{options};

    EncoderSelectionRequest request;
    request.codec = gpu::CodecId::H264;
    request.caps = capableCaps(gpu::GpuVendor::NVIDIA, gpu::CodecId::H264);
    request.availability = allVendorPaths();

    const auto start = std::chrono::steady_clock::now();
    const Result<SelectionOutcome> result = selector.select(request);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    ASSERT_TRUE(result.isOk());
    EXPECT_EQ(result.value().probe, ProbeOutcome::TimedOut);
    EXPECT_FALSE(result.value().selection.isHardware());
    EXPECT_EQ(result.value().selection.encoderName(), "libx264");
    EXPECT_TRUE(result.value().selection.isSoftwareFallback());
    // A wedged driver must not block the caller: the deadline, not the probe,
    // decides when select() returns. Generous ceiling — the assertion that
    // matters is that it is nowhere near the probe's own 60 s bound.
    EXPECT_LT(elapsed, 30s);

    {
        std::lock_guard<std::mutex> lock(gate->m);
        gate->open = true;
    }
    gate->cv.notify_all();
}

TEST(EncoderSelector, RealAwaiterReturnsImmediatelyWhenTheProbeAnswers) {
    // The full 3000 ms deadline is configured, and the test still completes at
    // once: the deadline is a ceiling, never a wait.
    EncoderSelector::Options options;
    options.awaiter = EncoderSelector::detachedThreadAwaiter();
    options.probe = EncoderSelector::capsCapabilityProbe();
    const EncoderSelector selector{options};
    ASSERT_EQ(selector.probeDeadline(), 3000ms);

    EncoderSelectionRequest request;
    request.codec = gpu::CodecId::HEVC;
    request.caps = capableCaps(gpu::GpuVendor::Intel, gpu::CodecId::HEVC);
    request.availability = allVendorPaths();

    const auto start = std::chrono::steady_clock::now();
    const Result<SelectionOutcome> result = selector.select(request);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    ASSERT_TRUE(result.isOk());
    EXPECT_EQ(result.value().probe, ProbeOutcome::Supported);
    EXPECT_EQ(result.value().selection.encoderName(), "hevc_qsv");
    EXPECT_LT(elapsed, 1000ms);
}

TEST(HardwareSkip, ReasonNamesTheMissingSdkOrDeviceAndIsAbsentWhenHardwareIsPresent) {
    // The task-9.1 skip helper, checked on SYNTHETIC availability/caps so this
    // test itself never skips and holds identically on a GPU host and here.
    // Requirement 15.5: the reason must name the absent SDK or the absent device.
    const gpu::BridgeAvailability none = gpu::BridgeAvailability::softwareOnly();
    const std::optional<std::string> noSdk = test_support::hardwareSkipReason(
        gpu::CodecId::H264, gpu::CodecOperation::Encode, none, gpu::GpuCaps::software());
    ASSERT_TRUE(noSdk.has_value());
    EXPECT_NE(noSdk->find("PALMIER_HAVE_NVENC"), std::string::npos);
    EXPECT_NE(noSdk->find("H.264"), std::string::npos);

    // Paths compiled in, but the device reports nothing: the reason names the device.
    const std::optional<std::string> noDevice = test_support::hardwareSkipReason(
        gpu::CodecId::HEVC, gpu::CodecOperation::Encode, gpu::BridgeAvailability::all(),
        gpu::GpuCaps::software());
    ASSERT_TRUE(noDevice.has_value());
    EXPECT_NE(noDevice->find("HEVC"), std::string::npos);
    EXPECT_NE(noDevice->find("device"), std::string::npos);

    // A compiled-in path plus a capable device: nothing to skip for.
    EXPECT_FALSE(test_support::hardwareSkipReason(gpu::CodecId::VP9,
                                                 gpu::CodecOperation::Encode,
                                                 gpu::BridgeAvailability::all(),
                                                 capableCaps(gpu::GpuVendor::NVIDIA,
                                                             gpu::CodecId::VP9))
                     .has_value());
}

TEST(EncoderSelector, NegativeProbeSelectsSoftwareAndNamesTheAbsentDevice) {
    EncoderSelector::Options options;
    options.awaiter = virtualTimeAwaiter(0ms);
    options.probe = [](gpu::CodecId, const gpu::GpuCaps&) { return false; };
    const EncoderSelector selector{options};

    EncoderSelectionRequest request;
    request.codec = gpu::CodecId::VP9;
    request.caps = capableCaps(gpu::GpuVendor::AMD, gpu::CodecId::VP9);
    request.availability = allVendorPaths();

    const Result<SelectionOutcome> result = selector.select(request);
    ASSERT_TRUE(result.isOk());
    EXPECT_EQ(result.value().probe, ProbeOutcome::Unsupported);
    EXPECT_EQ(result.value().selection.encoderName(), "libvpx-vp9");
    EXPECT_TRUE(result.value().selection.isSoftwareFallback());
    EXPECT_NE(result.value().selection.fallbackReason().find("no VP9-compatible device"),
              std::string::npos);
}

} // namespace palmier::media
