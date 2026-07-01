// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/media_export_engine_test.cpp — unit tests for the Export Engine render
// loop + progress reporting (task 10.1; Requirements 11.1, 11.2, 10.3, 10.8) and
// export validation + failure cleanup (task 10.2; Requirements 11.3-11.6).
//
// These exercise ExportEngine::run driving a real software Compositor (fed
// synthetic frames through an injected ClipFrameProvider) and a MediaEncoder
// built behind the IEncodeBackend seam with a scriptable mock. No GPU, no
// FFmpeg, and no vendor SDK are required. They verify:
//   * the full timeline is rendered into a single encoder stream (Req 11.1);
//   * the source timeline is not modified (Req 11.1);
//   * progress is monotonic, spans 0..100, and ends at 100 (Req 11.2);
//   * frames are submitted in strictly increasing presentation time (P6 / Req
//     10.3) — also enforced by the encoder;
//   * a compatible device causes hardware encode to be preferred (Req 10.3/10.8);
//   * compositor / encoder errors propagate out of the loop;
//   * an unsupported output format/resolution is rejected before rendering (11.4);
//   * an empty timeline is rejected before rendering (11.5);
//   * a mid-export failure removes the incomplete output file and leaves the
//     source timeline unchanged (11.3);
//   * a successful export reports the output location (11.6).

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "core/Duration.hpp"
#include "core/Error.hpp"
#include "core/FrameRate.hpp"
#include "core/Project.hpp"
#include "core/Resolution.hpp"
#include "core/Result.hpp"
#include "core/Uuid.hpp"
#include "gpu/Compositor.hpp"
#include "gpu/GpuContext.hpp"
#include "gpu/GpuTypes.hpp"
#include "media/ExportEngine.hpp"
#include "media/MediaEncoder.hpp"

namespace palmier::media {
namespace {

// --- Scriptable encode backend (mirrors media_encoder_test.cpp) ------------

struct MockEncodeState {
    std::vector<gpu::CodecRoute> initRoutes{}; ///< routes the factory was invoked with.
    int                          encodeCalls{0};
    int                          finishCalls{0};
    std::vector<Duration>        presentations{};
    bool                         failFinish{false};
};

// Records into the (test-owned) MockEncodeState so observations survive the
// MediaEncoder's destruction inside ExportEngine::run().
class MockEncodeBackend final : public IEncodeBackend {
public:
    explicit MockEncodeBackend(MockEncodeState* state) : state_(state) {}

    [[nodiscard]] Result<void> encode(const EncoderInputFrame& frame) override {
        ++state_->encodeCalls;
        state_->presentations.push_back(frame.presentation);
        return ok();
    }

