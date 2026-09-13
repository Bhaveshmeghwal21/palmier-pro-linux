// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shared fixtures and helpers for the export_coordinator_test cases, split out so that no single file
// exceeds this repository's 1000-line ceiling.
//
// The support namespace opens INSIDE the original enclosing namespace, because these
// helpers name the types they use unqualified. `inline` and named rather than an
// anonymous namespace: an anonymous namespace in a header gives every translation unit
// its own copy, and a helper only some partitions use would warn as unused.

#ifndef PALMIER_TESTS_SERVICES_EXPORT_COORDINATOR_TEST_SUPPORT_HPP_
#define PALMIER_TESTS_SERVICES_EXPORT_COORDINATOR_TEST_SUPPORT_HPP_

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
#include <unistd.h>  // getpid, for a per-process scratch directory name
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
namespace testsupport_exportcoordinatortest {

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
[[nodiscard]] inline const std::filesystem::path& scratchRoot() {
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
[[nodiscard]] inline std::filesystem::path scratchPath(const std::string& name) {
    static std::atomic<unsigned> counter{0};
    return scratchRoot() / (name + "_" + std::to_string(counter.fetch_add(1)) + ".mp4");
}

[[nodiscard]] inline std::string readAllBytes(const std::filesystem::path& path) {
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
    std::mutex mutex_{};
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
    int hookOnFrame{-1};
    std::function<void()> hook{};

    // Observations.
    std::atomic<int> encodeCalls{0};
    std::atomic<int> audioCalls{0};
    std::atomic<int> finishCalls{0};
    std::atomic<bool> created{false};
    std::atomic<bool> hadAudioSpec{false};
    gpu::CodecRoute route{};
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
    std::ofstream out_{};
};

inline media::EncodeBackendFactory mockFactory(BackendScript* script) {
    return [script](
               const media::EncodeSpec& spec,
               const gpu::CodecRoute& route) -> Result<std::unique_ptr<media::IEncodeBackend>> {
        {
            std::lock_guard<std::mutex> lock(script->mutex);
            script->route = route;
            script->spec = spec;
        }
        script->created.store(true);
        script->hadAudioSpec.store(spec.audio.has_value());
        return std::unique_ptr<media::IEncodeBackend>(std::make_unique<FileWritingBackend>(script));
    };
}

/// A factory whose backend refuses to initialize — a hardware initialization
/// failure before the first frame.
inline media::EncodeBackendFactory failingInitFactory(std::atomic<int>* attempts) {
    return [attempts](const media::EncodeSpec&,
                      const gpu::CodecRoute&) -> Result<std::unique_ptr<media::IEncodeBackend>> {
        if (attempts != nullptr) attempts->fetch_add(1);
        return err<std::unique_ptr<media::IEncodeBackend>>(
            makeError(ErrorCode::Internal, "mock encoder initialization failure"));
    };
}

// ---------------------------------------------------------------------------
// Capability helpers
// ---------------------------------------------------------------------------

inline gpu::GpuCaps nvidiaCaps() {
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
inline gpu::BridgeAvailability allVendorsCompiledIn() { return gpu::BridgeAvailability::all(); }

// ---------------------------------------------------------------------------
// The fixture
// ---------------------------------------------------------------------------

class ExportCoordinatorTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_unique<gpu::GpuContext>(gpu::GpuContext::softwareFallback());
        // Two video clips of 4 frames each at 30 fps, plus one audio track, so the
        // timeline is non-empty and audio-bearing.
        ASSERT_TRUE(
            session_
                .createProject("export-coordinator", FrameRate::fps30(), kRes, defaultColorSpace())
                .isOk());
        seedTimeline(4);
        revisionBeforeExport_ = session_.revision();
    }

    void TearDown() override { coordinator_.reset(); }

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
        coordinator_ =
            std::make_unique<ExportCoordinator>(session_, *context_, teardown_, options(script));
    }

    /// Run one export to completion, returning the number of delivered
    /// notifications. Fails the test (rather than hanging) if the worker does not
    /// finish inside the budget.
    [[nodiscard]] std::size_t runToCompletion() {
        const std::size_t delivered = coordinator_->awaitCompletion(kWaitBudget);
        EXPECT_GT(delivered, 0u) << "the export did not finish within the wait budget";
        return delivered;
    }

    ProjectSession session_{};
    media::DecoderTeardownQueue teardown_{};
    std::unique_ptr<gpu::GpuContext> context_{};
    std::unique_ptr<ExportCoordinator> coordinator_{};
    ManualClock clock_{};
    std::uint64_t revisionBeforeExport_{0};
};

}  // namespace testsupport_exportcoordinatortest
}  // namespace palmier::services

#endif  // PALMIER_TESTS_SERVICES_EXPORT_COORDINATOR_TEST_SUPPORT_HPP_
