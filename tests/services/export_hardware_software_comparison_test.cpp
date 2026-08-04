// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/services/export_hardware_software_comparison_test.cpp — task 9.8 of the
// end-to-end-editor-integration spec; Requirements 8.6 and 15.5.
//
// Requirement 8.6 is a comparison, and it is the only export requirement that
// cannot be satisfied by reasoning about the encoder *selection*: it demands two
// real coded outputs.
//
//   "WHEN the same fixture timeline (at least 300 frames, 1920x1080, 30 frames per
//    second) is exported once with hardware encoding and once with software
//    encoding, THE Export_Coordinator SHALL produce two decodable outputs whose
//    frame counts are both equal to the fixture frame count and whose durations
//    differ by no more than one frame interval (1/30 s, approximately 33.4 ms)."
//
// So this file exports the SAME timeline twice through the SAME
// services::ExportCoordinator API, once with `preferHardware = true` and once with
// `preferHardware = false`, and then asserts on the two files that come out — that
// each probes and DECODES, that each yields exactly the fixture frame count of
// decoded frames, and that the two durations agree to within one frame interval.
//
// ## What is real here, and what is injected
//
// Everything on the encode and decode side is REAL: the production
// `media::ffmpegEncodeBackendFactory()` writes the bytes, the production GPU
// context selection decides what hardware is available, and the outputs are read
// back with `media::probeMediaFile` and a real `media::MediaDecoder`. That is the
// whole point of the task — a mocked encode backend cannot produce a decodable
// output, so it could not answer Requirement 8.6 at all.
//
// The ONE injected collaborator is the clip-frame provider, which paints a
// deterministic solid frame for every clip position. That is deliberate and it
// does not weaken the assertion:
//
//   * Requirement 8.6 compares two encodes of the same *timeline*, not two
//     decodes of an input file. What must be identical between the two runs is
//     the pixel sequence fed to the encoder, and an injected deterministic
//     provider guarantees that far more strongly than decoding a fixture file
//     twice would.
//   * A 300-frame 1920x1080 input fixture would itself have to be encoded first,
//     which on a host with no encoder is circular.
//
// ## This test SKIPS unless the host can really do both halves
//
// Requirement 15.5 requires a hardware-encode test on a host without the hardware
// to be reported as skipped "with a recorded reason naming the absent SDK or
// device", never as a failure. There are TWO independent ways this particular
// test can be unrunnable, and they are different missing things, so they are
// reported as different reasons:
//
//   1. **No hardware encoder.** No vendor path compiled in, or no capable device.
//      This is exactly what `PALMIER_SKIP_WITHOUT_HW` (task 9.1) answers, and the
//      macro is used for it below.
//   2. **No software encoder to compare against.** The hardware half could be
//      present and the comparison still impossible, because FFmpeg was built
//      without the external software encoder for the codec (`libx264` for H.264).
//      `PALMIER_SKIP_WITHOUT_HW` says nothing about this — it only inspects vendor
//      hardware — so this file probes it directly by asking the production encode
//      backend factory to open a software route, which is the same call the export
//      itself would make.
//
// When both halves are missing the skip reason names both, so the recorded reason
// is never misleading about why the comparison did not happen.
//
// **On the sandbox this was written on, both halves are missing** — libavcodec
// 60.31.102 built with no `libx264`/`libx265`/`libvpx-vp9`, and no vendor encoder
// — so this test skips here and its assertions are UNVERIFIED. They must be
// exercised on a host with a real encoder stack before task 9.8 counts as
// verified rather than merely written.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

#include <unistd.h> // getpid, for a per-process scratch directory name

#include "core/Duration.hpp"
#include "core/FrameRate.hpp"
#include "core/Project.hpp"
#include "core/Resolution.hpp"
#include "core/Result.hpp"
#include "core/Uuid.hpp"
#include "gpu/CodecBridge.hpp"
#include "gpu/GpuContext.hpp"
#include "gpu/GpuTypes.hpp"
#include "media/DecoderTeardownQueue.hpp"
#include "media/MediaDecoder.hpp"
#include "media/MediaEncoder.hpp"
#include "media/MediaProbe.hpp"
#include "services/ExportCoordinator.hpp"
#include "services/ProjectSession.hpp"
#include "support/HardwareSkip.hpp"

