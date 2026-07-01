// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/media_encoder_test.cpp — unit tests for MediaEncoder (task 8.3;
// Requirements 10.2, 10.5).
//
// These exercise the encoder's create/submit/finish surface and, above all, its
// HW-preferred / SW-fallback-on-init routing and its acceptance of both GPU-
// resident (zero-copy) and CPU frames — driven entirely through the
// IEncodeBackend seam with a scriptable mock. No GPU, no FFmpeg, and no vendor
// SDK are required: the mock stands in for the codec/mux work and a host-memory
// FramePool stands in for GPU-resident frames.

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "core/Duration.hpp"
#include "core/Error.hpp"
#include "core/FrameRate.hpp"
#include "core/Resolution.hpp"
#include "core/Result.hpp"
#include "gpu/CodecBridge.hpp"
#include "gpu/Compositor.hpp"
#include "gpu/FramePool.hpp"
#include "gpu/GpuTypes.hpp"
#include "media/MediaEncoder.hpp"

namespace palmier::media {
namespace {

// --- Scriptable encode backend ---------------------------------------------

struct MockEncodeConfig {
    bool failHardwareInit{false}; ///< factory errors when route.hardware (forces SW retry).
    bool failAllInit{false};      ///< factory always errors.
    bool failEncode{false};       ///< encode() returns an Error.
    bool failFinish{false};       ///< finish() returns an Error.
};

class MockEncodeBackend final : public IEncodeBackend {
public:
    explicit MockEncodeBackend(MockEncodeConfig cfg) : cfg_(cfg) {}

    [[nodiscard]] Result<void> encode(const EncoderInputFrame& frame) override {
        ++encodeCalls;
        if (cfg_.failEncode) {
            return err(Error(ErrorCode::Io, "mock encode failure"));
        }
        presentations.push_back(frame.presentation);
        gpuResident.push_back(frame.gpuResident);
        hadHostData.push_back(frame.hostData != nullptr);
        return ok();
    }

    [[nodiscard]] Result<void> finish() override {
        ++finishCalls;
        if (cfg_.failFinish) return err(Error(ErrorCode::Io, "mock finish failure"));
        return ok();
    }