    [[nodiscard]] Result<void> finish() override {
        ++state_->finishCalls;
        if (state_->failFinish) return err(Error(ErrorCode::Io, "mock finish failure"));
        return ok();
    }

private:
    MockEncodeState* state_;
};

struct MockEncodeConfig {
    bool failFinish{false};
};

EncodeBackendFactory mockFactory(MockEncodeConfig cfg, MockEncodeState* state) {
    if (state != nullptr) state->failFinish = cfg.failFinish;
    return [state](const EncodeSpec&, const gpu::CodecRoute& route)
               -> Result<std::unique_ptr<IEncodeBackend>> {
        if (state != nullptr) state->initRoutes.push_back(route);
        return std::unique_ptr<IEncodeBackend>(std::make_unique<MockEncodeBackend>(state));
    };
}

// --- Project / caps helpers -------------------------------------------------

gpu::GpuCaps nvidiaCaps() {
    gpu::GpuCaps c;
    c.vendorId = gpu::GpuVendor::NVIDIA;
    c.vendor = "NVIDIA";
    c.supportsCompute = true;
    c.hwDecode = true;
    c.hwEncode = true;
    c.decodeCodecs = {gpu::CodecId::H264, gpu::CodecId::HEVC};
    c.encodeCodecs = {gpu::CodecId::H264, gpu::CodecId::HEVC};
    c.vramBytes = 4ull * 1024 * 1024 * 1024;
    return c;
}

constexpr Resolution kRes{4, 4};

// A video track with one clip covering [0, frameCount frames) at 30fps.
Project makeProject(int frameCount) {
    Clip c;
    c.id = Uuid::generateV4();
    c.timelineStart = Duration::zero();
    c.sourceIn = Duration::zero();
    c.sourceOut = FrameRate::fps30().durationForFrames(frameCount);
    c.opacity = 1.0;

    Track t;
    t.id = Uuid::generateV4();
    t.kind = TrackKind::Video;
    t.clips = {c};

    Project p;
    p.id = Uuid::generateV4();
    p.name = "export-test";
    p.timelineFps = FrameRate::fps30();
    p.canvas = kRes;
    p.tracks = {t};
    return p;
}

// A project with tracks but zero clips (an empty timeline / zero media segments).
Project emptyProject() {
    Project p;
    p.id = Uuid::generateV4();
    p.name = "empty-export-test";
    p.timelineFps = FrameRate::fps30();
    p.canvas = kRes;
    Track t;
    t.id = Uuid::generateV4();
    t.kind = TrackKind::Video;
    // No clips: zero media segments.
    p.tracks = {t};
    return p;
}

// A compositor over the software fallback context, wired to a solid-frame
// provider matching the export resolution.
std::unique_ptr<gpu::Compositor> makeCompositor(gpu::GpuContext& ctx) {
    auto comp = std::make_unique<gpu::Compositor>(ctx);
    comp->setFrameProvider([](const Clip&, Duration) -> Result<gpu::SourceFrame> {
        return gpu::SourceFrame::solid(kRes.width, kRes.height, gpu::RgbaColor{10, 20, 30, 255});
    });
    return comp;
}

ExportRequest baseRequest() {
    ExportRequest r;
    r.codec = gpu::CodecId::H264;
    r.resolution = kRes;
    r.frameRate = FrameRate::fps30();
    r.bitrateBitsPerSecond = 2'000'000;
    r.preferHardware = false;
    r.outputPath = "out.mp4";
    r.containerFormat = "mp4";
    r.progressInterval = std::chrono::milliseconds{0}; // emit every frame.
    return r;
}

// --- plannedFrameCount ------------------------------------------------------

TEST(ExportEnginePlan, CountsWholeFramesCoveringTheTimeline) {
    Project p = makeProject(5);
    EXPECT_EQ(ExportEngine::plannedFrameCount(p, FrameRate::fps30()), 5u);
}

TEST(ExportEnginePlan, CeilsPartialTrailingFrame) {
    // A clip a bit longer than 5 frames (at 30fps) must still render the partial
    // 6th frame: ceil(duration / frameStep).
    Project p = makeProject(5);
    p.tracks[0].clips[0].sourceOut =
        FrameRate::fps30().durationForFrames(5) + Duration::fromMilliseconds(10);
    EXPECT_EQ(ExportEngine::plannedFrameCount(p, FrameRate::fps30()), 6u);
}

TEST(ExportEnginePlan, ZeroForInvalidFrameRate) {
    Project p = makeProject(5);
    EXPECT_EQ(ExportEngine::plannedFrameCount(p, FrameRate{0, 0}), 0u);
}

// --- Render loop ------------------------------------------------------------

TEST(ExportEngineRun, RendersEveryFrameIntoASingleStream) {
    auto ctx = gpu::GpuContext::softwareFallback();
    auto comp = makeCompositor(ctx);
    ExportEngine engine(*comp);

    MockEncodeState state;
    Project p = makeProject(5);
    auto result = engine.run(p, baseRequest(), mockFactory({}, &state));

    ASSERT_TRUE(result.isOk());
    EXPECT_EQ(result.value().framesRendered, 5u);
    EXPECT_EQ(result.value().totalFrames, 5u);

    EXPECT_EQ(state.encodeCalls, 5);
    EXPECT_EQ(state.finishCalls, 1);
}

TEST(ExportEngineRun, SubmitsFramesInStrictlyIncreasingPresentationTime) {
    auto ctx = gpu::GpuContext::softwareFallback();
    auto comp = makeCompositor(ctx);
    ExportEngine engine(*comp);

    MockEncodeState state;
    Project p = makeProject(4);
    ASSERT_TRUE(engine.run(p, baseRequest(), mockFactory({}, &state)).isOk());

    ASSERT_EQ(state.presentations.size(), 4u);
    for (std::size_t i = 1; i < state.presentations.size(); ++i) {
        EXPECT_LT(state.presentations[i - 1], state.presentations[i]);
    }
    // First frame is at t=0, subsequent frames step by one frame duration.
    EXPECT_EQ(state.presentations[0], Duration::zero());
    EXPECT_EQ(state.presentations[1], FrameRate::fps30().frameDuration());
}

TEST(ExportEngineRun, DoesNotModifyTheSourceTimeline) {
    auto ctx = gpu::GpuContext::softwareFallback();
    auto comp = makeCompositor(ctx);
    ExportEngine engine(*comp);

    Project p = makeProject(3);
    const std::size_t trackCount = p.tracks.size();
    const std::size_t clipCount = p.tracks[0].clips.size();
    const Duration clipOut = p.tracks[0].clips[0].sourceOut;
    const Uuid clipId = p.tracks[0].clips[0].id;

    MockEncodeState state;
    ASSERT_TRUE(engine.run(p, baseRequest(), mockFactory({}, &state)).isOk());

    EXPECT_EQ(p.tracks.size(), trackCount);
    ASSERT_EQ(p.tracks[0].clips.size(), clipCount);
    EXPECT_EQ(p.tracks[0].clips[0].sourceOut, clipOut);
    EXPECT_EQ(p.tracks[0].clips[0].id, clipId);
}

// --- Progress ---------------------------------------------------------------

TEST(ExportEngineRun, ProgressIsMonotonicSpansZeroToHundred) {
    auto ctx = gpu::GpuContext::softwareFallback();
    auto comp = makeCompositor(ctx);
    ExportEngine engine(*comp);

    std::vector<int> percents;
    MockEncodeState state;
    Project p = makeProject(5);
    auto progress = [&](const ExportProgress& pr) { percents.push_back(pr.percent); };

    ASSERT_TRUE(engine.run(p, baseRequest(), mockFactory({}, &state), progress).isOk());

    ASSERT_FALSE(percents.empty());
    EXPECT_EQ(percents.front(), 0);   // starts at 0%
    EXPECT_EQ(percents.back(), 100);  // ends at 100%
    for (std::size_t i = 1; i < percents.size(); ++i) {
        EXPECT_GE(percents[i], percents[i - 1]); // monotonic non-decreasing
        EXPECT_GE(percents[i], 0);
        EXPECT_LE(percents[i], 100);
    }
}

TEST(ExportEngineRun, ProgressReportsFrameCounts) {
    auto ctx = gpu::GpuContext::softwareFallback();
    auto comp = makeCompositor(ctx);
    ExportEngine engine(*comp);

    std::size_t maxRendered = 0;
    std::size_t seenTotal = 0;
    MockEncodeState state;
    Project p = makeProject(5);
    auto progress = [&](const ExportProgress& pr) {
        maxRendered = std::max(maxRendered, pr.framesRendered);
        seenTotal = pr.totalFrames;
    };

    ASSERT_TRUE(engine.run(p, baseRequest(), mockFactory({}, &state), progress).isOk());
    EXPECT_EQ(seenTotal, 5u);
    EXPECT_EQ(maxRendered, 5u);
}

// --- Hardware preference ----------------------------------------------------

TEST(ExportEngineRun, PrefersHardwareEncodeWhenDeviceIsCompatible) {
    auto ctx = gpu::GpuContext::softwareFallback();
    auto comp = makeCompositor(ctx);
    ExportEngine engine(*comp);

    MockEncodeState state;
    Project p = makeProject(2);
    ExportRequest req = baseRequest();
    req.preferHardware = true;
    req.caps = nvidiaCaps();
    req.availability = gpu::BridgeAvailability::all();

    auto result = engine.run(p, req, mockFactory({}, &state));
    ASSERT_TRUE(result.isOk());
    EXPECT_TRUE(result.value().usedHardwareEncode);
    EXPECT_FALSE(result.value().usedSoftwareFallback);

    ASSERT_FALSE(state.initRoutes.empty());
    EXPECT_TRUE(state.initRoutes[0].hardware); // hardware route tried first.
}

// --- Validation & error propagation -----------------------------------------

TEST(ExportEngineRun, RejectsMissingOutputPath) {
    auto ctx = gpu::GpuContext::softwareFallback();
    auto comp = makeCompositor(ctx);
    ExportEngine engine(*comp);

    ExportRequest req = baseRequest();
    req.outputPath.clear();
    MockEncodeState state;
    auto result = engine.run(makeProject(2), req, mockFactory({}, &state));
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(ExportEngineRun, FallsBackToProjectFrameRateAndCanvas) {
    auto ctx = gpu::GpuContext::softwareFallback();
    auto comp = makeCompositor(ctx);
    ExportEngine engine(*comp);

    ExportRequest req = baseRequest();
    req.frameRate = FrameRate{0, 0};   // invalid -> use project timelineFps (30).
    req.resolution = Resolution{0, 0}; // invalid -> use project canvas (kRes).
    MockEncodeState state;
    auto result = engine.run(makeProject(5), req, mockFactory({}, &state));
    ASSERT_TRUE(result.isOk());
    EXPECT_EQ(result.value().totalFrames, 5u);
}

TEST(ExportEngineRun, PropagatesCompositorError) {
    auto ctx = gpu::GpuContext::softwareFallback();
    auto comp = std::make_unique<gpu::Compositor>(ctx);
    comp->setFrameProvider([](const Clip&, Duration) -> Result<gpu::SourceFrame> {
        return err<gpu::SourceFrame>(makeError(ErrorCode::Io, "decode failed"));
    });
    ExportEngine engine(*comp);

    MockEncodeState state;
    auto result = engine.run(makeProject(3), baseRequest(), mockFactory({}, &state));
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::Io);
}

TEST(ExportEngineRun, PropagatesEncoderFinishError) {
    auto ctx = gpu::GpuContext::softwareFallback();
    auto comp = makeCompositor(ctx);
    ExportEngine engine(*comp);

    MockEncodeState state;
    auto result = engine.run(makeProject(2), baseRequest(), mockFactory({.failFinish = true}, &state));
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::Io);
}

// --- Task 10.2: export validation (reject before rendering) -----------------

// Requirement 11.4: an unsupported output format (an unencodable codec) is
// rejected before rendering begins — the encoder is never built and no frame is
// composited or submitted.
TEST(ExportEngineValidation, RejectsUnsupportedCodecBeforeRendering) {
    auto ctx = gpu::GpuContext::softwareFallback();
    auto comp = makeCompositor(ctx);
    ExportEngine engine(*comp);

    ExportRequest req = baseRequest();
    req.codec = gpu::CodecId::MPEG2; // decode-only in the catalog: no encoder.
    MockEncodeState state;
    auto result = engine.run(makeProject(5), req, mockFactory({}, &state));

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::Unsupported);
    // Rejected before rendering: the encode backend was never created/invoked.
    EXPECT_TRUE(state.initRoutes.empty());
    EXPECT_EQ(state.encodeCalls, 0);
    EXPECT_EQ(state.finishCalls, 0);
}