namespace palmier::services {
namespace {

using test_support::hardwareSkipReason;

// ---------------------------------------------------------------------------
// The fixture timeline of Requirement 8.6
// ---------------------------------------------------------------------------

/// "at least 300 frames" — exactly 300, so the assertion has a single expected
/// number rather than an inequality.
constexpr std::size_t kFixtureFrames = 300;

/// "1920x1080" and "30 frames per second".
const Resolution kFixtureResolution = Resolution::hd1080();

/// The codec the comparison runs on. H.264 is the codec Requirement 8.5's L4
/// validation lane uses (`h264_nvenc`), so it is the one with a named vendor
/// encoder to compare the software encoder against.
constexpr gpu::CodecId kComparisonCodec = gpu::CodecId::H264;

/// Encoding 300 frames of 1080p twice is real work; this budget is generous
/// enough not to be flaky on a loaded CI machine and still bounded, so a
/// coordinator that never finishes fails the test instead of hanging it.
constexpr std::chrono::milliseconds kExportBudget{600'000};

[[nodiscard]] FrameRate fixtureFrameRate() { return FrameRate::fps30(); }

/// "no more than one frame interval (1/30 s, approximately 33.4 ms)".
[[nodiscard]] Duration oneFrameInterval() { return fixtureFrameRate().frameDuration(); }

// ---------------------------------------------------------------------------
// Scratch space
// ---------------------------------------------------------------------------
//
// gtest_discover_tests runs one process per case and ctest runs those in
// parallel, so the directory name carries getpid() and every path handed to the
// coordinator is absolute.

[[nodiscard]] const std::filesystem::path& scratchRoot() {
    static const std::filesystem::path root = [] {
        std::filesystem::path dir =
            std::filesystem::temp_directory_path() /
            ("palmier_export_hw_sw_" + std::to_string(static_cast<long long>(::getpid())));
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir;
    }();
    return root;
}

[[nodiscard]] std::filesystem::path scratchPath(const std::string& name) {
    return scratchRoot() / (name + ".mp4");
}

// ---------------------------------------------------------------------------
// "Is there a software encoder to compare against?"
// ---------------------------------------------------------------------------

/// The software encode route for `codec` — the route `media::EncoderSelector`
/// resolves to when hardware is unavailable or not requested.
[[nodiscard]] gpu::CodecRoute softwareEncodeRoute(gpu::CodecId codec) {
    gpu::CodecRoute route;
    route.codec = codec;
    route.operation = gpu::CodecOperation::Encode;
    route.backend = gpu::CodecBackend::FFmpegSoftware;
    route.hardware = false;
    route.softwareEncoder = std::string(gpu::softwareEncoderName(codec));
    route.detail = "FFmpeg software (task 9.8 availability probe)";
    return route;
}

/// Why this host has no SOFTWARE encoder for `codec` to compare the hardware
/// encoder against, or std::nullopt when it has one.
///
/// This asks the production encode backend factory to actually open a software
/// route at the fixture's geometry and then throws the result away — the same
/// call the export performs, so a positive answer here means the export's own
/// software half will open too. Nothing is asserted about the bytes; the question
/// is only whether libavcodec carries the encoder at all.
[[nodiscard]] std::optional<std::string> softwareEncoderSkipReason(gpu::CodecId codec) {
    const std::string encoderName{gpu::softwareEncoderName(codec)};

    if (!media::isFfmpegEncodeAvailable()) {
        return "this build has no FFmpeg encode support at all (PALMIER_HAVE_FFMPEG is "
               "undefined), so there is no software " +
               test_support::hardwareCodecName(codec) +
               " encoder to compare a hardware encode against";
    }

    media::EncodeSpec spec;
    spec.codec = codec;
    spec.bitrateBitsPerSecond = 4'000'000;
    spec.resolution = kFixtureResolution;
    spec.frameRate = fixtureFrameRate();
    spec.preferHardware = false;
    spec.outputPath = scratchPath("software_encoder_probe");
    spec.containerFormat = "mp4";

    // The failure message is captured BEFORE anything is released, so the reason
    // survives the cleanup below.
    std::optional<std::string> failure;
    {
        Result<std::unique_ptr<media::IEncodeBackend>> backend =
            media::ffmpegEncodeBackendFactory()(spec, softwareEncodeRoute(codec));
        if (backend.isError()) {
            failure = backend.error().message();
        } else if (backend.value() != nullptr) {
            // Opened: release the file handle again. The probe asserts nothing
            // about the bytes, only that the encoder exists and initializes.
            (void)backend.value()->finish();
        }
    }

    // Whatever happened, do not leave the probe's file behind.
    std::error_code ec;
    std::filesystem::remove(spec.outputPath, ec);

    if (failure.has_value()) {
        return "libavcodec on this host carries no software " +
               test_support::hardwareCodecName(codec) + " encoder (\"" + encoderName +
               "\"): opening a software encode route failed with \"" + *failure +
               "\", so a hardware encode has nothing to be compared against";
    }
    return std::nullopt;
}

/// The combined Requirement 15.5 skip reason, naming EVERY half that is missing
/// and keeping the two causes distinguishable.
[[nodiscard]] std::string comparisonSkipReason(const std::optional<std::string>& hardware,
                                               const std::optional<std::string>& software) {
    std::ostringstream why;
    why << "Requirement 8.6's hardware-versus-software comparison cannot run on this host, "
           "because neither half of the comparison is available:";
    if (hardware.has_value()) {
        why << "\n  * no hardware encoder: " << *hardware;
    }
    if (software.has_value()) {
        why << "\n  * no software encoder to compare against: " << *software;
    }
    why << "\nThe comparison needs BOTH, so it is reported as skipped rather than failed "
           "(Requirement 15.5).";
    return why.str();
}

// ---------------------------------------------------------------------------
// Reading an output back
// ---------------------------------------------------------------------------

struct DecodedOutput {
    std::size_t frames{0};
    Duration    duration{Duration::zero()};
};

/// Decode `path` from the beginning, counting video frames until end of stream.
/// Returns an Error rather than a count when the file cannot be opened or a
/// decode fails part-way, so "both outputs decode" is a real assertion.
[[nodiscard]] Result<DecodedOutput> decodeAndCount(const std::filesystem::path& path) {
    // Read both outputs back through the SOFTWARE decode path. The comparison is
    // about what was encoded, so the readback must not vary with whatever hardware
    // decoder happens to be present — otherwise the hardware output would be
    // checked by a different decoder than the software one.
    media::DecodePrefs prefs;
    prefs.preferHardware = false;

    Result<media::MediaDecoder> opened = media::MediaDecoder::open(path, prefs);
    if (opened.isError()) {
        return opened.error();
    }
    media::MediaDecoder decoder = std::move(opened.value());

    DecodedOutput out;
    // A hard ceiling, so a decoder that never reports end of stream fails the
    // test rather than looping forever.
    const std::size_t ceiling = kFixtureFrames * 4 + 128;
    while (out.frames < ceiling) {
        Result<media::DecodedFrame> frame = decoder.nextFrame();
        if (frame.isError()) {
            return frame.error();
        }
        if (frame.value().isEndOfStream()) {
            break;
        }
        ++out.frames;
    }
    return out;
}

// ---------------------------------------------------------------------------
// The fixture
// ---------------------------------------------------------------------------

class ExportHardwareSoftwareComparisonTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_unique<gpu::GpuContext>(gpu::GpuContext::softwareFallback());
        ASSERT_TRUE(session_
                        .createProject("export-hw-sw-comparison", fixtureFrameRate(),
                                       kFixtureResolution, defaultColorSpace())
                        .isOk());
        seedFixtureTimeline();
    }

