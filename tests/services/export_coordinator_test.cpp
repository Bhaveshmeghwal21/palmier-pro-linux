// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/services/export_coordinator_test.cpp — unit tests for
// services::ExportCoordinator (task 9.4; Requirements 6.5, 6.10, 6.11, 7.1–7.7,
// 7.9, 7.10, 7.11, 8.11).
//
// The coordinator is the one component whose contract is mostly about what does
// NOT happen: no file after a rejection, no file after a cancellation, no project
// change ever, no disturbance of a running export by a rejected one. That shapes
// this file:
//
//   * Every write-performing case runs inside a per-process ABSOLUTE scratch
//     directory whose name contains getpid(), because gtest_discover_tests runs
//     one process per case and ctest runs those in parallel — two cases sharing an
//     output path would make "no file remains" mean nothing.
//   * "No file remains" is asserted against the real filesystem, and the
//     overwrite case compares the destination's BYTES before and after, which is
//     the only way "preserved byte-for-byte" (Requirement 7.11) can be checked.
//   * Cancellation and mid-export failure are made DETERMINISTIC by driving them
//     from inside the encode backend: the mock runs on the worker thread, so
//     calling coordinator.cancel() from its Nth encode() call sets the flag before
//     the render loop's next frame-boundary check. The export therefore always
//     stops at that exact frame — no sleep, no polling, no timing assumption.
//   * The ≤1 s progress cadence is driven by an INJECTED clock the test advances
//     by hand, so the interval is asserted arithmetically rather than waited out.
//   * Every wait is bounded (awaitCompletion(kWaitBudget)) and its result is
//     asserted, so a coordinator that fails to finish makes these tests FAIL
//     rather than hang.
//
// No GPU, no vendor SDK, no FFmpeg and no media fixture are involved: the
// compositor runs the software reference on gpu::GpuContext::softwareFallback(),
// the clip pixels come from an injected provider, and the encode backend is a
// mock that writes a real (small) file so the cleanup requirements are testable.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h> // getpid, for a per-process scratch directory name

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
#include "media/AudioEngine.hpp"
#include "media/AudioGraph.hpp"
#include "media/DecoderTeardownQueue.hpp"
#include "media/EncoderSelector.hpp"
#include "media/MediaEncoder.hpp"
#include "services/ExportCoordinator.hpp"
#include "services/Json.hpp"
#include "services/ProjectSession.hpp"
#include "services/ToolRegistry.hpp"
#include "services/ToolSchema.hpp"

namespace palmier::services {
namespace {

/// Bounded wait for every export in this file. Generous enough that a loaded host
/// never trips it, finite so a stuck coordinator fails the test instead of hanging
/// it.
constexpr std::chrono::milliseconds kWaitBudget{30'000};

constexpr Resolution kRes{128, 128};

// ---------------------------------------------------------------------------
// Per-process scratch directory
// ---------------------------------------------------------------------------

/// An absolute, per-process directory for the real files these tests write.
/// getpid() is in the name because gtest_discover_tests runs one process per test
/// case and ctest runs those in parallel.
[[nodiscard]] const std::filesystem::path& scratchRoot() {
    static const std::filesystem::path root = [] {
        std::filesystem::path dir =
            std::filesystem::temp_directory_path() /
            ("palmier_export_coordinator_" + std::to_string(static_cast<long long>(::getpid())));
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir;
    }();
    return root;
}

/// A unique absolute output path inside the scratch directory.
[[nodiscard]] std::filesystem::path scratchPath(const std::string& name) {
    static std::atomic<unsigned> counter{0};
    return scratchRoot() / (name + "_" + std::to_string(counter.fetch_add(1)) + ".mp4");
}

[[nodiscard]] std::string readAllBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// ---------------------------------------------------------------------------
// A manual steady clock
// ---------------------------------------------------------------------------

/// A steady clock the test advances by hand. Shared with the worker thread, so
/// reads and writes go through a mutex.
class ManualClock {
public:
    [[nodiscard]] media::SteadyClock fn() {
        return [this]() {
            std::lock_guard<std::mutex> lock(mutex_);
            return now_;
        };
    }

    void advance(std::chrono::milliseconds by) {
        std::lock_guard<std::mutex> lock(mutex_);
        now_ += by;
    }

private:
    std::mutex                            mutex_{};
    std::chrono::steady_clock::time_point now_{};
};

// ---------------------------------------------------------------------------
// A mock encode backend that writes a REAL file
// ---------------------------------------------------------------------------

/// What the mock backend should do, and what it observed. Shared between the test
/// thread and the export worker, so every field is touched under the mutex or is
/// atomic.
struct BackendScript {
    std::mutex mutex{};

    /// The path the backend writes to, so the cleanup requirements are checked
    /// against a file that really existed.
    std::filesystem::path outputPath{};

    /// Fail encode() on this 0-based video frame index (-1: never).
    int failVideoOnFrame{-1};
    /// Fail encodeAudio() on this 0-based block index (-1: never).
    int failAudioOnBlock{-1};
    /// Fail finish() (a mux-finalize failure).
    bool failFinish{false};
    /// Called from inside encode() for this 0-based frame index, on the WORKER
    /// thread — the hook that makes cancellation deterministic.
    int  hookOnFrame{-1};
    std::function<void()> hook{};

    // Observations.
    std::atomic<int>  encodeCalls{0};
    std::atomic<int>  audioCalls{0};
    std::atomic<int>  finishCalls{0};
    std::atomic<bool> created{false};
    std::atomic<bool> hadAudioSpec{false};
    gpu::CodecRoute   route{};
    media::EncodeSpec spec{};
};

class FileWritingBackend final : public media::IEncodeBackend {
public:
    explicit FileWritingBackend(BackendScript* script) : script_(script) {
        std::lock_guard<std::mutex> lock(script_->mutex);
        // Create the output file the moment the backend is built, exactly as the
        // FFmpeg backend does when it opens the muxer. This is what gives the
        // cleanup requirements something real to remove.
        out_.open(script_->outputPath, std::ios::binary | std::ios::trunc);
        out_ << "PALMIER-MOCK-HEADER";
        out_.flush();
    }

    [[nodiscard]] Result<void> encode(const media::EncoderInputFrame&) override {
        const int index = script_->encodeCalls.fetch_add(1);
        std::function<void()> hook;
        int failOn = -1;
        {
            std::lock_guard<std::mutex> lock(script_->mutex);
            failOn = script_->failVideoOnFrame;
            if (script_->hookOnFrame == index) hook = script_->hook;
        }
        // The hook runs BEFORE the frame is accepted, so a cancel() issued here is
        // visible to the render loop's next frame-boundary check.
        if (hook) hook();
        if (failOn == index) {
            return err(makeError(ErrorCode::Io, "mock video encode failure"));
        }
        out_ << "V";
        out_.flush();
        return ok();
    }

    [[nodiscard]] Result<void> encodeAudio(const media::EncoderInputAudio& audio) override {
        const int index = script_->audioCalls.fetch_add(1);
        int failOn = -1;
        {
            std::lock_guard<std::mutex> lock(script_->mutex);
            failOn = script_->failAudioOnBlock;
        }
        if (failOn == index) {
            return err(makeError(ErrorCode::Io, "mock audio encode failure"));
        }
        out_ << "A" << (audio.buffer != nullptr ? audio.buffer->frameCount() : 0u);
        out_.flush();
        return ok();
    }

    [[nodiscard]] Result<void> finish() override {
        script_->finishCalls.fetch_add(1);
        bool fail = false;
        {
            std::lock_guard<std::mutex> lock(script_->mutex);
            fail = script_->failFinish;
        }
        out_ << "PALMIER-MOCK-TRAILER";
        out_.flush();
        out_.close();
        if (fail) return err(makeError(ErrorCode::Io, "mock finish failure"));
        return ok();
    }

private:
    BackendScript* script_;
    std::ofstream  out_{};
};

media::EncodeBackendFactory mockFactory(BackendScript* script) {
    return [script](const media::EncodeSpec& spec, const gpu::CodecRoute& route)
               -> Result<std::unique_ptr<media::IEncodeBackend>> {
        {
            std::lock_guard<std::mutex> lock(script->mutex);
            script->route = route;
            script->spec = spec;
        }
        script->created.store(true);
        script->hadAudioSpec.store(spec.audio.has_value());
        return std::unique_ptr<media::IEncodeBackend>(
            std::make_unique<FileWritingBackend>(script));
    };
}

/// A factory whose backend refuses to initialize — a hardware initialization
/// failure before the first frame.
media::EncodeBackendFactory failingInitFactory(std::atomic<int>* attempts) {
    return [attempts](const media::EncodeSpec&, const gpu::CodecRoute&)
               -> Result<std::unique_ptr<media::IEncodeBackend>> {
        if (attempts != nullptr) attempts->fetch_add(1);
        return err<std::unique_ptr<media::IEncodeBackend>>(
            makeError(ErrorCode::Internal, "mock encoder initialization failure"));
    };
}

// ---------------------------------------------------------------------------
// Capability helpers
// ---------------------------------------------------------------------------

gpu::GpuCaps nvidiaCaps() {
    gpu::GpuCaps caps;
    caps.vendorId = gpu::GpuVendor::NVIDIA;
    caps.vendor = "NVIDIA";
    caps.supportsCompute = true;
    caps.hwDecode = true;
    caps.hwEncode = true;
    caps.decodeCodecs = {gpu::CodecId::H264, gpu::CodecId::HEVC};
    caps.encodeCodecs = {gpu::CodecId::H264, gpu::CodecId::HEVC};
    caps.vramBytes = 4ull * 1024 * 1024 * 1024;
    return caps;
}

/// Every vendor hardware path reported as compiled in. Supplied as a VALUE so the
/// hardware lane is reachable on a host whose build has no vendor SDK at all.
gpu::BridgeAvailability allVendorsCompiledIn() { return gpu::BridgeAvailability::all(); }

// ---------------------------------------------------------------------------
// The fixture
// ---------------------------------------------------------------------------

class ExportCoordinatorTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_unique<gpu::GpuContext>(gpu::GpuContext::softwareFallback());
        // Two video clips of 4 frames each at 30 fps, plus one audio track, so the
        // timeline is non-empty and audio-bearing.
        ASSERT_TRUE(session_.createProject("export-coordinator", FrameRate::fps30(), kRes,
                                           defaultColorSpace())
                        .isOk());
        seedTimeline(4);
        revisionBeforeExport_ = session_.revision();
    }