// Requirement 11.4: an unknown container short-name is rejected before rendering.
TEST(ExportEngineValidation, RejectsUnsupportedContainerBeforeRendering) {
    auto ctx = gpu::GpuContext::softwareFallback();
    auto comp = makeCompositor(ctx);
    ExportEngine engine(*comp);

    ExportRequest req = baseRequest();
    req.containerFormat = "avi"; // not in the supported container set.
    MockEncodeState state;
    auto result = engine.run(makeProject(5), req, mockFactory({}, &state));

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::Unsupported);
    EXPECT_TRUE(state.initRoutes.empty());
    EXPECT_EQ(state.encodeCalls, 0);
}

// Requirement 11.4: an unsupported output resolution (odd dimensions the 4:2:0
// export codecs cannot represent) is rejected before rendering begins.
TEST(ExportEngineValidation, RejectsUnsupportedResolutionBeforeRendering) {
    auto ctx = gpu::GpuContext::softwareFallback();
    auto comp = makeCompositor(ctx);
    ExportEngine engine(*comp);

    ExportRequest req = baseRequest();
    req.resolution = Resolution{1281, 719}; // odd width and height.
    MockEncodeState state;
    auto result = engine.run(makeProject(5), req, mockFactory({}, &state));

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::Unsupported);
    EXPECT_TRUE(state.initRoutes.empty());
    EXPECT_EQ(state.encodeCalls, 0);
}

