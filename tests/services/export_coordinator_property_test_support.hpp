// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/services/export_coordinator_property_test.cpp — Properties 33, 34, 35,
// 36, 37 and 39 for services::ExportCoordinator (task 9.5 of the
// end-to-end-editor-integration spec; Requirements 6.10, 7.1, 7.2, 7.3, 7.4,
// 7.5, 7.6, 7.7, 7.9, 7.11, 8.11).
//
//   * Property 33 — export runs exactly the requested parameters and never
//                   touches the project.
//   * Property 34 — progress is monotonic, bounded and timely.
//   * Property 35 — a successful export matches the planner.
//   * Property 36 — any failure after encoding begins leaves no file and no
//                   project change.
//   * Property 37 — cancellation leaves no file and no project change.
//   * Property 39 — invalid export requests are rejected before any file exists.
//
// Property 38 (two successive exports are identical) belongs to the export
// engine's own ordering suite and lives in
// tests/media_export_ordering_property_test.cpp (task 9.6).
//
// ## These properties are about ABSENCE, so vacuity is the real risk
//
// Four of the six assert that something is NOT there: no file after a failure, no
// file after a cancellation, no file after a rejection, no change to the project.
// A mock that never wrote anything would satisfy all of them while proving
// nothing. Every anti-vacuity measure here exists for that reason:
//
//   * **The mock encode backend writes REAL BYTES to a REAL PATH.** Its
//     constructor opens `script.outputPath` — an absolute path inside the case's
//     own scratch directory — writes a header, flushes, and then, from the worker
//     thread and against the actual filesystem, records `existedAfterOpen` and a
//     byte count. Every property that asserts "no file remains" ALSO asserts
//     `existedAfterOpen` and `bytesWritten > 0`, so each case proves a file
//     existed at exactly that path and was then removed. If the backend ever
//     stopped creating the file, these properties would fail rather than pass
//     vacuously. Two of them go further and assert `exists(outputPath)` from the
//     property body while the worker is parked mid-export, i.e. before cleanup.
//   * **Property 35 counts the frames in the FILE, not in the outcome struct.**
//     The backend appends one marker byte per encoded frame; the property re-reads
//     the finished file from disk, counts markers and compares that to
//     `ExportEngine::plannedFrameCount`. The coordinator's own `framesEncoded` is
//     then compared against the same independent number, so a miscounting
//     coordinator cannot agree with itself into a pass.
//   * **Property 39 compares the pre-existing destination's BYTES** before and
//     after the rejected request, together with its size — the only way
//     Requirement 7.11's "preserved byte-for-byte" can be checked — and it first
//     asserts that the UNPERTURBED request is accepted, so the rejection is
//     attributable to the single generated perturbation rather than to a request
//     that was invalid anyway.
//   * **Property 33 compares a SERIALIZED project snapshot** (`serializeProject`,
//     the same text the `.palmier` store writes) before and after the export, not
//     merely a revision counter; and one generated case in two applies a real edit
//     to the live session WHILE the worker is parked mid-encode, proving both that
//     the export ignores the edit (it exports the snapshot's frame count, not the
//     edited timeline's) and that the export itself contributes no change.
//
// ## Nothing here sleeps, and nothing here can hang
//
//   * The ≤1 s progress cadence of Requirement 7.3 is driven by an INJECTED steady
//     clock which the mock backend advances by a GENERATED per-frame amount from
//     inside `encode()`. Property 34 predicts the exact set of reports the cadence
//     rule must produce for that virtual-time schedule and compares it with what
//     was delivered, so the rule is checked arithmetically. No test waits out an
//     interval and no outcome depends on how long anything really took.
//   * Cancellation and the mid-export edit are made DETERMINISTIC from inside the
//     backend, which runs on the worker thread: a hook called at a chosen frame
//     index sets the cancel flag before the render loop's next frame-boundary
//     check, so the export always stops at that exact frame.
//   * Every wait is bounded (`waitForCompletion` / `awaitCompletion` with
//     `kWaitBudget`) and its result is asserted, so a coordinator that fails to
//     finish makes a property FAIL rather than hang. A parked worker is released by
//     a scope guard, so a failing assertion cannot leave the export blocked on a
//     gate nobody will open. Requirement 7.7's "within 2 seconds" is asserted as
//     measured elapsed time between the instant the worker set the cancel flag and
//     the instant the outcome became observable — measured, never slept.
//
// ## Parallel-safe by construction
//
// `gtest_discover_tests` runs one process per case and ctest runs those in
// parallel, and these properties write real files. Every write therefore goes to
// an ABSOLUTE path inside a per-case directory under a per-process root whose name
// contains `getpid()`, so no two cases can share an output path and "no file
// remains at the requested path" is a statement about this case alone.
//
// ## What is NOT checked here, and why
//
// Property 35's design text asks for a "probeable, decodable" file. The FFmpeg
// build on this host carries no `libx264`, `libx265` or `libvpx-vp9` encoder, so no
// real H.264/HEVC/VP9 bytes can be produced here at all — which is exactly what
// the encode-backend seam exists for. Real decodable output is task 9.8's
// hardware-versus-software comparison. What this property checks instead is
// stronger than the coordinator's self-report: the frame count is recovered from
// the bytes of the finished file on disk.
//
// Property 33's design text also asks that a request issued through the export
// dialog and through `timeline.export` produce equal outcome fields. Both callers
// enter through `ExportCoordinator::begin()` and the tool-surface wiring is task
// 9.7 (still open), so this property asserts the fact that makes the two agree:
// the outcome is a function of the request and the snapshot alone — re-issuing the
// identical request to a second path reproduces every outcome field but the path.

