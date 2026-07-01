// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/media_decoder_test.cpp — unit tests for MediaDecoder (task 8.2;
// Requirements 3.1, 10.2, 10.5).
//
// These exercise the decoder's open/nextFrame/seek surface and, above all, its
// HW-preferred / SW-fallback routing and zero-copy frame handling — driven
// entirely through the IDecodeBackend seam with a scriptable mock. No GPU, no
// FFmpeg, and no vendor SDK are required: the mock stands in for the codec work
// and a host-memory FramePool stands in for GPU-resident frames.

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <utility>
#include <vector>

#include "core/Duration.hpp"
#include "core/Error.hpp"
#include "core/Result.hpp"
#include "gpu/FramePool.hpp"
#include "gpu/GpuTypes.hpp"
#include "media/MediaDecoder.hpp"
#include "media/MediaInfo.hpp"

namespace palmier::media {
namespace {

// --- Scriptable decode backend ---------------------------------------------

struct MockConfig {
    MediaCodecId  codec{MediaCodecId::H264};
    bool          failHardware{false};      ///< decode(true) returns an Error.
    bool          produceHardwareFrame{true};///< on a HW route, export a HW surface.
    bool          invalidExternal{false};   ///< HW surface carries an unusable handle.
    bool          eosNow{false};            ///< every decode reports end-of-stream.
    gpu::FrameDesc desc{16, 16, gpu::FrameFormat::RGBA8};
};

class MockDecodeBackend final : public IDecodeBackend {
public:
    explicit MockDecodeBackend(MockConfig cfg) : cfg_(cfg) {
        MediaStreamInfo video;
        video.index = 0;
        video.type = MediaStreamType::Video;
        video.codec = cfg_.codec;
        video.codecName = "mock";
        video.resolution = Resolution{cfg_.desc.width, cfg_.desc.height};
        info_.streams.push_back(video);
        hostBacking_.resize(cfg_.desc.byteSize());
    }

    [[nodiscard]] const MediaInfo& info() const override { return info_; }

    [[nodiscard]] Result<BackendFrame> decode(bool useHardware) override {
        ++decodeCalls;
        hwHistory.push_back(useHardware);

        if (cfg_.eosNow) return BackendFrame::eos();

        if (useHardware) {
            if (cfg_.failHardware) {
                return err<BackendFrame>(Error(ErrorCode::Internal, "VK_ERROR_DEVICE_LOST"));
            }
            if (cfg_.produceHardwareFrame) {
                BackendFrame f;
                f.hardware = true;
                f.desc = cfg_.desc;
                f.timestamp = Duration::fromMilliseconds(10);
                f.external = cfg_.invalidExternal
                                 ? gpu::ExternalImageSource{}                    // fd == -1: invalid
                                 : gpu::ExternalImageSource::dmaBuf(7, hostBacking_.data());
                return f;
            }
        }

        BackendFrame f;
        f.hardware = false;
        f.desc = cfg_.desc;
        f.timestamp = Duration::fromMilliseconds(10);
        f.cpuPixels.resize(cfg_.desc.byteSize());
        return f;
    }

    [[nodiscard]] Result<void> seek(Duration ts) override {
        ++seekCalls;
        lastSeek = ts;
        return ok();
    }