    void TearDown() override {
        coordinator_.reset();
    }

    /// Give the session a timeline with `frames` frames of video and one unmuted
    /// audio clip. Built as a Project value and installed through the engine's
    /// reset, which is how the session accepts a whole document.
    void seedTimeline(int frames) {
        Project project = session_.engine().snapshot();
        project.timelineFps = FrameRate::fps30();
        project.canvas = kRes;
        project.tracks.clear();

        Clip video;
        video.id = Uuid::generateV4();
        video.timelineStart = Duration::zero();
        video.sourceIn = Duration::zero();
        video.sourceOut = FrameRate::fps30().durationForFrames(frames);
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
        audio.sourceOut = FrameRate::fps30().durationForFrames(frames);
        audio.gain = 1.0;
        Track audioTrack;
        audioTrack.id = Uuid::generateV4();
        audioTrack.kind = TrackKind::Audio;
        audioTrack.clips = {audio};
        project.tracks.push_back(audioTrack);

        ASSERT_TRUE(session_.engine().reset(project).isOk());
    }

    /// Options wired for a hostless export: an injected clip-frame provider, the
    /// mock encode backend, an export-local software GPU context, and the manual
    /// clock.
    ExportCoordinator::Options options(BackendScript* script) {
        ExportCoordinator::Options opts;
        opts.clock = clock_.fn();
        opts.encodeFactory = mockFactory(script);
        opts.frameProvider = [](const Clip&, Duration) -> Result<gpu::SourceFrame> {
            return gpu::SourceFrame::solid(kRes.width, kRes.height,
                                           gpu::RgbaColor{31, 63, 95, 255});
        };
        opts.gpuContextFactory = []() -> Result<gpu::GpuContext> {
            return gpu::GpuContext::softwareFallback();
        };
        return opts;
    }

    ExportRequest2 request(const std::filesystem::path& output) {
        ExportRequest2 r;
        r.outputPath = output;
        r.container = "mp4";
        r.codec = gpu::CodecId::H264;
        r.resolution = kRes;
        r.frameRate = FrameRate::fps30();
        r.bitrateKbps = 4'000;
        r.includeAudio = true;
        r.preferHardware = false;
        r.overwrite = false;
        return r;
    }

    void makeCoordinator(BackendScript* script) {
        coordinator_ = std::make_unique<ExportCoordinator>(session_, *context_, teardown_,
                                                          options(script));
    }

    /// Run one export to completion, returning the number of delivered
    /// notifications. Fails the test (rather than hanging) if the worker does not
    /// finish inside the budget.
    [[nodiscard]] std::size_t runToCompletion() {
        const std::size_t delivered = coordinator_->awaitCompletion(kWaitBudget);
        EXPECT_GT(delivered, 0u) << "the export did not finish within the wait budget";
        return delivered;
    }

    ProjectSession                     session_{};
    media::DecoderTeardownQueue        teardown_{};
    std::unique_ptr<gpu::GpuContext>   context_{};
    std::unique_ptr<ExportCoordinator> coordinator_{};
    ManualClock                        clock_{};
    std::uint64_t                      revisionBeforeExport_{0};
};

// ===========================================================================
// validate() — rejection before anything is created (Requirements 7.9, 7.11)
// ===========================================================================

TEST(ExportCoordinatorValidate, AcceptsARequestInsideEveryRange) {
    ExportRequest2 r;
    r.outputPath = scratchPath("valid");
    r.container = "mp4";
    r.codec = gpu::CodecId::H264;
    r.resolution = Resolution{1920, 1080};
    r.frameRate = FrameRate::fps30();
    r.bitrateKbps = 8'000;
    EXPECT_TRUE(ExportCoordinator::validate(r).isOk());

    // The range endpoints themselves are accepted.
    r.resolution = Resolution{kMinExportWidth, kMinExportHeight};
    r.bitrateKbps = kMinExportBitrateKbps;
    r.frameRate = FrameRate{1, 1};
    EXPECT_TRUE(ExportCoordinator::validate(r).isOk());

    r.resolution = Resolution{kMaxExportWidth, kMaxExportHeight};
    r.bitrateKbps = kMaxExportBitrateKbps;
    r.frameRate = FrameRate{120, 1};
    EXPECT_TRUE(ExportCoordinator::validate(r).isOk());
}

TEST(ExportCoordinatorValidate, RejectsAnEmptyPath) {
    ExportRequest2 r;
    r.outputPath.clear();
    auto result = ExportCoordinator::validate(r);
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    EXPECT_NE(result.error().message().find("output path"), std::string::npos);
}

TEST(ExportCoordinatorValidate, RejectsAnOverlongPathNamingTheAcceptedRange) {
    ExportRequest2 r;
    r.outputPath = scratchRoot() / std::string(kMaxExportPathLength + 1, 'a');
    auto result = ExportCoordinator::validate(r);
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::OutOfRange);
    EXPECT_NE(result.error().message().find("4096"), std::string::npos);
}

TEST(ExportCoordinatorValidate, RejectsAMissingParentDirectory) {
    ExportRequest2 r;
    r.outputPath = scratchRoot() / "no_such_directory" / "out.mp4";
    auto result = ExportCoordinator::validate(r);
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::NotFound);
    // No file, and no directory, was created by the rejection.
    EXPECT_FALSE(std::filesystem::exists(r.outputPath.parent_path()));
    EXPECT_FALSE(std::filesystem::exists(r.outputPath));
}

TEST(ExportCoordinatorValidate, RejectsAnUnwritableParentDirectory) {
    const std::filesystem::path dir = scratchRoot() / "readonly_parent";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::filesystem::permissions(dir, std::filesystem::perms::owner_read |
                                          std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::replace, ec);
    if (::access(dir.c_str(), W_OK) == 0) {
        // Running as a user who bypasses the permission bits (root in a container):
        // the check itself cannot be exercised here.
        GTEST_SKIP() << "this user can write to a read-only directory; "
                        "the writability rejection is not observable";
    }

    ExportRequest2 r;
    r.outputPath = dir / "out.mp4";
    auto result = ExportCoordinator::validate(r);
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::PermissionDenied);
    EXPECT_FALSE(std::filesystem::exists(r.outputPath));

    std::filesystem::permissions(dir, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, ec);
}

TEST(ExportCoordinatorValidate, RejectsAnUnsupportedContainerNamingTheSupportedOnes) {
    ExportRequest2 r;
    r.outputPath = scratchPath("container");
    r.container = "avi";
    auto result = ExportCoordinator::validate(r);
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::Unsupported);
    EXPECT_NE(result.error().message().find("mp4"), std::string::npos);

    // The four supported containers are accepted, case-insensitively.
    for (const char* container : {"mp4", "MOV", "mkv", "WebM"}) {
        r.container = container;
        EXPECT_TRUE(ExportCoordinator::validate(r).isOk()) << container;
    }
}