// Requirement 11.4: an output resolution beyond the maximum supported dimension
// is rejected before rendering begins.
TEST(ExportEngineValidation, RejectsOversizeResolutionBeforeRendering) {
    auto ctx = gpu::GpuContext::softwareFallback();
    auto comp = makeCompositor(ctx);
    ExportEngine engine(*comp);

    ExportRequest req = baseRequest();
    req.resolution = Resolution{10000, 10000}; // above the 8192 maximum.
    MockEncodeState state;
    auto result = engine.run(makeProject(5), req, mockFactory({}, &state));

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::Unsupported);
    EXPECT_TRUE(state.initRoutes.empty());
}

// Requirement 11.5: an export of an empty timeline (zero media segments) is
// rejected before rendering begins.
TEST(ExportEngineValidation, RejectsEmptyTimelineBeforeRendering) {
    auto ctx = gpu::GpuContext::softwareFallback();
    auto comp = makeCompositor(ctx);
    ExportEngine engine(*comp);

    MockEncodeState state;
    auto result = engine.run(emptyProject(), baseRequest(), mockFactory({}, &state));

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::FailedPrecondition);
    // Rejected before rendering: the encode backend was never created/invoked.
    EXPECT_TRUE(state.initRoutes.empty());
    EXPECT_EQ(state.encodeCalls, 0);
}