    void TearDown() override { coordinator_.reset(); }

    /// The fixture timeline of Requirement 8.6: one video track carrying a single
    /// 300-frame clip at 1920x1080/30 fps, plus one audio track over the same
    /// range so the comparison covers a muxed two-stream output rather than a
    /// video-only one.
    void seedFixtureTimeline() {
        Project project = session_.engine().snapshot();
        project.timelineFps = fixtureFrameRate();
        project.canvas = kFixtureResolution;
        project.tracks.clear();

        const Duration span =
            fixtureFrameRate().durationForFrames(static_cast<std::int64_t>(kFixtureFrames));

        Clip video;
        video.id = Uuid::generateV4();
        video.timelineStart = Duration::zero();
        video.sourceIn = Duration::zero();
        video.sourceOut = span;
        video.opacity = 1.0;
        Track videoTrack;
        videoTrack.id = Uuid::generateV4();
        videoTrack.kind = TrackKind::Video;
        videoTrack.clips = {video};
        project.tracks.push_back(videoTrack);

        Clip audio;
        audio.id = Uuid::generateV4();
        audio.timelineStart = Duration::zero();
        audio.sourceIn = Duration::zero();
        audio.sourceOut = span;
        audio.gain = 1.0;
        Track audioTrack;
        audioTrack.id = Uuid::generateV4();
        audioTrack.kind = TrackKind::Audio;
        audioTrack.clips = {audio};
        project.tracks.push_back(audioTrack);

        ASSERT_TRUE(session_.engine().reset(project).isOk());
    }