    int                    encodeCalls{0};
    int                    finishCalls{0};
    std::vector<Duration>  presentations{};
    std::vector<bool>      gpuResident{};
    std::vector<bool>      hadHostData{};

private:
    MockEncodeConfig cfg_;
};

// Shared, test-owned view of how the factory was driven and the backend it built.
struct MockEncodeState {
    std::vector<gpu::CodecRoute> initRoutes{}; ///< routes the factory was invoked with.
    MockEncodeBackend*           backend{nullptr};
};

EncodeBackendFactory mockFactory(MockEncodeConfig cfg, MockEncodeState* state) {
    return [cfg, state](const EncodeSpec&, const gpu::CodecRoute& route)
               -> Result<std::unique_ptr<IEncodeBackend>> {
        if (state != nullptr) state->initRoutes.push_back(route);
        if (cfg.failAllInit) {
            return err<std::unique_ptr<IEncodeBackend>>(
                Error(ErrorCode::Internal, "mock init failure"));
        }
        if (cfg.failHardwareInit && route.hardware) {
            return err<std::unique_ptr<IEncodeBackend>>(
                Error(ErrorCode::Internal, "mock hardware encoder init failure"));
        }
        auto backend = std::make_unique<MockEncodeBackend>(cfg);
        if (state != nullptr) state->backend = backend.get();
        return std::unique_ptr<IEncodeBackend>(std::move(backend));
    };
}

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

constexpr Resolution kRes{16, 16};

EncodeSpec baseSpec() {
    EncodeSpec s;
    s.codec = gpu::CodecId::H264;
    s.bitrateBitsPerSecond = 4'000'000;
    s.resolution = kRes;
    s.frameRate = FrameRate::fps30();
    return s;
}

// A CPU (host-memory pooled) RenderedFrame of resolution `res` at time `pts`.
gpu::RenderedFrame makeCpuFrame(gpu::FramePool& pool, Resolution res, Duration pts) {
    gpu::FrameDesc desc{res.width, res.height, gpu::FrameFormat::RGBA8};
    auto lease = pool.acquire(desc).value();
    return gpu::RenderedFrame(std::move(lease), pts, /*layerCount*/ 1);
}

// A GPU-resident (zero-copy imported) RenderedFrame. `backing` provides a host
// mapping and must outlive the returned frame.
gpu::RenderedFrame makeGpuFrame(gpu::FramePool& pool, std::vector<std::byte>& backing,
                                Resolution res, Duration pts) {
    gpu::FrameDesc desc{res.width, res.height, gpu::FrameFormat::RGBA8};
    backing.resize(desc.byteSize());
    auto lease =
        pool.acquireImported(desc, gpu::ExternalImageSource::dmaBuf(7, backing.data())).value();
    return gpu::RenderedFrame(std::move(lease), pts, /*layerCount*/ 1);
}

// --- create(): validation ---------------------------------------------------

TEST(MediaEncoderCreate, RejectsDegenerateResolution) {
    EncodeSpec spec = baseSpec();
    spec.resolution = Resolution{0, 0};
    auto enc = MediaEncoder::create(spec, mockFactory({}, nullptr));
    ASSERT_TRUE(enc.isError());
    EXPECT_EQ(enc.error().code(), ErrorCode::InvalidArgument);
}

TEST(MediaEncoderCreate, RejectsInvalidFrameRate) {
    EncodeSpec spec = baseSpec();
    spec.frameRate = FrameRate{0, 0};
    auto enc = MediaEncoder::create(spec, mockFactory({}, nullptr));
    ASSERT_TRUE(enc.isError());
    EXPECT_EQ(enc.error().code(), ErrorCode::InvalidArgument);
}

TEST(MediaEncoderCreate, RejectsNegativeBitrate) {
    EncodeSpec spec = baseSpec();
    spec.bitrateBitsPerSecond = -1;
    auto enc = MediaEncoder::create(spec, mockFactory({}, nullptr));
    ASSERT_TRUE(enc.isError());
    EXPECT_EQ(enc.error().code(), ErrorCode::InvalidArgument);
}

TEST(MediaEncoderCreate, RejectsUnknownCodec) {
    EncodeSpec spec = baseSpec();
    spec.codec = gpu::CodecId::Unknown;
    auto enc = MediaEncoder::create(spec, mockFactory({}, nullptr));
    ASSERT_TRUE(enc.isError());
    EXPECT_EQ(enc.error().code(), ErrorCode::Unsupported);
}

TEST(MediaEncoderCreate, DefaultFactoryReportsWhenFfmpegAbsent) {
    // Guarded so the assertion holds regardless of whether this build linked
    // FFmpeg: only the FFmpeg-absent build promises FailedPrecondition here.
    if (!isFfmpegEncodeAvailable()) {
        auto enc = MediaEncoder::create(baseSpec());
        ASSERT_TRUE(enc.isError());
        EXPECT_EQ(enc.error().code(), ErrorCode::FailedPrecondition);
    }
}

TEST(MediaEncoderCreate, InitFailurePropagatesAfterFallbackExhausted) {
    MockEncodeState state;
    EncodeSpec spec = baseSpec();
    spec.preferHardware = false; // software route; no HW retry to fall back to.
    auto enc = MediaEncoder::create(spec, mockFactory({.failAllInit = true}, &state));
    ASSERT_TRUE(enc.isError());
    EXPECT_EQ(enc.error().code(), ErrorCode::Internal);
}

// --- create(): routing (HW-preferred, SW fallback) --------------------------

TEST(MediaEncoderCreate, HardwareRouteWhenDeviceSupportsEncode) {
    MockEncodeState state;
    EncodeSpec spec = baseSpec();
    spec.preferHardware = true;
    spec.caps = nvidiaCaps();
    spec.availability = gpu::BridgeAvailability::all();

    auto enc = MediaEncoder::create(spec, mockFactory({}, &state)).value();
    EXPECT_TRUE(enc.isHardware());
    EXPECT_FALSE(enc.usedSoftwareFallback());
    EXPECT_EQ(enc.route().backend, gpu::CodecBackend::Nvenc);

    // Exactly one init attempt, on the hardware route.
    ASSERT_EQ(state.initRoutes.size(), 1u);
    EXPECT_TRUE(state.initRoutes[0].hardware);
}

TEST(MediaEncoderCreate, HardwareInitFailureRetriesOnSoftwareEncoder) {
    MockEncodeState state;
    EncodeSpec spec = baseSpec();
    spec.preferHardware = true;
    spec.caps = nvidiaCaps();
    spec.availability = gpu::BridgeAvailability::all();

    // Hardware encoder init fails: create() must retry once on the SW encoder.
    auto enc = MediaEncoder::create(spec, mockFactory({.failHardwareInit = true}, &state)).value();

    EXPECT_FALSE(enc.isHardware());
    EXPECT_TRUE(enc.usedSoftwareFallback());
    EXPECT_EQ(enc.route().backend, gpu::CodecBackend::FFmpegSoftware);

    // The same init was retried: hardware first, then software — never more.
    ASSERT_EQ(state.initRoutes.size(), 2u);
    EXPECT_TRUE(state.initRoutes[0].hardware);
    EXPECT_FALSE(state.initRoutes[1].hardware);

    // The failure was recorded in the bridge log (Requirement 10.5).
    EXPECT_FALSE(enc.bridge().log().empty());
}

TEST(MediaEncoderCreate, PreferHardwareFalseForcesSoftware) {
    MockEncodeState state;
    EncodeSpec spec = baseSpec();
    spec.preferHardware = false; // opt out of hardware entirely.
    spec.caps = nvidiaCaps();
    spec.availability = gpu::BridgeAvailability::all();

    auto enc = MediaEncoder::create(spec, mockFactory({}, &state)).value();
    EXPECT_FALSE(enc.isHardware());
    EXPECT_FALSE(enc.usedSoftwareFallback());
    ASSERT_EQ(state.initRoutes.size(), 1u);
    EXPECT_FALSE(state.initRoutes[0].hardware);
}

// --- submit(): GPU or CPU frames --------------------------------------------

TEST(MediaEncoderSubmit, AcceptsCpuFrame) {
    gpu::FramePool pool(16ull * 1024 * 1024);
    MockEncodeState state;
    auto enc = MediaEncoder::create(baseSpec(), mockFactory({}, &state)).value();

    auto frame = makeCpuFrame(pool, kRes, Duration::fromMilliseconds(0));
    ASSERT_TRUE(enc.submit(frame).isOk());
    EXPECT_EQ(enc.submittedFrameCount(), 1u);

    ASSERT_EQ(state.backend->encodeCalls, 1);
    ASSERT_EQ(state.backend->gpuResident.size(), 1u);
    EXPECT_FALSE(state.backend->gpuResident[0]);
    EXPECT_TRUE(state.backend->hadHostData[0]);
}

TEST(MediaEncoderSubmit, AcceptsGpuResidentZeroCopyFrame) {
    gpu::FramePool pool(16ull * 1024 * 1024);
    std::vector<std::byte> backing;
    MockEncodeState state;
    auto enc = MediaEncoder::create(baseSpec(), mockFactory({}, &state)).value();

    auto frame = makeGpuFrame(pool, backing, kRes, Duration::fromMilliseconds(0));
    ASSERT_TRUE(frame.image().isZeroCopy());
    ASSERT_TRUE(enc.submit(frame).isOk());
    EXPECT_EQ(enc.submittedFrameCount(), 1u);

    ASSERT_EQ(state.backend->gpuResident.size(), 1u);
    EXPECT_TRUE(state.backend->gpuResident[0]);
}

TEST(MediaEncoderSubmit, QueuesFramesInPresentationOrder) {
    gpu::FramePool pool(16ull * 1024 * 1024);
    MockEncodeState state;
    auto enc = MediaEncoder::create(baseSpec(), mockFactory({}, &state)).value();

    auto f0 = makeCpuFrame(pool, kRes, Duration::fromMilliseconds(0));
    auto f1 = makeCpuFrame(pool, kRes, Duration::fromMilliseconds(33));
    auto f2 = makeCpuFrame(pool, kRes, Duration::fromMilliseconds(66));
    ASSERT_TRUE(enc.submit(f0).isOk());
    ASSERT_TRUE(enc.submit(f1).isOk());
    ASSERT_TRUE(enc.submit(f2).isOk());

    ASSERT_EQ(state.backend->presentations.size(), 3u);
    EXPECT_EQ(state.backend->presentations[0], Duration::fromMilliseconds(0));
    EXPECT_EQ(state.backend->presentations[1], Duration::fromMilliseconds(33));
    EXPECT_EQ(state.backend->presentations[2], Duration::fromMilliseconds(66));
}

// --- submit(): validation guards (no stream corruption) ---------------------

TEST(MediaEncoderSubmit, RejectsResolutionMismatch) {
    gpu::FramePool pool(16ull * 1024 * 1024);
    MockEncodeState state;
    auto enc = MediaEncoder::create(baseSpec(), mockFactory({}, &state)).value();

    auto frame = makeCpuFrame(pool, Resolution{32, 32}, Duration::fromMilliseconds(0));
    auto res = enc.submit(frame);
    ASSERT_TRUE(res.isError());
    EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
    // The frame was not queued: the stream is uncorrupted.
    EXPECT_EQ(enc.submittedFrameCount(), 0u);
    EXPECT_EQ(state.backend->encodeCalls, 0);
}

TEST(MediaEncoderSubmit, RejectsOutOfOrderPresentation) {
    gpu::FramePool pool(16ull * 1024 * 1024);
    MockEncodeState state;
    auto enc = MediaEncoder::create(baseSpec(), mockFactory({}, &state)).value();

    auto f0 = makeCpuFrame(pool, kRes, Duration::fromMilliseconds(50));
    ASSERT_TRUE(enc.submit(f0).isOk());

    auto f1 = makeCpuFrame(pool, kRes, Duration::fromMilliseconds(10)); // regresses.
    auto res = enc.submit(f1);
    ASSERT_TRUE(res.isError());
    EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
    // Only the first frame was queued.
    EXPECT_EQ(enc.submittedFrameCount(), 1u);
}

TEST(MediaEncoderSubmit, RejectsInvalidFrame) {
    MockEncodeState state;
    auto enc = MediaEncoder::create(baseSpec(), mockFactory({}, &state)).value();

    gpu::RenderedFrame empty; // default-constructed: no backing.
    auto res = enc.submit(empty);
    ASSERT_TRUE(res.isError());
    EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

TEST(MediaEncoderSubmit, BackendEncodeErrorPropagatesWithoutAdvancing) {
    gpu::FramePool pool(16ull * 1024 * 1024);
    MockEncodeState state;
    auto enc = MediaEncoder::create(baseSpec(), mockFactory({.failEncode = true}, &state)).value();

    auto frame = makeCpuFrame(pool, kRes, Duration::fromMilliseconds(0));
    auto res = enc.submit(frame);
    ASSERT_TRUE(res.isError());
    EXPECT_EQ(res.error().code(), ErrorCode::Io);
    EXPECT_EQ(enc.submittedFrameCount(), 0u);
}

// --- finish() ---------------------------------------------------------------

TEST(MediaEncoderFinish, FlushesBackendOnce) {
    MockEncodeState state;
    auto enc = MediaEncoder::create(baseSpec(), mockFactory({}, &state)).value();

    ASSERT_TRUE(enc.finish().isOk());
    EXPECT_TRUE(enc.isFinished());
    EXPECT_EQ(state.backend->finishCalls, 1);
}

TEST(MediaEncoderFinish, SecondFinishIsRejected) {
    MockEncodeState state;
    auto enc = MediaEncoder::create(baseSpec(), mockFactory({}, &state)).value();

    ASSERT_TRUE(enc.finish().isOk());
    auto again = enc.finish();
    ASSERT_TRUE(again.isError());
    EXPECT_EQ(again.error().code(), ErrorCode::FailedPrecondition);
    EXPECT_EQ(state.backend->finishCalls, 1);
}

TEST(MediaEncoderSubmit, RejectedAfterFinish) {
    gpu::FramePool pool(16ull * 1024 * 1024);
    MockEncodeState state;
    auto enc = MediaEncoder::create(baseSpec(), mockFactory({}, &state)).value();
    ASSERT_TRUE(enc.finish().isOk());

    auto frame = makeCpuFrame(pool, kRes, Duration::fromMilliseconds(0));
    auto res = enc.submit(frame);
    ASSERT_TRUE(res.isError());
    EXPECT_EQ(res.error().code(), ErrorCode::FailedPrecondition);
}

} // namespace
} // namespace palmier::media