#include <algorithm>
#include <atomic>
#include <cctype>
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
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>  // getpid(), access()

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include "core/ColorSpace.hpp"
#include "core/Duration.hpp"
#include "core/Error.hpp"
#include "core/FrameRate.hpp"
#include "core/Project.hpp"
#include "core/Resolution.hpp"
#include "core/Result.hpp"
#include "core/TimelineEngine.hpp"
#include "core/Track.hpp"
#include "core/Uuid.hpp"
#include "gpu/Compositor.hpp"
#include "gpu/GpuContext.hpp"
#include "gpu/GpuTypes.hpp"
#include "media/DecoderTeardownQueue.hpp"
#include "media/EncoderSelector.hpp"
#include "media/ExportEngine.hpp"
#include "media/MediaEncoder.hpp"
#include "services/ExportCoordinator.hpp"
#include "services/ProjectSession.hpp"
#include "services/ProjectStore.hpp"

// Shared fixtures and helpers for the export_coordinator_property_test cases, split out so that no single file
// exceeds this repository's 1000-line ceiling.
//
// The support namespace opens INSIDE the original enclosing namespace, because these
// helpers name the types they use unqualified. `inline` and named rather than an
// anonymous namespace: an anonymous namespace in a header gives every translation unit
// its own copy, and a helper only some partitions use would warn as unused.

#ifndef PALMIER_TESTS_SERVICES_EXPORT_COORDINATOR_PROPERTY_TEST_SUPPORT_HPP_
#define PALMIER_TESTS_SERVICES_EXPORT_COORDINATOR_PROPERTY_TEST_SUPPORT_HPP_