TEST(ExportCoordinatorValidate, RejectsACodecTheSelectorDoesNotSupport) {
    ExportRequest2 r;
    r.outputPath = scratchPath("codec");
    for (gpu::CodecId codec : {gpu::CodecId::AV1, gpu::CodecId::MPEG2, gpu::CodecId::Unknown}) {
        r.codec = codec;
        auto result = ExportCoordinator::validate(r);
        ASSERT_TRUE(result.isError());
        EXPECT_EQ(result.error().code(), ErrorCode::Unsupported);
        EXPECT_NE(result.error().message().find("H.264"), std::string::npos);
    }
    for (gpu::CodecId codec : {gpu::CodecId::H264, gpu::CodecId::HEVC, gpu::CodecId::VP9}) {
        r.codec = codec;
        EXPECT_TRUE(ExportCoordinator::validate(r).isOk());
    }
}

TEST(ExportCoordinatorValidate, RejectsOutOfRangeGeometryCadenceAndBitRate) {
    const std::filesystem::path out = scratchPath("ranges");

    struct Case {
        const char*  what;
        Resolution   resolution;
        FrameRate    frameRate;
        std::int64_t bitrateKbps;
    };
    const Resolution ok{1920, 1080};
    const Case cases[] = {
        {"width below minimum", Resolution{127, 1080}, FrameRate::fps30(), 8'000},
        {"width above maximum", Resolution{3841, 1080}, FrameRate::fps30(), 8'000},
        {"height below minimum", Resolution{1920, 127}, FrameRate::fps30(), 8'000},
        {"height above maximum", Resolution{1920, 2161}, FrameRate::fps30(), 8'000},
        {"frame rate above maximum", ok, FrameRate{121, 1}, 8'000},
        {"bit rate below minimum", ok, FrameRate::fps30(), kMinExportBitrateKbps - 1},
        {"bit rate above maximum", ok, FrameRate::fps30(), kMaxExportBitrateKbps + 1},
    };

    for (const Case& c : cases) {
        ExportRequest2 r;
        r.outputPath = out;
        r.resolution = c.resolution;
        r.frameRate = c.frameRate;
        r.bitrateKbps = c.bitrateKbps;
        auto result = ExportCoordinator::validate(r);
        ASSERT_TRUE(result.isError()) << c.what;
        EXPECT_EQ(result.error().code(), ErrorCode::OutOfRange) << c.what;
        EXPECT_FALSE(result.error().message().empty()) << c.what;
        EXPECT_FALSE(std::filesystem::exists(out)) << c.what;
    }

    // A degenerate frame rate is a malformed argument rather than a range error.
    ExportRequest2 degenerate;
    degenerate.outputPath = out;
    degenerate.frameRate = FrameRate{0, 0};
    auto result = ExportCoordinator::validate(degenerate);
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(ExportCoordinatorValidate, RejectsAnExistingDestinationAndPreservesItByteForByte) {
    const std::filesystem::path out = scratchPath("existing");
    const std::string original = "the pre-existing deliverable, byte for byte";
    {
        std::ofstream file(out, std::ios::binary);
        file << original;
    }
    const auto sizeBefore = std::filesystem::file_size(out);
    const auto writeTimeBefore = std::filesystem::last_write_time(out);

    ExportRequest2 r;
    r.outputPath = out;
    r.overwrite = false;
    auto result = ExportCoordinator::validate(r);
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::AlreadyExists);
    EXPECT_NE(result.error().message().find("already exists"), std::string::npos);

    // Requirement 7.11: byte-for-byte, not merely "still there".
    EXPECT_EQ(readAllBytes(out), original);
    EXPECT_EQ(std::filesystem::file_size(out), sizeBefore);
    EXPECT_EQ(std::filesystem::last_write_time(out), writeTimeBefore);

    // With the acknowledgement the same request is admitted.
    r.overwrite = true;
    EXPECT_TRUE(ExportCoordinator::validate(r).isOk());
    EXPECT_EQ(readAllBytes(out), original); // validate() still wrote nothing.
}

TEST(ExportCoordinatorValidate, RejectsAnEmptyTimeline) {
    Project project;
    project.id = Uuid::generateV4();
    project.timelineFps = FrameRate::fps30();
    project.canvas = kRes;
    Track track;
    track.id = Uuid::generateV4();
    track.kind = TrackKind::Video;
    project.tracks = {track}; // a track, but zero media segments.

    auto result = ExportCoordinator::validateTimeline(project);
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::FailedPrecondition);
    EXPECT_NE(result.error().message().find("empty"), std::string::npos);
}

// ===========================================================================
// A successful export (Requirements 7.1, 7.2, 7.4, 6.5)
// ===========================================================================

TEST_F(ExportCoordinatorTest, RunsTheRequestedParametersAndReportsTheOutcome) {
    BackendScript script;
    const std::filesystem::path out = scratchPath("success");
    script.outputPath = out;
    makeCoordinator(&script);

    ExportRequest2 r = request(out);
    ASSERT_TRUE(coordinator_->begin(r).isOk());
    ASSERT_GT(runToCompletion(), 0u);

    ASSERT_TRUE(coordinator_->lastOutcome().has_value());
    const ExportOutcome& outcome = *coordinator_->lastOutcome();
    EXPECT_FALSE(coordinator_->lastError().has_value());

    // Requirement 7.2: the tool's return values.
    EXPECT_EQ(outcome.outputPath, out);
    EXPECT_EQ(outcome.framesEncoded, 4u);
    EXPECT_EQ(outcome.plannedFrames, 4u);
    EXPECT_FALSE(outcome.encoderName.empty());
    EXPECT_FALSE(outcome.usedHardwareEncode);
    EXPECT_FALSE(outcome.cancelled);

    // Requirement 7.4: the planner and the encode agree, and the reported duration
    // is the planned frame intervals.
    EXPECT_EQ(outcome.framesEncoded, outcome.plannedFrames);
    EXPECT_EQ(outcome.duration, FrameRate::fps30().durationForFrames(4));

    // Requirement 6.5: audio was requested, so exactly one audio stream was
    // configured and audio really was written.
    EXPECT_TRUE(script.hadAudioSpec.load());
    EXPECT_TRUE(outcome.containsAudio);
    EXPECT_EQ(script.audioCalls.load(), 4);
    EXPECT_GT(outcome.audioFrames, 0u);

    // The deliverable survives, and it is not empty.
    ASSERT_TRUE(std::filesystem::exists(out));
    EXPECT_GT(std::filesystem::file_size(out), 0u);
    EXPECT_EQ(script.finishCalls.load(), 1);

    // Requirement 7.1: the project is untouched, and the outcome says so.
    EXPECT_FALSE(outcome.projectModified);
    EXPECT_EQ(session_.revision(), revisionBeforeExport_);
    EXPECT_EQ(outcome.projectRevisionAtStart, revisionBeforeExport_);
    EXPECT_FALSE(coordinator_->running());
}

TEST_F(ExportCoordinatorTest, AVideoOnlyRequestConfiguresNoAudioStream) {
    BackendScript script;
    const std::filesystem::path out = scratchPath("video_only");
    script.outputPath = out;
    makeCoordinator(&script);

    ExportRequest2 r = request(out);
    r.includeAudio = false;
    ASSERT_TRUE(coordinator_->begin(r).isOk());
    ASSERT_GT(runToCompletion(), 0u);

    ASSERT_TRUE(coordinator_->lastOutcome().has_value());
    EXPECT_FALSE(coordinator_->lastOutcome()->containsAudio);
    EXPECT_EQ(coordinator_->lastOutcome()->audioFrames, 0u);
    EXPECT_FALSE(script.hadAudioSpec.load());
    EXPECT_EQ(script.audioCalls.load(), 0);
}

TEST_F(ExportCoordinatorTest, TheExportRunsOnASnapshotSoALaterEditCannotChangeIt) {
    // The snapshot is taken in begin(); the timeline is then lengthened. The
    // exported frame count must still be the snapshot's, which is what makes an
    // export reproducible while the user keeps working (Requirement 7.2).
    BackendScript script;
    const std::filesystem::path out = scratchPath("snapshot");
    script.outputPath = out;
    makeCoordinator(&script);

    ASSERT_TRUE(coordinator_->begin(request(out)).isOk());
    seedTimeline(40); // a much longer timeline, applied to the LIVE session.
    ASSERT_GT(runToCompletion(), 0u);

    ASSERT_TRUE(coordinator_->lastOutcome().has_value());
    EXPECT_EQ(coordinator_->lastOutcome()->framesEncoded, 4u);
    EXPECT_EQ(script.encodeCalls.load(), 4);
}

TEST_F(ExportCoordinatorTest, UsesTheEncoderSelectorForASoftwareSelection) {
    BackendScript script;
    const std::filesystem::path out = scratchPath("selection_sw");
    script.outputPath = out;
    makeCoordinator(&script);

    ExportRequest2 r = request(out);
    r.preferHardware = false; // software was asked for: not a fallback.
    ASSERT_TRUE(coordinator_->begin(r).isOk());
    ASSERT_GT(runToCompletion(), 0u);

    ASSERT_TRUE(coordinator_->lastSelection().has_value());
    const media::SelectionOutcome& selection = *coordinator_->lastSelection();
    ASSERT_TRUE(coordinator_->lastOutcome().has_value());
    const ExportOutcome& outcome = *coordinator_->lastOutcome();

    // The reported name is the selector's, not a second policy's.
    EXPECT_EQ(outcome.encoderName, selection.selection.encoderName());
    EXPECT_EQ(outcome.encoderName, "libx264");
    EXPECT_FALSE(outcome.usedHardwareEncode);
    // Software BY REQUEST is not a fallback (Requirement 8.8).
    EXPECT_FALSE(outcome.usedSoftwareFallback);
    EXPECT_TRUE(outcome.fallbackReason.empty());
    // The requested parameters survive selection unchanged (Requirement 8.3).
    EXPECT_EQ(selection.selection.parameters().resolution.width, kRes.width);
    EXPECT_EQ(selection.selection.parameters().resolution.height, kRes.height);
    EXPECT_EQ(selection.selection.parameters().bitrateBitsPerSecond, r.bitrateKbps * 1000);
    EXPECT_EQ(selection.probe, media::ProbeOutcome::NotRun); // no probe cost with no hardware.
}

TEST_F(ExportCoordinatorTest, UsesTheEncoderSelectorForAHardwareSelection) {
    // Capabilities and compiled-in vendor paths are supplied as VALUES, exactly as
    // media::EncoderSelector's own tests do, so the hardware lane is exercised on
    // this host, which has neither a GPU nor a vendor SDK. The probe is awaited
    // through an injected virtual-time awaiter, so nothing waits.
    BackendScript script;
    const std::filesystem::path out = scratchPath("selection_hw");
    script.outputPath = out;

    ExportCoordinator::Options opts = options(&script);
    opts.caps = nvidiaCaps();
    opts.availability = allVendorsCompiledIn();
    opts.selector.probe = [](gpu::CodecId, const gpu::GpuCaps&) { return true; };
    opts.selector.awaiter = [](const media::CapabilityProbe& probe, gpu::CodecId codec,
                               const gpu::GpuCaps& caps, std::chrono::milliseconds) {
        return probe(codec, caps) ? media::ProbeOutcome::Supported
                                  : media::ProbeOutcome::Unsupported;
    };
    coordinator_ =
        std::make_unique<ExportCoordinator>(session_, *context_, teardown_, std::move(opts));

    ExportRequest2 r = request(out);
    r.preferHardware = true;
    ASSERT_TRUE(coordinator_->begin(r).isOk());
    ASSERT_GT(runToCompletion(), 0u);

    ASSERT_TRUE(coordinator_->lastSelection().has_value());
    const media::SelectionOutcome& selection = *coordinator_->lastSelection();
    ASSERT_TRUE(selection.selection.isHardware());
    EXPECT_EQ(selection.selection.encoderName(), "h264_nvenc");
    EXPECT_EQ(selection.probe, media::ProbeOutcome::Supported);

    ASSERT_TRUE(coordinator_->lastOutcome().has_value());
    const ExportOutcome& outcome = *coordinator_->lastOutcome();
    // Requirement 8.8: never both, whichever way the encoder ended up routed.
    EXPECT_FALSE(outcome.usedHardwareEncode && outcome.usedSoftwareFallback);
    if (outcome.usedHardwareEncode) {
        EXPECT_EQ(outcome.encoderName, "h264_nvenc");
        EXPECT_TRUE(outcome.fallbackReason.empty());
    } else {
        // The encoder's own hardware-init retry ended on software: the outcome must
        // report the SOFTWARE encoder and the fallback, never the hardware name
        // (Requirements 8.3, 8.8).
        EXPECT_EQ(outcome.encoderName, "libx264");
        EXPECT_TRUE(outcome.usedSoftwareFallback);
        EXPECT_FALSE(outcome.fallbackReason.empty());
    }
    EXPECT_TRUE(std::filesystem::exists(out));
}

// ===========================================================================
// Progress: monotonic, bounded, and at most one second apart (Requirement 7.3)
// ===========================================================================

TEST_F(ExportCoordinatorTest, ProgressIsMonotonicBoundedAndDeliveredOnTheCallingThread) {
    BackendScript script;
    const std::filesystem::path out = scratchPath("progress");
    script.outputPath = out;

    // Every frame advances the injected clock past the interval, so every frame is
    // due — the cadence is exercised without a single sleep.
    ExportCoordinator::Options opts = options(&script);
    script.hookOnFrame = -1;
    coordinator_ = std::make_unique<ExportCoordinator>(session_, *context_, teardown_,
                                                       std::move(opts));

    std::vector<ExportProgressReport> seen;
    std::thread::id deliveringThread{};
    ASSERT_TRUE(coordinator_
                    ->begin(request(out),
                            [&seen, &deliveringThread](const ExportProgressReport& report) {
                                deliveringThread = std::this_thread::get_id();
                                seen.push_back(report);
                            })
                    .isOk());
    ASSERT_GT(runToCompletion(), 0u);

    ASSERT_FALSE(seen.empty());
    // Delivered on the thread that pumped — never from the worker (Requirement 7.3:
    // the shell keeps processing its own events).
    EXPECT_EQ(deliveringThread, std::this_thread::get_id());

    EXPECT_EQ(seen.front().percent, 0);
    EXPECT_EQ(seen.back().percent, 100);
    for (std::size_t i = 0; i < seen.size(); ++i) {
        EXPECT_GE(seen[i].percent, 0);
        EXPECT_LE(seen[i].percent, 100);
        if (i > 0) EXPECT_GE(seen[i].percent, seen[i - 1].percent);
    }
    EXPECT_EQ(seen.back().framesEncoded, 4u);
    EXPECT_EQ(seen.back().totalFrames, 4u);
}

TEST_F(ExportCoordinatorTest, TheProgressIntervalCeilingIsOneSecond) {
    BackendScript script;
    const std::filesystem::path out = scratchPath("interval");
    script.outputPath = out;

    ExportCoordinator::Options opts = options(&script);
    opts.progressInterval = std::chrono::milliseconds{5'000}; // asked for 5 s.
    ExportCoordinator coordinator(session_, *context_, teardown_, std::move(opts));

    // Requirement 7.3 is a ceiling, so a longer configured interval is clamped
    // down rather than honoured: the guarantee cannot be configured away.
    EXPECT_EQ(coordinator.progressInterval(), std::chrono::milliseconds{1'000});
}

TEST_F(ExportCoordinatorTest, AStoppedClockStillDeliversTheZeroAndHundredPercentReports) {
    // The clock never advances, so no intermediate report is ever due. The two
    // reports Requirement 7.3/7.4 mandate — the initial 0% and the final 100% —
    // are forced and therefore still arrive.
    BackendScript script;
    const std::filesystem::path out = scratchPath("stopped_clock");
    script.outputPath = out;
    makeCoordinator(&script);

    std::vector<int> percents;
    ASSERT_TRUE(coordinator_
                    ->begin(request(out),
                            [&percents](const ExportProgressReport& r) {
                                percents.push_back(r.percent);
                            })
                    .isOk());
    ASSERT_GT(runToCompletion(), 0u);

    ASSERT_GE(percents.size(), 2u);
    EXPECT_EQ(percents.front(), 0);
    EXPECT_EQ(percents.back(), 100);
}

// ===========================================================================
// Cancellation (Requirement 7.7)
// ===========================================================================

TEST_F(ExportCoordinatorTest, CancellationStopsAtAKnownFrameAndLeavesNoFile) {
    BackendScript script;
    const std::filesystem::path out = scratchPath("cancel");
    script.outputPath = out;
    makeCoordinator(&script);

    // Deterministic cancellation: the hook runs on the worker inside the second
    // frame's encode(), so the flag is set before the loop's third frame-boundary
    // check. The export always stops after exactly 2 encoded frames.
    script.hookOnFrame = 1;
    script.hook = [this]() { coordinator_->cancel(); };

    ASSERT_TRUE(coordinator_->begin(request(out)).isOk());
    ASSERT_GT(runToCompletion(), 0u);

    ASSERT_TRUE(coordinator_->lastOutcome().has_value());
    const ExportOutcome& outcome = *coordinator_->lastOutcome();
    EXPECT_TRUE(outcome.cancelled);
    EXPECT_EQ(outcome.framesEncoded, 2u);
    EXPECT_EQ(script.encodeCalls.load(), 2);

    // Requirement 7.7: no file remains at the requested path, and the project is
    // unchanged. The backend DID create the file — that is the point.
    EXPECT_TRUE(script.created.load());
    EXPECT_FALSE(std::filesystem::exists(out));
    EXPECT_FALSE(outcome.projectModified);
    EXPECT_EQ(session_.revision(), revisionBeforeExport_);
    EXPECT_TRUE(coordinator_->cancelRequested());
    EXPECT_FALSE(coordinator_->running());
}

TEST_F(ExportCoordinatorTest, CancellingBeforeTheFirstFrameEncodesNothingAndLeavesNoFile) {
    BackendScript script;
    const std::filesystem::path out = scratchPath("cancel_first");
    script.outputPath = out;
    makeCoordinator(&script);

    script.hookOnFrame = 0;
    script.hook = [this]() { coordinator_->cancel(); };

    ASSERT_TRUE(coordinator_->begin(request(out)).isOk());
    ASSERT_GT(runToCompletion(), 0u);

    ASSERT_TRUE(coordinator_->lastOutcome().has_value());
    EXPECT_TRUE(coordinator_->lastOutcome()->cancelled);
    EXPECT_EQ(coordinator_->lastOutcome()->framesEncoded, 1u);
    EXPECT_FALSE(std::filesystem::exists(out));
}

TEST_F(ExportCoordinatorTest, CancelWithNoRunningExportIsHarmless) {
    BackendScript script;
    script.outputPath = scratchPath("cancel_idle");
    makeCoordinator(&script);
    coordinator_->cancel(); // no worker exists.
    EXPECT_FALSE(coordinator_->running());
    EXPECT_FALSE(coordinator_->lastOutcome().has_value());
}

// ===========================================================================
// Failure after encoding begins (Requirements 6.10, 7.5, 8.11)
// ===========================================================================

TEST_F(ExportCoordinatorTest, AVideoFailureAfterEncodingBeginsLeavesNoFileAndNamesTheStage) {
    BackendScript script;
    const std::filesystem::path out = scratchPath("video_failure");
    script.outputPath = out;
    makeCoordinator(&script);
    script.failVideoOnFrame = 2; // the third frame fails.

    ASSERT_TRUE(coordinator_->begin(request(out)).isOk());
    ASSERT_GT(runToCompletion(), 0u);

    ASSERT_TRUE(coordinator_->lastError().has_value());
    EXPECT_EQ(coordinator_->lastError()->code(), ErrorCode::Io);
    EXPECT_NE(coordinator_->lastError()->message().find("video encoding failed"),
              std::string::npos);

    // Requirement 7.5: no file remains, and the project is unmodified.
    EXPECT_TRUE(script.created.load());
    EXPECT_FALSE(std::filesystem::exists(out));
    ASSERT_TRUE(coordinator_->lastOutcome().has_value());
    EXPECT_FALSE(coordinator_->lastOutcome()->projectModified);
    EXPECT_EQ(coordinator_->lastOutcome()->framesEncoded, 2u);
    EXPECT_EQ(session_.revision(), revisionBeforeExport_);
}

TEST_F(ExportCoordinatorTest, AnAudioFailureIdentifiesTheAudioStageAndLeavesNoFile) {
    BackendScript script;
    const std::filesystem::path out = scratchPath("audio_failure");
    script.outputPath = out;
    makeCoordinator(&script);
    script.failAudioOnBlock = 1; // the second interval's audio fails.

    ASSERT_TRUE(coordinator_->begin(request(out)).isOk());
    ASSERT_GT(runToCompletion(), 0u);

    // Requirement 6.10: the error identifies audio encoding or muxing as the
    // failing stage, not merely "the export".
    ASSERT_TRUE(coordinator_->lastError().has_value());
    EXPECT_NE(coordinator_->lastError()->message().find("audio encoding or muxing failed"),
              std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(out));
    ASSERT_TRUE(coordinator_->lastOutcome().has_value());
    EXPECT_FALSE(coordinator_->lastOutcome()->projectModified);
}

TEST_F(ExportCoordinatorTest, AFinalizeFailureIdentifiesTheFinishStageAndLeavesNoFile) {
    BackendScript script;
    const std::filesystem::path out = scratchPath("finish_failure");
    script.outputPath = out;
    makeCoordinator(&script);
    script.failFinish = true;

    ASSERT_TRUE(coordinator_->begin(request(out)).isOk());
    ASSERT_GT(runToCompletion(), 0u);

    ASSERT_TRUE(coordinator_->lastError().has_value());
    EXPECT_NE(coordinator_->lastError()->message().find("finalizing the output file failed"),
              std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(out));
}

TEST_F(ExportCoordinatorTest, AMidExportHardwareFailureIsReportedAsSuch) {
    // A hardware route whose encode fails AFTER a frame has been encoded is a hard
    // failure with its own name (Requirement 8.11) — distinct from an
    // initialization failure, which the selector turns into a software fallback.
    BackendScript script;
    const std::filesystem::path out = scratchPath("hw_mid_export");
    script.outputPath = out;

    ExportCoordinator::Options opts = options(&script);
    opts.caps = nvidiaCaps();
    opts.availability = allVendorsCompiledIn();
    opts.selector.probe = [](gpu::CodecId, const gpu::GpuCaps&) { return true; };
    opts.selector.awaiter = [](const media::CapabilityProbe&, gpu::CodecId,
                               const gpu::GpuCaps&, std::chrono::milliseconds) {
        return media::ProbeOutcome::Supported;
    };
    coordinator_ =
        std::make_unique<ExportCoordinator>(session_, *context_, teardown_, std::move(opts));

    script.failVideoOnFrame = 2; // two frames encoded, then the encoder fails.
    ExportRequest2 r = request(out);
    r.preferHardware = true;
    ASSERT_TRUE(coordinator_->begin(r).isOk());
    ASSERT_GT(runToCompletion(), 0u);

    ASSERT_TRUE(coordinator_->lastSelection().has_value());
    ASSERT_TRUE(coordinator_->lastSelection()->selection.isHardware());
    ASSERT_TRUE(coordinator_->lastError().has_value());
    // Requirement 8.11: a hardware failure AFTER at least one frame is reported as
    // a mid-export hardware encode failure — a different fact from an
    // initialization failure, which the selector turns into a software fallback.
    EXPECT_NE(coordinator_->lastError()->message().find("mid-export hardware encode failure"),
              std::string::npos);
    ASSERT_TRUE(coordinator_->lastOutcome().has_value());
    EXPECT_EQ(coordinator_->lastOutcome()->framesEncoded, 2u);
    // The incomplete output is gone and the project is unmodified.
    EXPECT_TRUE(script.created.load());
    EXPECT_FALSE(std::filesystem::exists(out));
    EXPECT_FALSE(coordinator_->lastOutcome()->projectModified);
    EXPECT_EQ(session_.revision(), revisionBeforeExport_);
}

TEST_F(ExportCoordinatorTest, AFirstFrameHardwareFailureIsNotCalledMidExport) {
    // The distinction Requirement 8.11 draws is "after at least one frame". A
    // failure on the very first frame is the plain video-stage failure.
    BackendScript script;
    const std::filesystem::path out = scratchPath("hw_first_frame");
    script.outputPath = out;

    ExportCoordinator::Options opts = options(&script);
    opts.caps = nvidiaCaps();
    opts.availability = allVendorsCompiledIn();
    opts.selector.probe = [](gpu::CodecId, const gpu::GpuCaps&) { return true; };
    opts.selector.awaiter = [](const media::CapabilityProbe&, gpu::CodecId,
                               const gpu::GpuCaps&, std::chrono::milliseconds) {
        return media::ProbeOutcome::Supported;
    };
    coordinator_ =
        std::make_unique<ExportCoordinator>(session_, *context_, teardown_, std::move(opts));

    script.failVideoOnFrame = 0;
    ExportRequest2 r = request(out);
    r.preferHardware = true;
    ASSERT_TRUE(coordinator_->begin(r).isOk());
    ASSERT_GT(runToCompletion(), 0u);

    ASSERT_TRUE(coordinator_->lastError().has_value());
    const std::string message = coordinator_->lastError()->message();
    EXPECT_EQ(message.find("mid-export hardware encode failure"), std::string::npos);
    EXPECT_NE(message.find("video encoding failed"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(out));
}

TEST_F(ExportCoordinatorTest, AnEncoderThatNeverInitializesCreatesNoFile) {
    BackendScript script;
    const std::filesystem::path out = scratchPath("init_failure");
    script.outputPath = out;

    std::atomic<int> attempts{0};
    ExportCoordinator::Options opts = options(&script);
    opts.encodeFactory = failingInitFactory(&attempts);
    coordinator_ = std::make_unique<ExportCoordinator>(session_, *context_, teardown_,
                                                       std::move(opts));

    ASSERT_TRUE(coordinator_->begin(request(out)).isOk());
    ASSERT_GT(runToCompletion(), 0u);

    ASSERT_TRUE(coordinator_->lastError().has_value());
    EXPECT_GE(attempts.load(), 1);
    EXPECT_FALSE(std::filesystem::exists(out));
    ASSERT_TRUE(coordinator_->lastOutcome().has_value());
    EXPECT_EQ(coordinator_->lastOutcome()->framesEncoded, 0u);
    EXPECT_FALSE(coordinator_->lastOutcome()->projectModified);
}

// ===========================================================================
// Concurrency and admission at begin() (Requirements 7.6, 7.9, 7.10, 7.11)
// ===========================================================================

TEST_F(ExportCoordinatorTest, ASecondConcurrentRequestIsRejectedWithoutDisturbingTheFirst) {
    BackendScript script;
    const std::filesystem::path out = scratchPath("concurrent_first");
    const std::filesystem::path second = scratchPath("concurrent_second");
    script.outputPath = out;
    makeCoordinator(&script);

    // Hold the worker inside the first frame until the test has issued the second
    // request, so the two really do overlap. The gate is released by this thread,
    // so there is no timing assumption: the worker cannot proceed until it is.
    std::promise<void> release;
    std::shared_future<void> released(release.get_future());
    std::promise<void> reached;
    std::shared_future<void> hasReached(reached.get_future());
    script.hookOnFrame = 0;
    script.hook = [&reached, released]() mutable {
        reached.set_value();
        released.wait();
    };

    std::vector<ExportProgressReport> firstProgress;
    ASSERT_TRUE(coordinator_
                    ->begin(request(out),
                            [&firstProgress](const ExportProgressReport& r) {
                                firstProgress.push_back(r);
                            })
                    .isOk());

    // The worker is now parked inside the first frame.
    ASSERT_EQ(hasReached.wait_for(kWaitBudget), std::future_status::ready);
    EXPECT_TRUE(coordinator_->running());

    // Requirement 7.10: the second request is refused and says why.
    auto rejected = coordinator_->begin(request(second));
    ASSERT_TRUE(rejected.isError());
    EXPECT_EQ(rejected.error().code(), ErrorCode::FailedPrecondition);
    EXPECT_NE(rejected.error().message().find("already in progress"), std::string::npos);
    // ...and it created nothing of its own.
    EXPECT_FALSE(std::filesystem::exists(second));

    // The running export is untouched: still running, not cancelled.
    EXPECT_TRUE(coordinator_->running());
    EXPECT_FALSE(coordinator_->cancelRequested());

    release.set_value();
    ASSERT_GT(runToCompletion(), 0u);

    // The first export completed normally, with its own progress and its own file.
    ASSERT_TRUE(coordinator_->lastOutcome().has_value());
    EXPECT_FALSE(coordinator_->lastOutcome()->cancelled);
    EXPECT_EQ(coordinator_->lastOutcome()->framesEncoded, 4u);
    EXPECT_TRUE(std::filesystem::exists(out));
    ASSERT_FALSE(firstProgress.empty());
    EXPECT_EQ(firstProgress.back().percent, 100);
    EXPECT_FALSE(std::filesystem::exists(second));
}

TEST_F(ExportCoordinatorTest, BeginRejectsAnInvalidRequestWithoutStartingAWorker) {
    BackendScript script;
    const std::filesystem::path out = scratchPath("begin_invalid");
    script.outputPath = out;
    makeCoordinator(&script);

    ExportRequest2 r = request(out);
    r.bitrateKbps = 1; // below the accepted range.
    auto result = coordinator_->begin(r);
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::OutOfRange);

    EXPECT_FALSE(coordinator_->running());
    EXPECT_FALSE(script.created.load());
    EXPECT_FALSE(std::filesystem::exists(out));
}

TEST_F(ExportCoordinatorTest, BeginRejectsAnEmptyTimelineWithoutStartingAWorker) {
    BackendScript script;
    const std::filesystem::path out = scratchPath("begin_empty");
    script.outputPath = out;
    makeCoordinator(&script);

    // Empty the timeline: tracks remain, clips do not.
    Project project = session_.engine().snapshot();
    for (Track& track : project.tracks) track.clips.clear();
    ASSERT_TRUE(session_.engine().reset(project).isOk());

    auto result = coordinator_->begin(request(out));
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::FailedPrecondition);
    EXPECT_FALSE(coordinator_->running());
    EXPECT_FALSE(script.created.load());
    EXPECT_FALSE(std::filesystem::exists(out));
}

TEST_F(ExportCoordinatorTest, BeginRejectsAnExistingDestinationAndPreservesIt) {
    BackendScript script;
    const std::filesystem::path out = scratchPath("begin_existing");
    script.outputPath = out;
    makeCoordinator(&script);

    const std::string original = "an earlier export nobody asked to replace";
    {
        std::ofstream file(out, std::ios::binary);
        file << original;
    }

    auto result = coordinator_->begin(request(out));
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::AlreadyExists);
    EXPECT_FALSE(coordinator_->running());
    EXPECT_FALSE(script.created.load());
    EXPECT_EQ(readAllBytes(out), original);
}

TEST_F(ExportCoordinatorTest, TwoExportsCanRunOneAfterTheOther) {
    BackendScript first;
    const std::filesystem::path outA = scratchPath("sequential_a");
    first.outputPath = outA;
    makeCoordinator(&first);

    ASSERT_TRUE(coordinator_->begin(request(outA)).isOk());
    ASSERT_GT(runToCompletion(), 0u);
    ASSERT_TRUE(coordinator_->lastOutcome().has_value());
    EXPECT_EQ(coordinator_->lastOutcome()->framesEncoded, 4u);
    EXPECT_TRUE(std::filesystem::exists(outA));

    // The second export reuses the same coordinator: running() is false again and
    // the per-export state (progress, outcome) starts clean.
    const std::filesystem::path outB = scratchPath("sequential_b");
    first.outputPath = outB;
    first.encodeCalls.store(0);
    first.audioCalls.store(0);
    first.finishCalls.store(0);
    EXPECT_FALSE(coordinator_->running());

    ASSERT_TRUE(coordinator_->begin(request(outB)).isOk());
    ASSERT_GT(runToCompletion(), 0u);
    ASSERT_TRUE(coordinator_->lastOutcome().has_value());
    EXPECT_EQ(coordinator_->lastOutcome()->outputPath, outB);
    EXPECT_EQ(coordinator_->lastOutcome()->framesEncoded, 4u);
    EXPECT_TRUE(std::filesystem::exists(outB));
    EXPECT_EQ(coordinator_->deliveredProgress().front().percent, 0);
}

TEST_F(ExportCoordinatorTest, TheNotifierFiresSoAnOwnerKnowsWhenToPump) {
    BackendScript script;
    const std::filesystem::path out = scratchPath("notifier");
    script.outputPath = out;
    makeCoordinator(&script);

    std::atomic<int> notifications{0};
    coordinator_->setNotifier([&notifications]() { notifications.fetch_add(1); });

    ASSERT_TRUE(coordinator_->begin(request(out)).isOk());
    ASSERT_GT(runToCompletion(), 0u);
    EXPECT_GT(notifications.load(), 0);
}

TEST_F(ExportCoordinatorTest, ADestroyedCoordinatorCancelsAndLeavesNoPartialFile) {
    BackendScript script;
    const std::filesystem::path out = scratchPath("destroyed");
    script.outputPath = out;
    makeCoordinator(&script);

    // Park the worker in the first frame, then destroy the coordinator: its
    // destructor cancels and joins, and the guard removes the partial output.
    std::promise<void> reached;
    std::shared_future<void> hasReached(reached.get_future());
    script.hookOnFrame = 0;
    script.hook = [&reached]() mutable { reached.set_value(); };

    ASSERT_TRUE(coordinator_->begin(request(out)).isOk());
    ASSERT_EQ(hasReached.wait_for(kWaitBudget), std::future_status::ready);

    coordinator_.reset(); // cancels, joins — must not hang.
    EXPECT_FALSE(std::filesystem::exists(out));
    EXPECT_TRUE(script.created.load());
}

// ===========================================================================
// The `timeline.export` tool-surface adapter (task 9.7; Requirements 3.1, 7.2)
//
// Requirement 7.2 asks the tool to "perform the same export as criterion 1" and
// to return the output path, the encoded frame count, the selected encoder name
// and whether hardware encode was used. These cases check both halves:
//
//   * "the same export" — the tool enters through the same `begin()`, so the same
//     request yields outcome fields equal to the dialog's, and every admission
//     rejection reaches the caller unchanged with no file created.
//   * "and returns" — every field named above appears in the result, taken from
//     the ExportOutcome rather than recomputed.
//
// The encode backend is still the mock that writes real bytes: this host's
// libavcodec carries no H.264, HEVC or VP9 encoder, so the seam is the only way an
// export runs here at all.
// ===========================================================================

/// The three codec spellings the schema publishes must be exactly the three the
/// translation parses, and the numeric ranges it publishes must be the
/// coordinator's own constants — the declarations are in two files (the tool
/// surface depends only on Palmier::core) and this is what stops them drifting.
TEST(ExportToolSchema, MatchesTheCoordinatorRanges) {
    ProjectSession     session;
    const ToolRegistry registry = buildDefaultToolRegistry(session);
    const Tool*        tool = registry.find("timeline.export");
    ASSERT_NE(tool, nullptr);

    const ArgSpec* width = tool->schema.find("width");
    ASSERT_NE(width, nullptr);
    EXPECT_EQ(width->minInt, static_cast<std::int64_t>(kMinExportWidth));
    EXPECT_EQ(width->maxInt, static_cast<std::int64_t>(kMaxExportWidth));

    const ArgSpec* height = tool->schema.find("height");
    ASSERT_NE(height, nullptr);
    EXPECT_EQ(height->minInt, static_cast<std::int64_t>(kMinExportHeight));
    EXPECT_EQ(height->maxInt, static_cast<std::int64_t>(kMaxExportHeight));

    const ArgSpec* fps = tool->schema.find("fps");
    ASSERT_NE(fps, nullptr);
    EXPECT_EQ(fps->minNum, kMinExportFps);
    EXPECT_EQ(fps->maxNum, kMaxExportFps);

    const ArgSpec* bitrate = tool->schema.find("bitrateKbps");
    ASSERT_NE(bitrate, nullptr);
    EXPECT_EQ(bitrate->minInt, kMinExportBitrateKbps);
    EXPECT_EQ(bitrate->maxInt, kMaxExportBitrateKbps);

    // Every published codec spelling translates to a codec the coordinator
    // supports, and the set is exactly the supported three.
    const ArgSpec* codec = tool->schema.find("codec");
    ASSERT_NE(codec, nullptr);
    ASSERT_EQ(codec->enumValues.size(), 3u);
    Project probe;
    for (const std::string& value : codec->enumValues) {
        Json args = Json::object();
        args.set("outputPath", std::string("/tmp/palmier-codec-probe.mp4"));
        args.set("format", std::string("mp4"));
        args.set("codec", value);
        const Result<ExportRequest2> translated = exportRequestFromToolArguments(args, probe);
        ASSERT_TRUE(translated.isOk()) << value;
        EXPECT_TRUE(isSupportedExportCodec(translated.value().codec)) << value;
    }
}

TEST_F(ExportCoordinatorTest, ExportToolReportsEveryFieldRequirement72Names) {
    BackendScript               script;
    const std::filesystem::path out = scratchPath("tool_success");
    script.outputPath = out;
    makeCoordinator(&script);

    const Tool::Handler handler = makeExportToolHandler(*coordinator_, session_);
    Json                args = Json::object();
    args.set("outputPath", out.string());
    args.set("format", std::string("mp4"));
    args.set("width", static_cast<std::int64_t>(kRes.width));
    args.set("height", static_cast<std::int64_t>(kRes.height));
    args.set("codec", std::string("h264"));
    args.set("fps", 30.0);
    args.set("bitrateKbps", static_cast<std::int64_t>(4'000));
    args.set("preferHardware", false);

    const Result<Json> result = handler(args);
    ASSERT_TRUE(result.isOk()) << result.error().toString();
    const Json& out_json = result.value();

    EXPECT_EQ(out_json.stringOr("outputPath"), out.string());
    // The seeded timeline is four frames at 30 fps, and the count the tool reports
    // is the count the planner predicted.
    EXPECT_EQ(out_json.intOr("framesEncoded"), 4);
    EXPECT_EQ(out_json.intOr("plannedFrames"), 4);
    EXPECT_EQ(out_json.intOr("framesEncoded"), out_json.intOr("plannedFrames"));
    // One encoder name, and it is the software encoder this host selects.
    EXPECT_EQ(out_json.stringOr("encoderName"), "libx264");
    EXPECT_FALSE(out_json.boolOr("usedHardwareEncode", true));
    // preferHardware was false, so software is what was asked for — not a fallback.
    EXPECT_FALSE(out_json.boolOr("usedSoftwareFallback", true));
    EXPECT_TRUE(out_json.boolOr("containsAudio"));
    EXPECT_EQ(out_json.intOr("durationNs"),
              static_cast<std::int64_t>(FrameRate::fps30().durationForFrames(4).nanoseconds()));
    // Requirement 7.2's "leaves the project state unchanged", reported so it can be
    // asserted rather than assumed — and checked here against the session too.
    EXPECT_FALSE(out_json.boolOr("projectModified", true));
    EXPECT_FALSE(session_.modified());
    EXPECT_EQ(session_.revision(), revisionBeforeExport_);

    // The file the export produced is really there, with the bytes the backend wrote.
    ASSERT_TRUE(std::filesystem::exists(out));
    EXPECT_FALSE(readAllBytes(out).empty());
}

TEST_F(ExportCoordinatorTest, ExportToolDefaultsGeometryAndCadenceFromTheProject) {
    BackendScript               script;
    const std::filesystem::path out = scratchPath("tool_defaults");
    script.outputPath = out;
    makeCoordinator(&script);

    // Only the two arguments that were required before task 9.7 — exactly what a
    // pre-9.7 caller (and the offline interpreter's "export as mp4 to <path>"
    // phrase) sends.
    const Tool::Handler handler = makeExportToolHandler(*coordinator_, session_);
    Json                args = Json::object();
    args.set("outputPath", out.string());
    args.set("format", std::string("mp4"));

    const Result<Json> result = handler(args);
    ASSERT_TRUE(result.isOk()) << result.error().toString();

    // The omitted geometry and cadence came from the project, not from a constant.
    const Project project = session_.engine().snapshot();
    std::lock_guard<std::mutex> lock(script.mutex);
    EXPECT_EQ(script.spec.resolution.width, project.canvas.width);
    EXPECT_EQ(script.spec.resolution.height, project.canvas.height);
    EXPECT_EQ(script.spec.frameRate.numerator(), project.timelineFps.numerator());
    EXPECT_EQ(script.spec.frameRate.denominator(), project.timelineFps.denominator());
    // The container came from `format`, and the default codec for mp4 is H.264.
    EXPECT_EQ(script.spec.containerFormat, "mp4");
    EXPECT_EQ(script.spec.codec, gpu::CodecId::H264);
    // `includeAudio` defaults to true, so the encoder was given an audio stream.
    EXPECT_TRUE(script.spec.audio.has_value());
}

TEST_F(ExportCoordinatorTest, ExportToolMatchesTheDialogForTheSameRequest) {
    BackendScript               script;
    const std::filesystem::path viaDialog = scratchPath("tool_vs_dialog_a");
    script.outputPath = viaDialog;
    makeCoordinator(&script);

    // (1) The dialog path: begin() directly.
    ASSERT_TRUE(coordinator_->begin(request(viaDialog)).isOk());
    ASSERT_GT(runToCompletion(), 0u);
    ASSERT_TRUE(coordinator_->lastOutcome().has_value());
    const ExportOutcome dialogOutcome = *coordinator_->lastOutcome();

    // (2) The tool path: the same parameters, expressed as tool arguments.
    const std::filesystem::path viaTool = scratchPath("tool_vs_dialog_b");
    {
        std::lock_guard<std::mutex> lock(script.mutex);
        script.outputPath = viaTool;
    }
    const Tool::Handler handler = makeExportToolHandler(*coordinator_, session_);
    Json                args = Json::object();
    args.set("outputPath", viaTool.string());
    args.set("format", std::string("mp4"));
    args.set("width", static_cast<std::int64_t>(kRes.width));
    args.set("height", static_cast<std::int64_t>(kRes.height));
    args.set("codec", std::string("h264"));
    args.set("fps", 30.0);
    args.set("bitrateKbps", static_cast<std::int64_t>(4'000));
    args.set("includeAudio", true);
    args.set("preferHardware", false);

    const Result<Json> toolResult = handler(args);
    ASSERT_TRUE(toolResult.isOk()) << toolResult.error().toString();

    // Every outcome field but the path is equal: the two callers really did run the
    // same export through the same coordinator.
    const Json expected = exportOutcomeToJson(dialogOutcome);
    for (const char* field : {"framesEncoded", "plannedFrames", "encoderName",
                              "usedHardwareEncode", "usedSoftwareFallback", "containsAudio",
                              "audioFrames", "durationNs", "projectModified"}) {
        ASSERT_EQ(expected.contains(field), toolResult.value().contains(field)) << field;
        if (expected.contains(field)) {
            EXPECT_EQ(expected.find(field)->dump(), toolResult.value().find(field)->dump())
                << field;
        }
    }
    EXPECT_NE(expected.stringOr("outputPath"), toolResult.value().stringOr("outputPath"));
}

TEST_F(ExportCoordinatorTest, ExportToolForwardsAdmissionRejectionsAndCreatesNoFile) {
    BackendScript               script;
    const std::filesystem::path out = scratchPath("tool_rejected");
    script.outputPath = out;
    makeCoordinator(&script);
    const Tool::Handler handler = makeExportToolHandler(*coordinator_, session_);

    // A missing destination is refused by the translation, before the coordinator.
    Json noPath = Json::object();
    noPath.set("format", std::string("mp4"));
    const Result<Json> missing = handler(noPath);
    ASSERT_TRUE(missing.isError());
    EXPECT_EQ(missing.error().code(), ErrorCode::InvalidArgument);

    // An unsupported container is refused by ExportCoordinator::validate, whose
    // message names the supported set (Requirement 7.9) — the translation does not
    // duplicate that rule.
    Json badContainer = Json::object();
    badContainer.set("outputPath", out.string());
    badContainer.set("format", std::string("avi"));
    const Result<Json> unsupportedContainer = handler(badContainer);
    ASSERT_TRUE(unsupportedContainer.isError());
    EXPECT_EQ(unsupportedContainer.error().code(), ErrorCode::Unsupported);
    EXPECT_NE(unsupportedContainer.error().message().find("mp4"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(out));

    // A parent directory that does not exist (Requirement 7.9).
    const std::filesystem::path absent = scratchRoot() / "no_such_dir" / "out.mp4";
    Json missingParent = Json::object();
    missingParent.set("outputPath", absent.string());
    missingParent.set("format", std::string("mp4"));
    const Result<Json> notFound = handler(missingParent);
    ASSERT_TRUE(notFound.isError());
    EXPECT_EQ(notFound.error().code(), ErrorCode::NotFound);
    EXPECT_FALSE(std::filesystem::exists(absent));

    // Requirement 7.11: an existing destination without acknowledgement is refused
    // and preserved byte-for-byte; the same request WITH `overwrite` is accepted.
    const std::filesystem::path existing = scratchPath("tool_existing");
    {
        std::ofstream seed(existing, std::ios::binary | std::ios::trunc);
        seed << "ORIGINAL-CONTENT";
    }
    const std::string before = readAllBytes(existing);
    Json              exists = Json::object();
    exists.set("outputPath", existing.string());
    exists.set("format", std::string("mp4"));
    const Result<Json> alreadyExists = handler(exists);
    ASSERT_TRUE(alreadyExists.isError());
    EXPECT_EQ(alreadyExists.error().code(), ErrorCode::AlreadyExists);
    EXPECT_EQ(readAllBytes(existing), before);

    {
        std::lock_guard<std::mutex> lock(script.mutex);
        script.outputPath = existing;
    }
    exists.set("overwrite", true);
    exists.set("preferHardware", false);
    const Result<Json> acknowledged = handler(exists);
    ASSERT_TRUE(acknowledged.isOk()) << acknowledged.error().toString();
    EXPECT_NE(readAllBytes(existing), before);

    // Requirement 7.2 again: no rejection and no acceptance touched the project.
    EXPECT_FALSE(session_.modified());
    EXPECT_EQ(session_.revision(), revisionBeforeExport_);
}

TEST_F(ExportCoordinatorTest, ExportToolRunsThroughTheSharedToolRegistry) {
    BackendScript               script;
    const std::filesystem::path out = scratchPath("tool_registry");
    script.outputPath = out;
    makeCoordinator(&script);

    // The composition root wires exactly this hook, so this is the path the MCP
    // endpoint, the GUI and the in-app agent all take (Requirement 3.1).
    ToolRegistryHooks hooks;
    hooks.exportTimeline = makeExportToolHandler(*coordinator_, session_);
    const ToolRegistry registry = buildDefaultToolRegistry(session_, std::move(hooks));

    const Tool* tool = registry.find("timeline.export");
    ASSERT_NE(tool, nullptr);

    Json args = Json::object();
    args.set("outputPath", out.string());
    args.set("format", std::string("mp4"));
    args.set("preferHardware", false);
    // The arguments validate against the very schema `tools/list` publishes.
    ASSERT_TRUE(tool->schema.validate(args).isOk());

    const Result<Json> result = registry.invoke("timeline.export", args);
    ASSERT_TRUE(result.isOk()) << result.error().toString();
    EXPECT_EQ(result.value().stringOr("outputPath"), out.string());
    EXPECT_EQ(result.value().intOr("framesEncoded"), 4);
    EXPECT_TRUE(std::filesystem::exists(out));
}

// ---------------------------------------------------------------------------
// Captions sidecar export (usable-editor task 13; Requirement 10.3's second
// export mode, alongside burn-in). A caption cue on the project's timeline
// also needs a text rasterizer installed on the export-local Compositor
// (Requirement 9.5/10.3's shared seam), which the shared fixture's own
// options() does not set for every OTHER test — set directly here instead of
// changing that shared default.
// ---------------------------------------------------------------------------

TEST_F(ExportCoordinatorTest, WritesAnSrtSidecarNextToTheVideoWhenTheProjectHasACaptionCue) {
    // Add a caption track/cue to the already-seeded video+audio timeline.
    Project project = session_.engine().snapshot();
    Clip cue;
    cue.id = Uuid::generateV4();
    cue.timelineStart = Duration::zero();
    cue.sourceIn = Duration::zero();
    cue.sourceOut = FrameRate::fps30().durationForFrames(4);
    cue.captionText = "Hello, captions!";
    Track captionTrack;
    captionTrack.id = Uuid::generateV4();
    captionTrack.kind = TrackKind::Caption;
    captionTrack.clips = {cue};
    project.tracks.push_back(captionTrack);
    ASSERT_TRUE(session_.engine().reset(project).isOk());

    BackendScript script;
    const std::filesystem::path out = scratchPath("captions_sidecar");
    script.outputPath = out;

    ExportCoordinator::Options opts = options(&script);
    // Requirement 9.5/10.3: the export-local Compositor needs a rasterizer to
    // burn the caption cue in at all; a fixed solid frame stands in for a real
    // glyph render, exactly like the video frameProvider above stands in for a
    // real decode.
    opts.textRasterizer = [](const TextStyle&, std::uint32_t w,
                            std::uint32_t h) -> Result<gpu::SourceFrame> {
        return gpu::SourceFrame::solid(w, h, gpu::RgbaColor{255, 255, 255, 255});
    };
    coordinator_ = std::make_unique<ExportCoordinator>(session_, *context_, teardown_, opts);

    ExportRequest2 r = request(out);
    ASSERT_TRUE(coordinator_->begin(r).isOk());
    ASSERT_GT(runToCompletion(), 0u);

    ASSERT_TRUE(coordinator_->lastOutcome().has_value());
    const ExportOutcome& outcome = *coordinator_->lastOutcome();
    EXPECT_FALSE(coordinator_->lastError().has_value());

    // The video output itself succeeded, independent of the sidecar.
    ASSERT_TRUE(std::filesystem::exists(out));

    // The sidecar sits next to it with the same base name and a .srt extension.
    ASSERT_FALSE(outcome.captionsSidecarPath.empty());
    std::filesystem::path expectedSidecar = out;
    expectedSidecar.replace_extension(".srt");
    EXPECT_EQ(outcome.captionsSidecarPath, expectedSidecar);
    ASSERT_TRUE(std::filesystem::exists(outcome.captionsSidecarPath));

    std::ifstream sidecar(outcome.captionsSidecarPath, std::ios::binary);
    const std::string contents((std::istreambuf_iterator<char>(sidecar)),
                               std::istreambuf_iterator<char>());
    EXPECT_NE(contents.find("Hello, captions!"), std::string::npos);
    EXPECT_NE(contents.find("00:00:00,000 -->"), std::string::npos);
}

TEST_F(ExportCoordinatorTest, ReportsNoSidecarPathWhenTheProjectHasNoCaptions) {
    // The fixture's own seeded timeline (video + audio, no captions).
    BackendScript script;
    const std::filesystem::path out = scratchPath("no_captions");
    script.outputPath = out;
    makeCoordinator(&script);

    ExportRequest2 r = request(out);
    ASSERT_TRUE(coordinator_->begin(r).isOk());
    ASSERT_GT(runToCompletion(), 0u);

    ASSERT_TRUE(coordinator_->lastOutcome().has_value());
    const ExportOutcome& outcome = *coordinator_->lastOutcome();
    EXPECT_TRUE(outcome.captionsSidecarPath.empty());

    std::filesystem::path wouldBeSidecar = out;
    wouldBeSidecar.replace_extension(".srt");
    EXPECT_FALSE(std::filesystem::exists(wouldBeSidecar));
}

} // namespace
} // namespace palmier::services