// Requirement 11.4 (accept path): the common supported codec/container/resolution
// combinations are accepted and export succeeds.
TEST(ExportEngineValidation, AcceptsSupportedFormatAndResolution) {
    auto ctx = gpu::GpuContext::softwareFallback();
    auto comp = makeCompositor(ctx);
    ExportEngine engine(*comp);

    ExportRequest req = baseRequest();
    req.codec = gpu::CodecId::HEVC;
    req.containerFormat = "MKV"; // case-insensitive match.
    MockEncodeState state;
    auto result = engine.run(makeProject(3), req, mockFactory({}, &state));

    ASSERT_TRUE(result.isOk());
    EXPECT_EQ(result.value().framesRendered, 3u);
}

// --- Task 10.2: mid-export failure cleanup + success notification -----------

// Requirement 11.3: when the export fails part-way through, the incomplete
// output file is removed. This uses a REAL file on disk to prove the deletion.
TEST(ExportEngineValidation, RemovesPartialOutputOnMidExportFailure) {
    auto ctx = gpu::GpuContext::softwareFallback();
    // A compositor whose provider fails triggers a mid-export failure.
    auto comp = std::make_unique<gpu::Compositor>(ctx);
    comp->setFrameProvider([](const Clip&, Duration) -> Result<gpu::SourceFrame> {
        return err<gpu::SourceFrame>(makeError(ErrorCode::Io, "decode failed"));
    });
    ExportEngine engine(*comp);

    // Stand in for a partial output written by the encoder: a real temp file.
    const std::filesystem::path out =
        std::filesystem::temp_directory_path() / "palmier_export_partial_output.mp4";
    {
        std::ofstream f(out, std::ios::binary | std::ios::trunc);
        f << "partial-incomplete-export-bytes";
    }
    ASSERT_TRUE(std::filesystem::exists(out));

    ExportRequest req = baseRequest();
    req.outputPath = out;
    MockEncodeState state;
    auto result = engine.run(makeProject(3), req, mockFactory({}, &state));

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::Io);
    // The incomplete output file was removed (Requirement 11.3).
    EXPECT_FALSE(std::filesystem::exists(out));

    // Defensive cleanup in case the assertion above did not hold.
    std::error_code ec;
    std::filesystem::remove(out, ec);
}