namespace palmier::services {
namespace testsupport_exportcoordinatorpropertytest {

namespace {

using namespace std::chrono_literals;

/// Bounded wait for every export in this file: generous enough that a loaded host
/// never trips it, finite so a stuck coordinator fails a property instead of
/// hanging it.
constexpr std::chrono::milliseconds kWaitBudget{30'000};

/// Requirement 7.7's ceiling: a cancelled export stops within 2 seconds of the
/// cancellation. Asserted as measured elapsed time, never slept.
constexpr std::chrono::milliseconds kCancelStopBudget{2'000};

constexpr std::string_view kHeaderMarker{"PALMIER-MOCK-HEADER\n"};
constexpr std::string_view kTrailerMarker{"PALMIER-MOCK-TRAILER\n"};
constexpr char kFrameMarker{'F'};

// ---------------------------------------------------------------------------
// Per-process root and per-case scratch directories
// ---------------------------------------------------------------------------

[[nodiscard]] inline const std::filesystem::path& scratchRoot() {
    static const std::filesystem::path root = [] {
        std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("palmier_export_coordinator_prop_" +
                                     std::to_string(static_cast<long long>(::getpid())));
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir;
    }();
    return root;
}

/// An absolute directory owned by ONE generated case. Every file a case writes
/// lives here, so neither two cases nor two parallel ctest processes can collide
/// on an output path.
class CaseDir {
public:
    explicit CaseDir(std::string_view label) {
        static std::atomic<unsigned long long> counter{0};
        path_ = scratchRoot() /
                (std::string(label) + "_" + std::to_string(static_cast<long long>(::getpid())) +
                 "_" + std::to_string(counter.fetch_add(1)));
        std::error_code ec;
        std::filesystem::create_directories(path_, ec);
    }

    ~CaseDir() {
        std::error_code ec;
        std::filesystem::permissions(path_, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::add, ec);
        std::filesystem::remove_all(path_, ec);
    }

    CaseDir(const CaseDir&) = delete;
    CaseDir& operator=(const CaseDir&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] std::filesystem::path file(const std::string& name) const { return path_ / name; }

private:
    std::filesystem::path path_{};
};

[[nodiscard]] inline std::string readAllBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// Non-throwing existence check: some generated paths are deliberately malformed
/// (over 4096 characters), for which the throwing overload would raise rather than
/// answer.
[[nodiscard]] inline bool pathExists(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec) && !ec;
}

[[nodiscard]] inline std::string toLowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

/// Can this process observe a non-writable directory at all? Running as root —
/// which this sandbox and many CI containers do — bypasses the permission bits, so
/// the "parent directory is not writable" rejection of Requirement 7.9 is simply
/// unreachable. Probed once; the perturbation is dropped from Property 39's
/// generator where it is unobservable rather than asserted vacuously.
[[nodiscard]] inline bool unwritableDirectoriesAreObservable() {
    static const bool observable = [] {
        const std::filesystem::path dir = scratchRoot() / "writability_probe";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::filesystem::permissions(
            dir, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
            std::filesystem::perm_options::replace, ec);
        const bool bypassed = ::access(dir.c_str(), W_OK) == 0;
        std::filesystem::permissions(dir, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace, ec);
        std::filesystem::remove_all(dir, ec);
        return !bypassed;
    }();
    return observable;
}

// ---------------------------------------------------------------------------
// A manual steady clock — the injected progress clock (Requirement 7.3)
// ---------------------------------------------------------------------------

/// Advanced by hand from the export worker (inside the mock backend), so the
/// ≤1 s cadence is exercised in virtual time and nothing sleeps.
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

/// Opens a gate on scope exit. A property that parks the export worker declares
/// one of these AFTER the coordinator, so a failing assertion releases the worker
/// before the coordinator is joined: a bug then fails the property instead of
/// hanging it.
class ReleaseOnExit {
public:
    explicit ReleaseOnExit(std::promise<void>& gate) : gate_(gate) {}
    ~ReleaseOnExit() { release(); }

    ReleaseOnExit(const ReleaseOnExit&) = delete;
    ReleaseOnExit& operator=(const ReleaseOnExit&) = delete;

    void release() {
        if (released_) return;
        released_ = true;
        try {
            gate_.set_value();
        } catch (const std::future_error&) {
            // Already satisfied; nothing to do.
        }
    }

private:
    std::promise<void>& gate_;
    bool released_{false};
};

// ---------------------------------------------------------------------------
// The scripted mock encode backend — writes REAL bytes to a REAL path
// ---------------------------------------------------------------------------

/// What the backend should do and what it observed. Shared between the property
/// body and the export worker, so every field is atomic or guarded by `mutex`.
struct ExportScript {
    std::mutex mutex{};
    std::filesystem::path outputPath{};

    /// Fail `encode()` on this 0-based video frame index (-1: never).
    int failVideoOnFrame{-1};
    /// Fail `encodeAudio()` on this 0-based block index (-1: never).
    int failAudioOnBlock{-1};
    /// Fail `finish()` — a mux-finalize failure.
    bool failFinish{false};

    /// The injected clock and the amount each encoded frame advances it: how the
    /// progress cadence is driven without sleeping.
    ManualClock* clock{nullptr};
    std::chrono::milliseconds advancePerFrame{0};

    /// Called on the WORKER thread from inside `encode()`, with the frame index —
    /// the hook that makes cancellation and mid-export edits deterministic.
    std::function<void(int)> encodeHook{};
    /// Called on the WORKER thread once the backend exists and its output file has
    /// been created, before the render loop starts.
    std::function<void()> afterCreateHook{};

    // --- Observations ------------------------------------------------------
    std::atomic<int> factoryCalls{0};
    std::atomic<int> encodeCalls{0};
    std::atomic<int> audioCalls{0};
    std::atomic<int> finishCalls{0};
    std::atomic<bool> created{false};
    /// Anti-vacuity: whether the output path really exists on disk, as observed by
    /// the worker immediately after the file was opened and flushed.
    std::atomic<bool> existedAfterOpen{false};
    std::atomic<std::uint64_t> bytesWritten{0};

    media::EncodeSpec spec{};
    gpu::CodecRoute route{};
};

class MarkerWritingBackend final : public media::IEncodeBackend {
public:
    explicit MarkerWritingBackend(ExportScript* script) : script_(script) {
        std::filesystem::path path;
        {
            std::lock_guard<std::mutex> lock(script_->mutex);
            path = script_->outputPath;
        }
        // Create the output file the moment the backend is built, exactly as the
        // FFmpeg backend does when it opens the muxer: this is what gives the
        // cleanup requirements something real to remove.
        out_.open(path, std::ios::binary | std::ios::trunc);
        out_ << kHeaderMarker;
        out_.flush();
        script_->created.store(true);
        script_->bytesWritten.fetch_add(kHeaderMarker.size());
        std::error_code ec;
        script_->existedAfterOpen.store(std::filesystem::exists(path, ec) && !ec);
    }

    [[nodiscard]] Result<void> encode(const media::EncoderInputFrame&) override {
        const int index = script_->encodeCalls.fetch_add(1);

        std::function<void(int)> hook;
        int failOn{-1};
        ManualClock* clock{nullptr};
        std::chrono::milliseconds advance{0};
        {
            std::lock_guard<std::mutex> lock(script_->mutex);
            hook = script_->encodeHook;
            failOn = script_->failVideoOnFrame;
            clock = script_->clock;
            advance = script_->advancePerFrame;
        }

        // "This frame took `advance` milliseconds", in virtual time: the injected
        // clock moves before the engine reports this frame's progress.
        if (clock != nullptr && advance > 0ms) clock->advance(advance);

        // The hook runs BEFORE the frame is accepted, so a cancel() issued here is
        // visible to the render loop's next frame-boundary check.
        if (hook) hook(index);

        if (failOn == index) {
            return err(makeError(ErrorCode::Io, "mock video encode failure"));
        }
        out_ << kFrameMarker;
        out_.flush();
        script_->bytesWritten.fetch_add(1);
        return ok();
    }

    [[nodiscard]] Result<void> encodeAudio(const media::EncoderInputAudio& audio) override {
        const int index = script_->audioCalls.fetch_add(1);
        int failOn{-1};
        {
            std::lock_guard<std::mutex> lock(script_->mutex);
            failOn = script_->failAudioOnBlock;
        }
        if (failOn == index) {
            return err(makeError(ErrorCode::Io, "mock audio encode failure"));
        }
        const std::string block =
            "audio:" + std::to_string(audio.buffer != nullptr ? audio.buffer->frameCount() : 0u) +
            "\n";
        out_ << block;
        out_.flush();
        script_->bytesWritten.fetch_add(block.size());
        return ok();
    }

    [[nodiscard]] Result<void> finish() override {
        script_->finishCalls.fetch_add(1);
        bool fail = false;
        {
            std::lock_guard<std::mutex> lock(script_->mutex);
            fail = script_->failFinish;
        }
        out_ << kTrailerMarker;
        out_.flush();
        out_.close();
        script_->bytesWritten.fetch_add(kTrailerMarker.size());
        if (fail) return err(makeError(ErrorCode::Io, "mock finish failure"));
        return ok();
    }

private:
    ExportScript* script_;
    std::ofstream out_{};
};

[[nodiscard]] inline media::EncodeBackendFactory scriptedFactory(ExportScript* script) {
    return
        [script](const media::EncodeSpec& spec,
                 const gpu::CodecRoute& route) -> Result<std::unique_ptr<media::IEncodeBackend>> {
            script->factoryCalls.fetch_add(1);
            {
                std::lock_guard<std::mutex> lock(script->mutex);
                script->spec = spec;
                script->route = route;
            }
            auto backend = std::make_unique<MarkerWritingBackend>(script);
            std::function<void()> hook;
            {
                std::lock_guard<std::mutex> lock(script->mutex);
                hook = script->afterCreateHook;
            }
            // The output file exists by now, so a hook that parks here lets the
            // property observe the real file before any cleanup can remove it.
            if (hook) hook();
            return std::unique_ptr<media::IEncodeBackend>(std::move(backend));
        };
}

/// The per-frame markers in a finished output file: the frame count as recovered
/// from the FILE, independent of anything the coordinator reported. The header,
/// trailer and audio blocks are the only other content and contain no 'F'.
[[nodiscard]] inline std::size_t frameMarkersInFile(const std::filesystem::path& path) {
    const std::string bytes = readAllBytes(path);
    return static_cast<std::size_t>(std::count(bytes.begin(), bytes.end(), kFrameMarker));
}

// ---------------------------------------------------------------------------
// The generated parameter space (Requirements 7.1, 7.9)
// ---------------------------------------------------------------------------
//
// Every generator draws a primitive index into one of these tables rather than a
// Gen of a domain type, so a reported counterexample is always printable.

/// The four supported containers, two of them in mixed case so the documented
/// case-insensitive match is exercised: the engine-level spec must always carry
/// the lower-case short-name.
constexpr const char* kContainers[] = {"mp4", "mov", "mkv", "webm", "MP4", "WebM"};
constexpr int kContainerCount = 6;

constexpr gpu::CodecId kCodecs[] = {gpu::CodecId::H264, gpu::CodecId::HEVC, gpu::CodecId::VP9};
constexpr int kCodecCount = 3;

/// Frame rates across the accepted 1–120 range, integer and NTSC-style rational.
const FrameRate kFrameRates[] = {
    FrameRate{1, 1},        FrameRate{24, 1},       FrameRate{25, 1},  FrameRate{30, 1},
    FrameRate{50, 1},       FrameRate{60, 1},       FrameRate{120, 1}, FrameRate{24000, 1001},
    FrameRate{30000, 1001}, FrameRate{60000, 1001},
};
constexpr int kFrameRateCount = 10;

/// Export geometries spanning the accepted range endpoints, 128×128 to 3840×2160.
/// Every dimension is even, which the export codecs' 4:2:0 chroma subsampling
/// requires.
const Resolution kGeometries[] = {
    Resolution{128, 128}, Resolution{128, 256},  Resolution{256, 144},   Resolution{320, 240},
    Resolution{640, 360}, Resolution{1280, 720}, Resolution{1920, 1080}, Resolution{3840, 2160},
};
constexpr int kGeometryCount = 8;

/// How many frames a case may export at `resolution`, under a fixed pixel budget:
/// the largest accepted geometry exports a frame or two and the smallest exports a
/// long timeline, so both endpoints are covered without any case becoming slow.
[[nodiscard]] inline int maxFramesFor(Resolution resolution) {
    constexpr std::uint64_t kPixelBudget = 3'000'000;
    const std::uint64_t pixels = static_cast<std::uint64_t>(resolution.width) * resolution.height;
    const std::uint64_t frames = pixels == 0 ? 1 : kPixelBudget / pixels;
    return static_cast<int>(std::clamp<std::uint64_t>(frames, 1, 48));
}

// ---------------------------------------------------------------------------
// Synthetic capabilities: the hardware lane on a host with no GPU and no SDK
// ---------------------------------------------------------------------------

[[nodiscard]] inline gpu::GpuCaps hardwareCapsFor(gpu::CodecId codec) {
    gpu::GpuCaps caps;
    caps.vendorId = gpu::GpuVendor::NVIDIA;
    caps.vendor = "NVIDIA";
    caps.supportsCompute = true;
    caps.hwDecode = true;
    caps.hwEncode = true;
    caps.decodeCodecs = {codec};
    caps.encodeCodecs = {codec};
    caps.vramBytes = 4ull * 1024 * 1024 * 1024;
    return caps;
}

// ---------------------------------------------------------------------------
// The timeline under export
// ---------------------------------------------------------------------------

/// A project whose single video clip spans exactly `videoFrames` whole frames at
/// `fps` from t=0, plus — when asked — one unmuted audio-bearing track of the same
/// length. Whole-frame geometry makes the planned frame count exactly
/// `videoFrames`; the planner is nevertheless consulted as the oracle rather than
/// assumed.
[[nodiscard]] inline Project makeTimeline(int videoFrames, FrameRate fps, Resolution canvas,
                                          bool withAudioTrack) {
    Project project;
    project.id = Uuid::generateV4();
    project.name = "export-coordinator-property";
    project.timelineFps = fps;
    project.canvas = canvas;

    Clip video;
    video.id = Uuid::generateV4();
    video.timelineStart = Duration::zero();
    video.sourceIn = Duration::zero();
    video.sourceOut = fps.durationForFrames(videoFrames);
    video.opacity = 1.0;
    Track videoTrack;
    videoTrack.id = Uuid::generateV4();
    videoTrack.kind = TrackKind::Video;
    videoTrack.clips = {video};
    project.tracks.push_back(videoTrack);

    if (withAudioTrack) {
        Clip audio;
        audio.id = Uuid::generateV4();
        audio.timelineStart = Duration::zero();
        audio.sourceIn = Duration::zero();
        audio.sourceOut = fps.durationForFrames(videoFrames);
        audio.gain = 1.0;
        Track audioTrack;
        audioTrack.id = Uuid::generateV4();
        audioTrack.kind = TrackKind::Audio;
        audioTrack.clips = {audio};
        project.tracks.push_back(audioTrack);
    }
    return project;
}

/// A live session, a live GPU context, a teardown queue and the coordinator under
/// test — everything one generated case needs. Declared AFTER the script and the
/// gates it shares with the worker, so the coordinator (whose destructor cancels
/// and joins) is destroyed before the state the worker touches.
struct Harness {
    ProjectSession session{};
    media::DecoderTeardownQueue teardown{};
    std::unique_ptr<gpu::GpuContext> context{};
    std::unique_ptr<ExportCoordinator> coordinator{};

    Harness() : context(std::make_unique<gpu::GpuContext>(gpu::GpuContext::softwareFallback())) {}

    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;

    /// Install `project` as the live document. Returns false when the session
    /// refuses it, which callers assert on rather than ignore.
    [[nodiscard]] bool install(const Project& project) {
        if (session
                .createProject("export-coordinator-property", project.timelineFps, project.canvas,
                               defaultColorSpace())
                .isError()) {
            return false;
        }
        Project installed = session.engine().snapshot();
        installed.timelineFps = project.timelineFps;
        installed.canvas = project.canvas;
        installed.tracks = project.tracks;
        return session.engine().reset(installed).isOk();
    }

    void make(ExportCoordinatorOptions options) {
        coordinator =
            std::make_unique<ExportCoordinator>(session, *context, teardown, std::move(options));
    }

    [[nodiscard]] std::string serialized() { return serializeProject(session.engine().snapshot()); }
};

/// Options for a hostless export: the injected clock, the scripted backend, an
/// injected clip-frame provider, an export-local software GPU context, and a
/// virtual-time capability probe so no probe thread is created and no deadline is
/// ever waited out.
[[nodiscard]] inline ExportCoordinatorOptions hostlessOptions(ExportScript* script,
                                                              ManualClock* clock,
                                                              Resolution resolution) {
    ExportCoordinatorOptions options;
    options.clock = clock->fn();
    options.encodeFactory = scriptedFactory(script);
    options.frameProvider = [resolution](const Clip&, Duration) -> Result<gpu::SourceFrame> {
        return gpu::SourceFrame::solid(resolution.width, resolution.height,
                                       gpu::RgbaColor{31, 63, 95, 255});
    };
    options.gpuContextFactory = []() -> Result<gpu::GpuContext> {
        return gpu::GpuContext::softwareFallback();
    };
    options.selector.probe = media::EncoderSelector::capsCapabilityProbe();
    options.selector.awaiter = [](const media::CapabilityProbe& probe, gpu::CodecId codec,
                                  const gpu::GpuCaps& caps, std::chrono::milliseconds) {
        return probe(codec, caps) ? media::ProbeOutcome::Supported
                                  : media::ProbeOutcome::Unsupported;
    };
    return options;
}

/// A request over the valid parameter space (Requirement 7.1).
[[nodiscard]] inline ExportRequest2 makeRequest(const std::filesystem::path& output,
                                                const std::string& container, gpu::CodecId codec,
                                                Resolution resolution, FrameRate frameRate,
                                                std::int64_t bitrateKbps, bool includeAudio,
                                                bool preferHardware) {
    ExportRequest2 request;
    request.outputPath = output;
    request.container = container;
    request.codec = codec;
    request.resolution = resolution;
    request.frameRate = frameRate;
    request.bitrateKbps = bitrateKbps;
    request.includeAudio = includeAudio;
    request.preferHardware = preferHardware;
    request.overwrite = false;
    return request;
}

}  // namespace

}  // namespace testsupport_exportcoordinatorpropertytest
}  // namespace palmier::services

#endif  // PALMIER_TESTS_SERVICES_EXPORT_COORDINATOR_PROPERTY_TEST_SUPPORT_HPP_