    /// Options for a REAL export. Only the clip-frame provider is injected — the
    /// encode backend, the decode backend and the GPU context are the production
    /// defaults, because the hardware half of this comparison is meaningless
    /// otherwise.
    [[nodiscard]] ExportCoordinator::Options realExportOptions() {
        ExportCoordinator::Options opts;
        opts.frameProvider = [](const Clip&, Duration at) -> Result<gpu::SourceFrame> {
            // A position-dependent but deterministic colour ramp: identical across
            // the two runs, and varying between frames so the encoder is given
            // real inter-frame change to compress rather than 300 identical
            // frames.
            const auto step =
                static_cast<std::uint8_t>((at.milliseconds() / 33) % 256);
            return gpu::SourceFrame::solid(
                kFixtureResolution.width, kFixtureResolution.height,
                gpu::RgbaColor{step, static_cast<std::uint8_t>(255 - step), 128, 255});
        };
        return opts;
    }

    [[nodiscard]] ExportRequest2 comparisonRequest(const std::filesystem::path& output,
                                                   bool preferHardware) {
        ExportRequest2 r;
        r.outputPath = output;
        r.container = "mp4";
        r.codec = kComparisonCodec;
        r.resolution = kFixtureResolution;
        r.frameRate = fixtureFrameRate();
        r.bitrateKbps = 8'000;
        r.includeAudio = true;
        r.preferHardware = preferHardware;
        r.overwrite = true;
        return r;
    }

    /// Run one export of the fixture timeline to completion and return its
    /// outcome. A fresh coordinator per run, so neither run can observe the
    /// other's state.
    [[nodiscard]] std::optional<ExportOutcome> runExport(const std::filesystem::path& output,
                                                        bool preferHardware) {
        coordinator_ = std::make_unique<ExportCoordinator>(session_, *context_, teardown_,
                                                          realExportOptions());
        const Result<void> begun =
            coordinator_->begin(comparisonRequest(output, preferHardware));
        if (begun.isError()) {
            ADD_FAILURE() << "begin() refused the "
                          << (preferHardware ? "hardware" : "software")
                          << " export: " << begun.error().toString();
            return std::nullopt;
        }
        const std::size_t delivered = coordinator_->awaitCompletion(kExportBudget);
        EXPECT_GT(delivered, 0u)
            << "the " << (preferHardware ? "hardware" : "software")
            << " export did not finish inside the wait budget";
        return coordinator_->lastOutcome();
    }