// Requirement 11.3: an encoder finish failure also removes the partial output.
TEST(ExportEngineValidation, RemovesPartialOutputOnFinishFailure) {
    auto ctx = gpu::GpuContext::softwareFallback();
    auto comp = makeCompositor(ctx);
    ExportEngine engine(*comp);

    const std::filesystem::path out =
        std::filesystem::temp_directory_path() / "palmier_export_finish_fail_output.mp4";
    {
        std::ofstream f(out, std::ios::binary | std::ios::trunc);
        f << "partial-bytes";
    }
    ASSERT_TRUE(std::filesystem::exists(out));

    ExportRequest req = baseRequest();
    req.outputPath = out;
    MockEncodeState state;
    auto result = engine.run(makeProject(2), req, mockFactory({.failFinish = true}, &state));

    ASSERT_TRUE(result.isError());
    EXPECT_FALSE(std::filesystem::exists(out));

    std::error_code ec;
    std::filesystem::remove(out, ec);
}

// Requirement 11.3: a mid-export failure leaves the source timeline unchanged.
TEST(ExportEngineValidation, PreservesSourceTimelineOnMidExportFailure) {
    auto ctx = gpu::GpuContext::softwareFallback();
    auto comp = std::make_unique<gpu::Compositor>(ctx);
    comp->setFrameProvider([](const Clip&, Duration) -> Result<gpu::SourceFrame> {
        return err<gpu::SourceFrame>(makeError(ErrorCode::Io, "decode failed"));
    });
    ExportEngine engine(*comp);

    Project p = makeProject(3);
    const std::size_t trackCount = p.tracks.size();
    const std::size_t clipCount = p.tracks[0].clips.size();
    const Duration clipOut = p.tracks[0].clips[0].sourceOut;
    const Uuid clipId = p.tracks[0].clips[0].id;

    MockEncodeState state;
    auto result = engine.run(p, baseRequest(), mockFactory({}, &state));

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(p.tracks.size(), trackCount);
    ASSERT_EQ(p.tracks[0].clips.size(), clipCount);
    EXPECT_EQ(p.tracks[0].clips[0].sourceOut, clipOut);
    EXPECT_EQ(p.tracks[0].clips[0].id, clipId);
}

// Requirement 11.6: a successful export reports the output location.
TEST(ExportEngineValidation, SuccessReportsOutputLocation) {
    auto ctx = gpu::GpuContext::softwareFallback();
    auto comp = makeCompositor(ctx);
    ExportEngine engine(*comp);

    ExportRequest req = baseRequest();
    req.outputPath = "the-final-render.mp4";
    MockEncodeState state;
    auto result = engine.run(makeProject(4), req, mockFactory({}, &state));

    ASSERT_TRUE(result.isOk());
    EXPECT_EQ(result.value().outputPath, req.outputPath);
    EXPECT_EQ(result.value().framesRendered, 4u);
}

} // namespace
} // namespace palmier::media