    int                decodeCalls{0};
    int                seekCalls{0};
    Duration           lastSeek{};
    std::vector<bool>  hwHistory{};

private:
    MockConfig             cfg_;
    MediaInfo              info_{};
    std::vector<std::byte> hostBacking_{};
};

// --- Helpers ---------------------------------------------------------------

gpu::GpuCaps nvidiaCaps() {
    gpu::GpuCaps c;
    c.vendorId = gpu::GpuVendor::NVIDIA;
    c.vendor = "NVIDIA";
    c.supportsCompute = true;
    c.hwDecode = true;
    c.hwEncode = true;
    c.decodeCodecs = {gpu::CodecId::H264, gpu::CodecId::HEVC, gpu::CodecId::AV1};
    c.encodeCodecs = {gpu::CodecId::H264, gpu::CodecId::HEVC};
    c.vramBytes = 4ull * 1024 * 1024 * 1024;
    return c;
}

// A factory that builds the mock and hands back a raw pointer for inspection.
// The decoder owns the backend on the heap, so the pointer stays valid for the
// decoder's lifetime.
DecodeBackendFactory mockFactory(MockConfig cfg, MockDecodeBackend** out) {
    return [cfg, out](const std::filesystem::path&, const DecodePrefs&)
               -> Result<std::unique_ptr<IDecodeBackend>> {
        auto backend = std::make_unique<MockDecodeBackend>(cfg);
        if (out != nullptr) *out = backend.get();
        return std::unique_ptr<IDecodeBackend>(std::move(backend));
    };
}

// --- open() ----------------------------------------------------------------

TEST(MediaDecoderOpen, PopulatesInfoAndVideoCodec) {
    MockDecodeBackend* mock = nullptr;
    DecodePrefs prefs;
    auto opened = MediaDecoder::open("clip.mp4", prefs, mockFactory({}, &mock));
    ASSERT_TRUE(opened.isOk());

    MediaDecoder decoder = std::move(opened).value();
    EXPECT_TRUE(decoder.info().hasVideo());
    EXPECT_EQ(decoder.videoCodec(), gpu::CodecId::H264);
}

TEST(MediaDecoderOpen, EmptyPathIsRejected) {
    auto opened = MediaDecoder::open("", DecodePrefs{}, mockFactory({}, nullptr));
    ASSERT_TRUE(opened.isError());
    EXPECT_EQ(opened.error().code(), ErrorCode::InvalidArgument);
}

TEST(MediaDecoderOpen, BackendFactoryErrorPropagates) {
    DecodeBackendFactory failing =
        [](const std::filesystem::path&, const DecodePrefs&)
            -> Result<std::unique_ptr<IDecodeBackend>> {
        return err<std::unique_ptr<IDecodeBackend>>(
            makeError(ErrorCode::Io, "could not open the file"));
    };
    auto opened = MediaDecoder::open("clip.mp4", DecodePrefs{}, failing);
    ASSERT_TRUE(opened.isError());
    EXPECT_EQ(opened.error().code(), ErrorCode::Io);
}

TEST(MediaDecoderOpen, DefaultFactoryReportsWhenFfmpegAbsent) {
    // Guarded so the assertion holds regardless of whether this build linked
    // FFmpeg: only the FFmpeg-absent build promises FailedPrecondition here.
    if (!isFfmpegDecodeAvailable()) {
        auto opened = MediaDecoder::open("clip.mp4", DecodePrefs{});
        ASSERT_TRUE(opened.isError());
        EXPECT_EQ(opened.error().code(), ErrorCode::FailedPrecondition);
    }
}

// --- nextFrame(): hardware path & zero-copy FramePool frames ----------------

TEST(MediaDecoderNextFrame, HardwareRouteYieldsGpuResidentZeroCopyFrame) {
    gpu::FramePool pool(16ull * 1024 * 1024);
    MockDecodeBackend* mock = nullptr;

    DecodePrefs prefs;
    prefs.preferHardware = true;
    prefs.caps = nvidiaCaps();
    prefs.framePool = &pool;
    prefs.availability = gpu::BridgeAvailability::all();

    auto decoder = MediaDecoder::open("clip.mp4", prefs, mockFactory({}, &mock)).value();
    auto frameRes = decoder.nextFrame();
    ASSERT_TRUE(frameRes.isOk());

    DecodedFrame frame = std::move(frameRes).value();
    EXPECT_TRUE(frame.isGpuResident());
    EXPECT_TRUE(frame.isZeroCopy());
    EXPECT_FALSE(frame.isCpu());
    EXPECT_FALSE(decoder.lastFrameRetriedOnCpu());

    // Exactly one hardware attempt was made, and the pool adopted the surface.
    ASSERT_EQ(mock->hwHistory.size(), 1u);
    EXPECT_TRUE(mock->hwHistory[0]);
    EXPECT_EQ(pool.stats().importedInUse, 1u);
}

// --- nextFrame(): transparent HW -> CPU fallback (Requirement 10.5) ---------

TEST(MediaDecoderNextFrame, HardwareFailureFallsBackToCpuTransparently) {
    gpu::FramePool pool(16ull * 1024 * 1024);
    MockDecodeBackend* mock = nullptr;

    DecodePrefs prefs;
    prefs.preferHardware = true;
    prefs.caps = nvidiaCaps();
    prefs.framePool = &pool;
    prefs.availability = gpu::BridgeAvailability::all();

    MockConfig cfg;
    cfg.failHardware = true; // GPU decode fails; must retry once on the CPU.

    auto decoder = MediaDecoder::open("clip.mp4", prefs, mockFactory(cfg, &mock)).value();
    auto frameRes = decoder.nextFrame();
    ASSERT_TRUE(frameRes.isOk());

    DecodedFrame frame = std::move(frameRes).value();
    EXPECT_TRUE(frame.isCpu());
    EXPECT_FALSE(frame.isGpuResident());
    EXPECT_TRUE(decoder.lastFrameRetriedOnCpu());

    // The same frame was reprocessed: hardware first, then CPU — never more.
    ASSERT_EQ(mock->hwHistory.size(), 2u);
    EXPECT_TRUE(mock->hwHistory[0]);
    EXPECT_FALSE(mock->hwHistory[1]);

    // The failure was recorded in the bridge log (Requirement 10.5).
    EXPECT_FALSE(decoder.bridge().log().empty());
}

TEST(MediaDecoderNextFrame, ZeroCopyImportFailureFallsBackToCpu) {
    gpu::FramePool pool(16ull * 1024 * 1024);
    MockDecodeBackend* mock = nullptr;

    DecodePrefs prefs;
    prefs.preferHardware = true;
    prefs.caps = nvidiaCaps();
    prefs.framePool = &pool;
    prefs.availability = gpu::BridgeAvailability::all();

    MockConfig cfg;
    cfg.produceHardwareFrame = true;
    cfg.invalidExternal = true; // the exported surface cannot be adopted.

    auto decoder = MediaDecoder::open("clip.mp4", prefs, mockFactory(cfg, &mock)).value();
    auto frame = decoder.nextFrame().value();

    EXPECT_TRUE(frame.isCpu());
    EXPECT_TRUE(decoder.lastFrameRetriedOnCpu());
    EXPECT_EQ(pool.stats().importedInUse, 0u);
}

TEST(MediaDecoderNextFrame, HardwareRouteWithoutPoolFallsBackToCpu) {
    MockDecodeBackend* mock = nullptr;

    DecodePrefs prefs;
    prefs.preferHardware = true;
    prefs.caps = nvidiaCaps();
    prefs.framePool = nullptr; // no pool to adopt HW surfaces into.
    prefs.availability = gpu::BridgeAvailability::all();

    auto decoder = MediaDecoder::open("clip.mp4", prefs, mockFactory({}, &mock)).value();
    auto frame = decoder.nextFrame().value();

    EXPECT_TRUE(frame.isCpu());
    EXPECT_TRUE(decoder.lastFrameRetriedOnCpu());
}

// --- nextFrame(): software routing conditions -------------------------------

TEST(MediaDecoderNextFrame, UnsupportedCodecRoutesToSoftwareNoRetry) {
    gpu::FramePool pool(16ull * 1024 * 1024);
    MockDecodeBackend* mock = nullptr;

    DecodePrefs prefs;
    prefs.preferHardware = true;
    prefs.caps = nvidiaCaps(); // decodeCodecs does NOT include VP9.
    prefs.framePool = &pool;
    prefs.availability = gpu::BridgeAvailability::all();

    MockConfig cfg;
    cfg.codec = MediaCodecId::VP9;

    auto decoder = MediaDecoder::open("clip.webm", prefs, mockFactory(cfg, &mock)).value();
    auto frame = decoder.nextFrame().value();

    EXPECT_TRUE(frame.isCpu());
    EXPECT_FALSE(decoder.lastFrameRetriedOnCpu());
    // Routed straight to software: a single CPU attempt, no hardware try.
    ASSERT_EQ(mock->hwHistory.size(), 1u);
    EXPECT_FALSE(mock->hwHistory[0]);
}

TEST(MediaDecoderNextFrame, PreferHardwareFalseForcesSoftware) {
    gpu::FramePool pool(16ull * 1024 * 1024);
    MockDecodeBackend* mock = nullptr;

    DecodePrefs prefs;
    prefs.preferHardware = false; // opt out of hardware entirely.
    prefs.caps = nvidiaCaps();
    prefs.framePool = &pool;
    prefs.availability = gpu::BridgeAvailability::all();

    auto decoder = MediaDecoder::open("clip.mp4", prefs, mockFactory({}, &mock)).value();
    auto frame = decoder.nextFrame().value();

    EXPECT_TRUE(frame.isCpu());
    ASSERT_EQ(mock->hwHistory.size(), 1u);
    EXPECT_FALSE(mock->hwHistory[0]);
}

TEST(MediaDecoderNextFrame, SoftwareOnlyCapsProduceCpuFrames) {
    MockDecodeBackend* mock = nullptr;

    DecodePrefs prefs;
    prefs.preferHardware = true;
    prefs.caps = gpu::GpuCaps::software();
    prefs.availability = gpu::BridgeAvailability::all();

    auto decoder = MediaDecoder::open("clip.mp4", prefs, mockFactory({}, &mock)).value();
    auto frame = decoder.nextFrame().value();

    EXPECT_TRUE(frame.isCpu());
    EXPECT_FALSE(decoder.lastFrameRetriedOnCpu());
}

TEST(MediaDecoderNextFrame, EndOfStreamIsReported) {
    MockDecodeBackend* mock = nullptr;
    MockConfig cfg;
    cfg.eosNow = true;

    auto decoder = MediaDecoder::open("clip.mp4", DecodePrefs{}, mockFactory(cfg, &mock)).value();
    auto frame = decoder.nextFrame().value();

    EXPECT_TRUE(frame.isEndOfStream());
    EXPECT_FALSE(frame.isCpu());
    EXPECT_FALSE(frame.isGpuResident());
}

// --- seek() -----------------------------------------------------------------

TEST(MediaDecoderSeek, DelegatesToBackend) {
    MockDecodeBackend* mock = nullptr;
    auto decoder = MediaDecoder::open("clip.mp4", DecodePrefs{}, mockFactory({}, &mock)).value();

    const Duration target = Duration::fromMilliseconds(1500);
    ASSERT_TRUE(decoder.seek(target).isOk());
    EXPECT_EQ(mock->seekCalls, 1);
    EXPECT_EQ(mock->lastSeek, target);
}

// --- codec mapping ----------------------------------------------------------

TEST(ToGpuCodec, MapsKnownVideoCodecs) {
    EXPECT_EQ(toGpuCodec(MediaCodecId::H264), gpu::CodecId::H264);
    EXPECT_EQ(toGpuCodec(MediaCodecId::HEVC), gpu::CodecId::HEVC);
    EXPECT_EQ(toGpuCodec(MediaCodecId::AV1), gpu::CodecId::AV1);
    EXPECT_EQ(toGpuCodec(MediaCodecId::VP9), gpu::CodecId::VP9);
    EXPECT_EQ(toGpuCodec(MediaCodecId::Mpeg2Video), gpu::CodecId::MPEG2);
    // Codecs the GPU bridge does not enumerate map to Unknown (software route).
    EXPECT_EQ(toGpuCodec(MediaCodecId::ProRes), gpu::CodecId::Unknown);
    EXPECT_EQ(toGpuCodec(MediaCodecId::AAC), gpu::CodecId::Unknown);
}

} // namespace
} // namespace palmier::media