    ProjectSession                     session_{};
    media::DecoderTeardownQueue        teardown_{};
    std::unique_ptr<gpu::GpuContext>   context_{};
    std::unique_ptr<ExportCoordinator> coordinator_{};
};

// ===========================================================================
// Requirement 8.6
// ===========================================================================

TEST_F(ExportHardwareSoftwareComparisonTest,
       HardwareAndSoftwareExportsOfTheFixtureAgreeOnFrameCountAndDuration) {
    // --- Requirement 15.5: skip, naming what is missing -------------------
    //
    // Both halves are needed. When both are absent the reason names both, so the
    // record never implies the hardware was the only problem.
    const std::optional<std::string> hardwareReason =
        hardwareSkipReason(kComparisonCodec, gpu::CodecOperation::Encode);
    const std::optional<std::string> softwareReason =
        softwareEncoderSkipReason(kComparisonCodec);

    if (hardwareReason.has_value() && softwareReason.has_value()) {
        GTEST_SKIP() << comparisonSkipReason(hardwareReason, softwareReason);
    }
    // Hardware missing but software present — task 9.1's macro owns this reason.
    PALMIER_SKIP_WITHOUT_HW(kComparisonCodec, gpu::CodecOperation::Encode);
    // Hardware present but no software encoder to compare it against.
    if (softwareReason.has_value()) {
        GTEST_SKIP() << "Requirement 8.6's comparison cannot run: no software encoder to "
                        "compare against — "
                     << *softwareReason;
    }

    // --- Both halves are available: perform the comparison ----------------

    const std::filesystem::path hardwareOut = scratchPath("hardware");
    const std::filesystem::path softwareOut = scratchPath("software");

    const std::optional<ExportOutcome> hardware = runExport(hardwareOut, /*preferHardware=*/true);
    ASSERT_TRUE(hardware.has_value()) << "the hardware export produced no outcome";
    EXPECT_FALSE(hardware->cancelled);
    EXPECT_TRUE(hardware->usedHardwareEncode)
        << "this host reports a usable hardware encoder, but the export fell back to software: "
        << hardware->fallbackReason;

    const std::optional<ExportOutcome> software = runExport(softwareOut, /*preferHardware=*/false);
    ASSERT_TRUE(software.has_value()) << "the software export produced no outcome";
    EXPECT_FALSE(software->cancelled);
    EXPECT_FALSE(software->usedHardwareEncode)
        << "the software run must not use a hardware encoder, or there is no comparison";

    // The two runs encoded the same timeline, so the coordinator's own counts must
    // both be the fixture frame count before the files are even opened.
    EXPECT_EQ(hardware->framesEncoded, kFixtureFrames);
    EXPECT_EQ(software->framesEncoded, kFixtureFrames);
    EXPECT_EQ(hardware->plannedFrames, kFixtureFrames);
    EXPECT_EQ(software->plannedFrames, kFixtureFrames);

    // --- "two decodable outputs" ------------------------------------------

    ASSERT_TRUE(std::filesystem::exists(hardwareOut));
    ASSERT_TRUE(std::filesystem::exists(softwareOut));
    EXPECT_GT(std::filesystem::file_size(hardwareOut), 0u);
    EXPECT_GT(std::filesystem::file_size(softwareOut), 0u);

    const Result<media::MediaInfo> hardwareProbe = media::probeMediaFile(hardwareOut);
    ASSERT_TRUE(hardwareProbe.isOk())
        << "the hardware output did not probe: " << hardwareProbe.error().toString();
    const Result<media::MediaInfo> softwareProbe = media::probeMediaFile(softwareOut);
    ASSERT_TRUE(softwareProbe.isOk())
        << "the software output did not probe: " << softwareProbe.error().toString();
    EXPECT_TRUE(hardwareProbe.value().hasVideo());
    EXPECT_TRUE(softwareProbe.value().hasVideo());

    const Result<DecodedOutput> hardwareDecoded = decodeAndCount(hardwareOut);
    ASSERT_TRUE(hardwareDecoded.isOk())
        << "the hardware output did not decode: " << hardwareDecoded.error().toString();
    const Result<DecodedOutput> softwareDecoded = decodeAndCount(softwareOut);
    ASSERT_TRUE(softwareDecoded.isOk())
        << "the software output did not decode: " << softwareDecoded.error().toString();

    // --- "frame counts are both equal to the fixture frame count" ---------

    EXPECT_EQ(hardwareDecoded.value().frames, kFixtureFrames);
    EXPECT_EQ(softwareDecoded.value().frames, kFixtureFrames);

    // --- "durations differ by no more than one frame interval" ------------
    //
    // Compared on the probed container durations, which is what a consumer of the
    // two files would see, rather than on anything the coordinator reports about
    // itself.
    const Duration hardwareDuration = hardwareProbe.value().duration;
    const Duration softwareDuration = softwareProbe.value().duration;
    const Duration delta = (hardwareDuration - softwareDuration).abs();
    EXPECT_LE(delta.ticks(), oneFrameInterval().ticks())
        << "hardware duration " << hardwareDuration.milliseconds() << " ms vs software "
        << softwareDuration.milliseconds() << " ms differ by " << delta.milliseconds()
        << " ms, more than the one frame interval of "
        << oneFrameInterval().milliseconds() << " ms";

    // Each output must also match the timeline it came from, or two equally wrong
    // outputs would satisfy the comparison above.
    const Duration expected =
        fixtureFrameRate().durationForFrames(static_cast<std::int64_t>(kFixtureFrames));
    EXPECT_LE((hardwareDuration - expected).abs().ticks(), oneFrameInterval().ticks());
    EXPECT_LE((softwareDuration - expected).abs().ticks(), oneFrameInterval().ticks());
}

// ===========================================================================
// The guard itself, asserted — so "it skipped" is never the whole story
// ===========================================================================
//
// The comparison above cannot run on a host without the encoders, which means on
// such a host nothing would be asserted at all and a permanently-skipping guard
// would be indistinguishable from a working one. This case closes that hole. It
// runs everywhere, and it pins the two things the guard must get right:
//
//   * The hardware gate is CONDITIONAL, not unconditional: driven with synthetic
//     "vendor path compiled in and device reports H.264 encode" inputs it produces
//     NO skip reason, which is what proves the body above is reachable on real
//     hardware rather than dead code.
//   * The two causes are reported DISTINGUISHABLY, which is what Requirement 15.5
//     asks of the recorded reason.
//
// `hardwareSkipReason` takes its availability and capabilities as parameters
// precisely so this can be checked without the hardware.
TEST(ExportHardwareSoftwareComparisonGuard, DistinguishesMissingHardwareFromMissingSoftware) {
    // --- A host that CAN encode H.264 in hardware: no reason to skip -------
    gpu::BridgeAvailability capable;
    capable.nvenc = true;
    capable.nvdec = true;
    capable.ffmpegSoftware = true;

    gpu::GpuCaps nvidia = gpu::GpuCaps::software();
    nvidia.vendor = "NVIDIA";
    nvidia.vendorId = gpu::GpuVendor::NVIDIA;
    nvidia.hwEncode = true;
    nvidia.encodeCodecs = {gpu::CodecId::H264};

    EXPECT_FALSE(hardwareSkipReason(kComparisonCodec, gpu::CodecOperation::Encode, capable,
                                    nvidia)
                     .has_value())
        << "the hardware gate must NOT skip on a host that reports a usable H.264 encoder — "
           "otherwise the comparison test could never run anywhere";

    // --- No vendor path compiled in: a reason naming the defines ----------
    gpu::BridgeAvailability none;
    none.ffmpegSoftware = true;
    const std::optional<std::string> noPath =
        hardwareSkipReason(kComparisonCodec, gpu::CodecOperation::Encode, none, nvidia);
    ASSERT_TRUE(noPath.has_value());
    EXPECT_NE(noPath->find("compiled in"), std::string::npos);

    // --- Compiled in, but the device cannot: a reason naming the device ---
    const std::optional<std::string> noDevice = hardwareSkipReason(
        kComparisonCodec, gpu::CodecOperation::Encode, capable, gpu::GpuCaps::software());
    ASSERT_TRUE(noDevice.has_value());
    EXPECT_NE(noDevice->find("device"), std::string::npos);

    // --- The two causes never read the same ------------------------------
    //
    // The software half is asked of the real host, whatever this host is. Either
    // answer is legitimate; what must hold is that when a software reason exists
    // it is about the software encoder and is not confusable with the hardware
    // one, and that the combined message keeps both halves separately labelled.
    const std::optional<std::string> software = softwareEncoderSkipReason(kComparisonCodec);
    if (software.has_value()) {
        EXPECT_NE(software->find("software"), std::string::npos);
        EXPECT_NE(*software, *noPath);
        EXPECT_NE(*software, *noDevice);

        const std::string combined = comparisonSkipReason(noPath, software);
        EXPECT_NE(combined.find("no hardware encoder:"), std::string::npos);
        EXPECT_NE(combined.find("no software encoder to compare against:"), std::string::npos);
    } else {
        // This host HAS a software H.264 encoder, so the comparison's software
        // half would run; only the hardware half can gate it.
        EXPECT_TRUE(media::isFfmpegEncodeAvailable());
    }
}

}  // namespace
}  // namespace palmier::services
