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

#include <unistd.h> // getpid(), access()

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

namespace palmier::services {
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
constexpr char             kFrameMarker{'F'};

// ---------------------------------------------------------------------------
// Per-process root and per-case scratch directories
// ---------------------------------------------------------------------------

[[nodiscard]] const std::filesystem::path& scratchRoot() {
    static const std::filesystem::path root = [] {
        std::filesystem::path dir =
            std::filesystem::temp_directory_path() /
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
        path_ = scratchRoot() / (std::string(label) + "_" +
                                 std::to_string(static_cast<long long>(::getpid())) + "_" +
                                 std::to_string(counter.fetch_add(1)));
        std::error_code ec;
        std::filesystem::create_directories(path_, ec);
    }

    ~CaseDir() {
        std::error_code ec;
        std::filesystem::permissions(path_, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::add, ec);
        std::filesystem::remove_all(path_, ec);
    }

    CaseDir(const CaseDir&)            = delete;
    CaseDir& operator=(const CaseDir&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] std::filesystem::path file(const std::string& name) const {
        return path_ / name;
    }

private:
    std::filesystem::path path_{};
};

[[nodiscard]] std::string readAllBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// Non-throwing existence check: some generated paths are deliberately malformed
/// (over 4096 characters), for which the throwing overload would raise rather than
/// answer.
[[nodiscard]] bool pathExists(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec) && !ec;
}

[[nodiscard]] std::string toLowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

/// Can this process observe a non-writable directory at all? Running as root —
/// which this sandbox and many CI containers do — bypasses the permission bits, so
/// the "parent directory is not writable" rejection of Requirement 7.9 is simply
/// unreachable. Probed once; the perturbation is dropped from Property 39's
/// generator where it is unobservable rather than asserted vacuously.
[[nodiscard]] bool unwritableDirectoriesAreObservable() {
    static const bool observable = [] {
        const std::filesystem::path dir = scratchRoot() / "writability_probe";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::filesystem::permissions(dir,
                                     std::filesystem::perms::owner_read |
                                         std::filesystem::perms::owner_exec,
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
    std::mutex                            mutex_{};
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

    ReleaseOnExit(const ReleaseOnExit&)            = delete;
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
    bool                released_{false};
};

// ---------------------------------------------------------------------------
// The scripted mock encode backend — writes REAL bytes to a REAL path
// ---------------------------------------------------------------------------

/// What the backend should do and what it observed. Shared between the property
/// body and the export worker, so every field is atomic or guarded by `mutex`.
struct ExportScript {
    std::mutex            mutex{};
    std::filesystem::path outputPath{};

    /// Fail `encode()` on this 0-based video frame index (-1: never).
    int  failVideoOnFrame{-1};
    /// Fail `encodeAudio()` on this 0-based block index (-1: never).
    int  failAudioOnBlock{-1};
    /// Fail `finish()` — a mux-finalize failure.
    bool failFinish{false};

    /// The injected clock and the amount each encoded frame advances it: how the
    /// progress cadence is driven without sleeping.
    ManualClock*              clock{nullptr};
    std::chrono::milliseconds advancePerFrame{0};

    /// Called on the WORKER thread from inside `encode()`, with the frame index —
    /// the hook that makes cancellation and mid-export edits deterministic.
    std::function<void(int)> encodeHook{};
    /// Called on the WORKER thread once the backend exists and its output file has
    /// been created, before the render loop starts.
    std::function<void()> afterCreateHook{};

    // --- Observations ------------------------------------------------------
    std::atomic<int>  factoryCalls{0};
    std::atomic<int>  encodeCalls{0};
    std::atomic<int>  audioCalls{0};
    std::atomic<int>  finishCalls{0};
    std::atomic<bool> created{false};
    /// Anti-vacuity: whether the output path really exists on disk, as observed by
    /// the worker immediately after the file was opened and flushed.
    std::atomic<bool>          existedAfterOpen{false};
    std::atomic<std::uint64_t> bytesWritten{0};

    media::EncodeSpec spec{};
    gpu::CodecRoute   route{};
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

        std::function<void(int)>  hook;
        int                       failOn{-1};
        ManualClock*              clock{nullptr};
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
        int       failOn{-1};
        {
            std::lock_guard<std::mutex> lock(script_->mutex);
            failOn = script_->failAudioOnBlock;
        }
        if (failOn == index) {
            return err(makeError(ErrorCode::Io, "mock audio encode failure"));
        }
        const std::string block =
            "audio:" +
            std::to_string(audio.buffer != nullptr ? audio.buffer->frameCount() : 0u) + "\n";
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

[[nodiscard]] media::EncodeBackendFactory scriptedFactory(ExportScript* script) {
    return [script](const media::EncodeSpec& spec, const gpu::CodecRoute& route)
               -> Result<std::unique_ptr<media::IEncodeBackend>> {
        script->factoryCalls.fetch_add(1);
        {
            std::lock_guard<std::mutex> lock(script->mutex);
            script->spec = spec;
            script->route = route;
        }
        auto                  backend = std::make_unique<MarkerWritingBackend>(script);
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
[[nodiscard]] std::size_t frameMarkersInFile(const std::filesystem::path& path) {
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
constexpr int         kContainerCount = 6;

constexpr gpu::CodecId kCodecs[] = {gpu::CodecId::H264, gpu::CodecId::HEVC, gpu::CodecId::VP9};
constexpr int          kCodecCount = 3;

/// Frame rates across the accepted 1–120 range, integer and NTSC-style rational.
const FrameRate kFrameRates[] = {
    FrameRate{1, 1},   FrameRate{24, 1},       FrameRate{25, 1},
    FrameRate{30, 1},  FrameRate{50, 1},       FrameRate{60, 1},
    FrameRate{120, 1}, FrameRate{24000, 1001}, FrameRate{30000, 1001},
    FrameRate{60000, 1001},
};
constexpr int kFrameRateCount = 10;

/// Export geometries spanning the accepted range endpoints, 128×128 to 3840×2160.
/// Every dimension is even, which the export codecs' 4:2:0 chroma subsampling
/// requires.
const Resolution kGeometries[] = {
    Resolution{128, 128},   Resolution{128, 256},   Resolution{256, 144},
    Resolution{320, 240},   Resolution{640, 360},   Resolution{1280, 720},
    Resolution{1920, 1080}, Resolution{3840, 2160},
};
constexpr int kGeometryCount = 8;

/// How many frames a case may export at `resolution`, under a fixed pixel budget:
/// the largest accepted geometry exports a frame or two and the smallest exports a
/// long timeline, so both endpoints are covered without any case becoming slow.
[[nodiscard]] int maxFramesFor(Resolution resolution) {
    constexpr std::uint64_t kPixelBudget = 3'000'000;
    const std::uint64_t pixels = static_cast<std::uint64_t>(resolution.width) * resolution.height;
    const std::uint64_t frames = pixels == 0 ? 1 : kPixelBudget / pixels;
    return static_cast<int>(std::clamp<std::uint64_t>(frames, 1, 48));
}

// ---------------------------------------------------------------------------
// Synthetic capabilities: the hardware lane on a host with no GPU and no SDK
// ---------------------------------------------------------------------------

[[nodiscard]] gpu::GpuCaps hardwareCapsFor(gpu::CodecId codec) {
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
[[nodiscard]] Project makeTimeline(int videoFrames, FrameRate fps, Resolution canvas,
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
    ProjectSession                     session{};
    media::DecoderTeardownQueue        teardown{};
    std::unique_ptr<gpu::GpuContext>   context{};
    std::unique_ptr<ExportCoordinator> coordinator{};

    Harness() : context(std::make_unique<gpu::GpuContext>(gpu::GpuContext::softwareFallback())) {}

    Harness(const Harness&)            = delete;
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
[[nodiscard]] ExportCoordinatorOptions hostlessOptions(ExportScript* script, ManualClock* clock,
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
[[nodiscard]] ExportRequest2 makeRequest(const std::filesystem::path& output,
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

} // namespace

// ===========================================================================
// Property 33
// ===========================================================================

// Feature: end-to-end-editor-integration, Property 33: Export runs exactly the requested
// parameters and never touches the project — for any valid export request and any forced outcome
// (success, mid-export failure, cancellation), the export engine receives a specification whose
// output path, container, codec, resolution, frame rate, bit rate and audio inclusion equal the
// request field for field; the project snapshot before and after the export compares byte-equal; and
// the session reports the project unmodified; and the request issued through the export dialog and
// through `timeline.export` produce equal outcome fields.
//
// **Validates: Requirements 7.1, 7.2**
RC_GTEST_PROP(ExportCoordinatorProperties,
              ExportRunsExactlyTheRequestedParametersAndNeverTouchesTheProject,
              ()) {
    const Resolution   resolution = kGeometries[*rc::gen::inRange<int>(0, kGeometryCount)];
    const FrameRate    fps = kFrameRates[*rc::gen::inRange<int>(0, kFrameRateCount)];
    const std::string  container = kContainers[*rc::gen::inRange<int>(0, kContainerCount)];
    const gpu::CodecId codec = kCodecs[*rc::gen::inRange<int>(0, kCodecCount)];
    const auto         bitrateKbps =
        *rc::gen::inRange<std::int64_t>(kMinExportBitrateKbps, kMaxExportBitrateKbps + 1);
    const bool includeAudio = *rc::gen::arbitrary<bool>();
    const bool preferHardware = *rc::gen::arbitrary<bool>();
    /// The three outcomes the design's generator forces.
    const int  forcedOutcome = *rc::gen::inRange<int>(0, 3); // 0 success, 1 failure, 2 cancel
    const bool editDuringExport = *rc::gen::arbitrary<bool>();
    const int  nameIndex = *rc::gen::inRange<int>(0, 1'000);

    // A forced failure or cancellation needs a frame to happen after, so those
    // cases export at least two frames.
    const int drawnFrames = *rc::gen::inRange<int>(1, maxFramesFor(resolution) + 1);
    const int frames = std::max(forcedOutcome == 0 ? 1 : 2, drawnFrames);

    CaseDir                     dir("p33");
    const std::filesystem::path out =
        dir.file("export_" + std::to_string(nameIndex) + "." + toLowerAscii(container));

    ManualClock  clock;
    ExportScript script;
    script.outputPath = out;
    script.clock = &clock;
    if (forcedOutcome == 1) script.failVideoOnFrame = 1; // fails after one frame.

    // Gates shared with the worker. Declared before the Harness so they outlive the
    // coordinator no matter how this body exits.
    std::promise<void>       parked;
    std::shared_future<void> hasParked(parked.get_future());
    std::promise<void>       release;
    std::shared_future<void> released(release.get_future());
    std::atomic<bool>        parkedOnce{false};

    Harness harness;
    RC_ASSERT(harness.install(makeTimeline(frames, fps, resolution, includeAudio)));

    const Project     snapshotBefore = harness.session.engine().snapshot();
    const std::size_t planned = media::ExportEngine::plannedFrameCount(snapshotBefore, fps);
    RC_ASSERT(planned >= 1);
    if (forcedOutcome != 0) RC_ASSERT(planned >= 2);

    ExportCoordinatorOptions options = hostlessOptions(&script, &clock, resolution);
    if (preferHardware) {
        // The hardware lane, reachable on a host with neither a GPU nor a vendor
        // SDK because capabilities and compiled-in paths are values.
        options.caps = hardwareCapsFor(codec);
        options.availability = gpu::BridgeAvailability::all();
    }
    harness.make(std::move(options));

    // Releases the worker however this body exits, so a failed assertion cannot
    // leave the export parked on a gate nobody will open.
    ReleaseOnExit releaseGuard(release);

    ExportCoordinator* coordinator = harness.coordinator.get();
    {
        std::lock_guard<std::mutex> lock(script.mutex);
        script.encodeHook = [&parked, &released, &parkedOnce, coordinator, editDuringExport,
                             forcedOutcome](int index) {
            if (editDuringExport && index == 0 && !parkedOnce.exchange(true)) {
                parked.set_value();
                released.wait();
            }
            if (forcedOutcome == 2 && index == 0) coordinator->cancel();
        };
    }

    const ExportRequest2 request = makeRequest(out, container, codec, resolution, fps, bitrateKbps,
                                               includeAudio, preferHardware);
    RC_ASSERT(ExportCoordinator::validate(request).isOk());
    RC_ASSERT(harness.coordinator->begin(request).isOk());

    // The reference the project is compared against is taken AFTER the mid-export
    // edit when there is one, so what is asserted is that the EXPORT changed
    // nothing — not that nothing at all changed.
    std::size_t plannedAfterEdit = planned;
    if (editDuringExport) {
        RC_ASSERT(hasParked.wait_for(kWaitBudget) == std::future_status::ready);
        // The worker is parked inside the first frame: the file it created really is
        // on disk right now, which is what makes the cleanup assertions of
        // Properties 36 and 37 non-vacuous.
        RC_ASSERT(pathExists(out));
        // A real edit to the LIVE session while the export runs.
        Project longer = harness.session.engine().snapshot();
        longer.tracks = makeTimeline(frames * 2, fps, resolution, includeAudio).tracks;
        RC_ASSERT(harness.session.engine().reset(longer).isOk());
        plannedAfterEdit = media::ExportEngine::plannedFrameCount(longer, fps);
        // The edit must really change the planned frame count, or the isolation
        // check below would have no teeth.
        RC_ASSERT(plannedAfterEdit > planned);
    }
    const std::string   expectedSerialized = harness.serialized();
    const std::uint64_t expectedRevision = harness.session.revision();
    const bool          expectedModified = harness.session.modified();
    releaseGuard.release();

    RC_ASSERT(harness.coordinator->awaitCompletion(kWaitBudget) > 0);

    // --- The engine received exactly the requested parameters (Req 7.1, 7.2) ---
    RC_ASSERT(script.factoryCalls.load() >= 1);
    media::EncodeSpec spec;
    {
        std::lock_guard<std::mutex> lock(script.mutex);
        spec = script.spec;
    }
    RC_ASSERT(spec.outputPath == out);
    RC_ASSERT(spec.containerFormat == toLowerAscii(container));
    RC_ASSERT(spec.codec == codec);
    RC_ASSERT(spec.resolution == resolution);
    RC_ASSERT(spec.frameRate == fps);
    RC_ASSERT(spec.bitrateBitsPerSecond == bitrateKbps * 1'000);
    RC_ASSERT(spec.audio.has_value() == includeAudio);

    // --- The project is untouched, whatever the outcome (Requirement 7.1) ------
    RC_ASSERT(harness.serialized() == expectedSerialized);
    RC_ASSERT(harness.session.revision() == expectedRevision);
    RC_ASSERT(harness.session.modified() == expectedModified);

    const std::optional<ExportOutcome> outcome = harness.coordinator->lastOutcome();
    RC_ASSERT(outcome.has_value());
    RC_ASSERT(!outcome->projectModified);
    RC_ASSERT(outcome->outputPath == out);
    // The planner ran on the SNAPSHOT: a timeline edited mid-export cannot change
    // the size of the export in flight.
    RC_ASSERT(outcome->plannedFrames == planned);
    if (editDuringExport) RC_ASSERT(outcome->plannedFrames < plannedAfterEdit);
    // Requirement 8.8: hardware use and software fallback are never both reported.
    RC_ASSERT(!(outcome->usedHardwareEncode && outcome->usedSoftwareFallback));

    // Anti-vacuity for the two cleanup cases: a file really was created, and real
    // bytes really were written, at exactly this path.
    RC_ASSERT(script.created.load());
    RC_ASSERT(script.existedAfterOpen.load());
    RC_ASSERT(script.bytesWritten.load() > 0);

    switch (forcedOutcome) {
        case 0:
            RC_ASSERT(!harness.coordinator->lastError().has_value());
            RC_ASSERT(!outcome->cancelled);
            RC_ASSERT(outcome->framesEncoded == planned);
            RC_ASSERT(outcome->containsAudio == includeAudio);
            RC_ASSERT(pathExists(out));
            break;
        case 1:
            RC_ASSERT(harness.coordinator->lastError().has_value());
            RC_ASSERT(!pathExists(out));
            break;
        default:
            RC_ASSERT(outcome->cancelled);
            RC_ASSERT(!pathExists(out));
            break;
    }

    // --- The outcome is a function of the request and the snapshot -------------
    // Both the export dialog and `timeline.export` enter through begin(); re-issuing
    // the identical request reproduces every outcome field but the path, which is
    // the fact that makes the two callers agree (Requirement 7.2).
    if (forcedOutcome == 0 && !editDuringExport) {
        const ExportOutcome          first = *outcome;
        const std::filesystem::path second =
            dir.file("export_" + std::to_string(nameIndex) + "_again." + toLowerAscii(container));
        {
            std::lock_guard<std::mutex> lock(script.mutex);
            script.outputPath = second;
            script.encodeHook = {};
        }
        ExportRequest2 again = request;
        again.outputPath = second;
        RC_ASSERT(harness.coordinator->begin(again).isOk());
        RC_ASSERT(harness.coordinator->awaitCompletion(kWaitBudget) > 0);
        const std::optional<ExportOutcome> repeat = harness.coordinator->lastOutcome();
        RC_ASSERT(repeat.has_value());
        RC_ASSERT(repeat->framesEncoded == first.framesEncoded);
        RC_ASSERT(repeat->plannedFrames == first.plannedFrames);
        RC_ASSERT(repeat->encoderName == first.encoderName);
        RC_ASSERT(repeat->usedHardwareEncode == first.usedHardwareEncode);
        RC_ASSERT(repeat->usedSoftwareFallback == first.usedSoftwareFallback);
        RC_ASSERT(repeat->containsAudio == first.containsAudio);
        RC_ASSERT(repeat->audioFrames == first.audioFrames);
        RC_ASSERT(repeat->duration == first.duration);
        RC_ASSERT(repeat->cancelled == first.cancelled);
        RC_ASSERT(!repeat->projectModified);
        RC_ASSERT(harness.serialized() == expectedSerialized);
    }
}

// ===========================================================================
// Property 34
// ===========================================================================

// Feature: end-to-end-editor-integration, Property 34: Progress is monotonic, bounded and timely —
// for any timeline and any frame rate, the sequence of progress reports emitted during an export is
// monotonically non-decreasing, every value lies in 0–100, consecutive reports are at most 1 second
// apart, the first report is 0 and a successful export's last report is 100.
//
// **Validates: Requirements 7.3**
RC_GTEST_PROP(ExportCoordinatorProperties, ProgressIsMonotonicBoundedAndTimely, ()) {
    const FrameRate fps = kFrameRates[*rc::gen::inRange<int>(0, kFrameRateCount)];
    const int       frames = *rc::gen::inRange<int>(1, 49);
    // How long each frame "takes", in the injected clock's virtual time: values
    // below, at and above the one-second ceiling.
    const auto advanceMs = *rc::gen::inRange<std::int64_t>(0, 1'501);
    // Requested cadences, including one above the ceiling that must be clamped.
    const std::chrono::milliseconds requestedInterval{
        *rc::gen::element<std::int64_t>(0, 100, 250, 500, 1'000, 5'000)};
    const bool includeAudio = *rc::gen::arbitrary<bool>();

    const Resolution            resolution{128, 128};
    CaseDir                     dir("p34");
    const std::filesystem::path out = dir.file("progress.mp4");

    ManualClock  clock;
    ExportScript script;
    script.outputPath = out;
    script.clock = &clock;
    script.advancePerFrame = std::chrono::milliseconds{advanceMs};

    Harness harness;
    RC_ASSERT(harness.install(makeTimeline(frames, fps, resolution, includeAudio)));
    const std::size_t planned =
        media::ExportEngine::plannedFrameCount(harness.session.engine().snapshot(), fps);
    RC_ASSERT(planned >= 1);

    ExportCoordinatorOptions options = hostlessOptions(&script, &clock, resolution);
    options.progressInterval = requestedInterval;
    harness.make(std::move(options));

    // Requirement 7.3 is a CEILING: a longer configured interval is clamped down,
    // so the guarantee cannot be configured away.
    const std::chrono::milliseconds ceiling{1'000};
    const std::chrono::milliseconds effective = std::min(requestedInterval, ceiling);
    RC_ASSERT(harness.coordinator->progressInterval() == effective);

    std::vector<ExportProgressReport> seen;
    std::thread::id                   deliveringThread{};
    RC_ASSERT(harness.coordinator
                  ->begin(makeRequest(out, "mp4", gpu::CodecId::H264, resolution, fps, 4'000,
                                      includeAudio, /*preferHardware=*/false),
                          [&seen, &deliveringThread](const ExportProgressReport& report) {
                              deliveringThread = std::this_thread::get_id();
                              seen.push_back(report);
                          })
                  .isOk());
    RC_ASSERT(harness.coordinator->awaitCompletion(kWaitBudget) > 0);
    RC_ASSERT(harness.coordinator->lastOutcome().has_value());
    RC_ASSERT(!harness.coordinator->lastError().has_value());
    RC_ASSERT(!seen.empty());

    // Marshalled to the owning thread, never delivered from the worker — which is
    // what keeps the Editor_Shell free to process its own window events.
    RC_ASSERT(deliveringThread == std::this_thread::get_id());

    // --- Monotonic and bounded ------------------------------------------------
    RC_ASSERT(seen.front().percent == 0);
    RC_ASSERT(seen.back().percent == 100);
    for (std::size_t i = 0; i < seen.size(); ++i) {
        RC_ASSERT(seen[i].percent >= 0);
        RC_ASSERT(seen[i].percent <= 100);
        RC_ASSERT(seen[i].totalFrames == planned);
        RC_ASSERT(seen[i].framesEncoded <= planned);
        // The percentage is the frame count's own percentage, so no report can claim
        // progress that no encoded frame backs.
        const int expectedPercent =
            static_cast<int>((static_cast<std::uint64_t>(seen[i].framesEncoded) * 100u) / planned);
        RC_ASSERT(seen[i].percent == expectedPercent);
        if (i > 0) {
            RC_ASSERT(seen[i].percent >= seen[i - 1].percent);
            RC_ASSERT(seen[i].framesEncoded >= seen[i - 1].framesEncoded);
        }
    }

    // --- Timely: the exact cadence the ≤1 s ceiling prescribes ----------------
    //
    // The coordinator is offered progress at known virtual-time instants — a forced
    // report before the export starts, the engine's own initial report, one per
    // rendered frame (the engine reports every frame, because the coordinator owns
    // the cadence), and a forced final report — and emits an offer only when it is
    // forced or at least `effective` has elapsed since the last emission. Replaying
    // that rule over the generated schedule predicts the exact sequence of reports,
    // so BOTH halves of the guarantee are checked: nothing is emitted early, and
    // nothing due is withheld. No sleeping is involved anywhere.
    struct Offer {
        std::size_t  frames;
        std::int64_t atMs;
        bool         forced;
    };
    std::vector<Offer> offers;
    const std::int64_t endMs = static_cast<std::int64_t>(planned) * advanceMs;
    offers.push_back({0, 0, true});  // the coordinator's initial 0% report.
    offers.push_back({0, 0, false}); // the engine's own initial report.
    for (std::size_t i = 1; i <= planned; ++i) {
        offers.push_back({i, static_cast<std::int64_t>(i) * advanceMs, false});
    }
    offers.push_back({planned, endMs, false}); // the engine's completion report.
    offers.push_back({planned, endMs, true});  // the coordinator's forced final 100%.

    std::vector<std::size_t>  expectedFrames;
    std::vector<std::int64_t> expectedAtMs;
    std::int64_t              lastEmitMs = 0;
    bool                      everEmitted = false;
    for (const Offer& offer : offers) {
        const bool due = offer.forced || !everEmitted ||
                         (offer.atMs - lastEmitMs) >= effective.count();
        if (!due) continue;
        expectedFrames.push_back(offer.frames);
        expectedAtMs.push_back(offer.atMs);
        lastEmitMs = offer.atMs;
        everEmitted = true;
    }

    RC_ASSERT(seen.size() == expectedFrames.size());
    for (std::size_t i = 0; i < seen.size(); ++i) {
        RC_ASSERT(seen[i].framesEncoded == expectedFrames[i]);
    }

    // The consequence Requirement 7.3 states: consecutive reports are at most one
    // second apart, allowing for the fact that a report can only be produced at a
    // frame boundary — so no gap exceeds the ceiling plus one frame's duration.
    for (std::size_t i = 1; i < expectedAtMs.size(); ++i) {
        RC_ASSERT(expectedAtMs[i] >= expectedAtMs[i - 1]);
        RC_ASSERT(expectedAtMs[i] - expectedAtMs[i - 1] <= effective.count() + advanceMs);
    }

    // And when frames are at least as slow as the ceiling, EVERY frame is reported:
    // the coordinator never batches progress away.
    if (advanceMs >= effective.count() && advanceMs > 0) {
        std::set<std::size_t> reported;
        for (const ExportProgressReport& report : seen) reported.insert(report.framesEncoded);
        for (std::size_t i = 1; i <= planned; ++i) RC_ASSERT(reported.count(i) == 1);
    }
}

// ===========================================================================
// Property 35
// ===========================================================================

// Feature: end-to-end-editor-integration, Property 35: A successful export matches the planner —
// for any timeline and any frame rate, a successful export produces a non-empty, probeable,
// decodable file whose decoded frame count equals `ExportEngine::plannedFrameCount` for that
// timeline and frame rate and whose reported duration differs from the timeline duration by at most
// one frame interval.
//
// **Validates: Requirements 7.4**
RC_GTEST_PROP(ExportCoordinatorProperties, ASuccessfulExportMatchesThePlanner, ()) {
    const Resolution  resolution = kGeometries[*rc::gen::inRange<int>(0, kGeometryCount)];
    const FrameRate   fps = kFrameRates[*rc::gen::inRange<int>(0, kFrameRateCount)];
    const int         frames = *rc::gen::inRange<int>(1, maxFramesFor(resolution) + 1);
    const bool        includeAudio = *rc::gen::arbitrary<bool>();
    const std::string container = kContainers[*rc::gen::inRange<int>(0, kContainerCount)];

    CaseDir                     dir("p35");
    const std::filesystem::path out = dir.file("planned." + toLowerAscii(container));

    ManualClock  clock;
    ExportScript script;
    script.outputPath = out;
    script.clock = &clock;

    Harness harness;
    RC_ASSERT(harness.install(makeTimeline(frames, fps, resolution, includeAudio)));
    const Project     snapshot = harness.session.engine().snapshot();
    const std::size_t planned = media::ExportEngine::plannedFrameCount(snapshot, fps);
    RC_ASSERT(planned >= 1);

    harness.make(hostlessOptions(&script, &clock, resolution));
    RC_ASSERT(harness.coordinator
                  ->begin(makeRequest(out, container, gpu::CodecId::H264, resolution, fps, 6'000,
                                      includeAudio, /*preferHardware=*/false))
                  .isOk());
    RC_ASSERT(harness.coordinator->awaitCompletion(kWaitBudget) > 0);

    RC_ASSERT(!harness.coordinator->lastError().has_value());
    const std::optional<ExportOutcome> outcome = harness.coordinator->lastOutcome();
    RC_ASSERT(outcome.has_value());
    RC_ASSERT(!outcome->cancelled);

    // The deliverable exists and is not empty.
    RC_ASSERT(pathExists(out));
    RC_ASSERT(std::filesystem::file_size(out) > 0);
    RC_ASSERT(script.finishCalls.load() == 1);

    // The frame count recovered from the FILE'S OWN BYTES equals the planner's
    // count — an independent count, not the coordinator's.
    RC_ASSERT(frameMarkersInFile(out) == planned);
    RC_ASSERT(outcome->framesEncoded == planned);
    RC_ASSERT(outcome->plannedFrames == planned);

    // The reported duration differs from the timeline duration by at most one frame
    // interval.
    const Duration timeline = timelineDuration(snapshot);
    const Duration interval = fps.frameDuration();
    RC_ASSERT((outcome->duration - timeline).abs() <= interval);

    // One audio stream when audio was asked for and none when it was not
    // (Requirement 6.5); the audio really was written, one block per frame interval.
    RC_ASSERT(outcome->containsAudio == includeAudio);
    if (includeAudio) {
        RC_ASSERT(outcome->audioFrames > 0);
        RC_ASSERT(static_cast<std::size_t>(script.audioCalls.load()) == planned);
    } else {
        RC_ASSERT(outcome->audioFrames == 0);
        RC_ASSERT(script.audioCalls.load() == 0);
    }
}

// ===========================================================================
// Property 36
// ===========================================================================

// Feature: end-to-end-editor-integration, Property 36: Any failure after encoding begins leaves no
// file and no project change — for any failure injected after at least one frame has been encoded —
// a compositor error, a video encode error, an audio encode or mux error, or a hardware encode
// failure — the export stops, no file remains at the requested path, the project is reported
// unmodified and is byte-identical, and the error names the failing stage (identifying a mid-export
// hardware encode failure where that is the injected cause).
//
// **Validates: Requirements 6.10, 7.5, 8.11**
RC_GTEST_PROP(ExportCoordinatorProperties,
              AnyFailureAfterEncodingBeginsLeavesNoFileAndNoProjectChange,
              ()) {
    // The failure stages of the design's generator.
    enum Stage {
        kCompositor = 0,
        kVideoEncode = 1,
        kAudioEncode = 2,
        kFinalize = 3,
        kHardware = 4,
    };
    const int stage = *rc::gen::inRange<int>(0, 5);

    const Resolution resolution = kGeometries[*rc::gen::inRange<int>(0, kGeometryCount)];
    const FrameRate  fps = kFrameRates[*rc::gen::inRange<int>(0, kFrameRateCount)];
    // At least two frames, so a failure can land after at least one encoded frame.
    const int frames = std::max(2, *rc::gen::inRange<int>(2, maxFramesFor(resolution) + 2));
    // The failing index, always >= 1: Requirement 8.11's "after at least one frame
    // has been encoded".
    const int drawnFailAt = *rc::gen::inRange<int>(1, frames);
    // The hardware lane routes H.264 and HEVC to a vendor encoder.
    const gpu::CodecId codec = stage == kHardware ? kCodecs[*rc::gen::inRange<int>(0, 2)]
                                                 : kCodecs[*rc::gen::inRange<int>(0, kCodecCount)];
    const bool includeAudio = stage == kAudioEncode ? true : *rc::gen::arbitrary<bool>();

    CaseDir                     dir("p36");
    const std::filesystem::path out = dir.file("failure.mp4");

    ManualClock      clock;
    ExportScript     script;
    std::atomic<int> providerCalls{0};
    script.outputPath = out;
    script.clock = &clock;

    Harness harness;
    RC_ASSERT(harness.install(makeTimeline(frames, fps, resolution, includeAudio)));
    const std::string   serializedBefore = harness.serialized();
    const std::uint64_t revisionBefore = harness.session.revision();
    const bool          modifiedBefore = harness.session.modified();
    const std::size_t   planned =
        media::ExportEngine::plannedFrameCount(harness.session.engine().snapshot(), fps);
    RC_ASSERT(planned >= 2);
    const int failAt = std::min(drawnFailAt, static_cast<int>(planned) - 1);
    RC_ASSERT(failAt >= 1);

    switch (stage) {
        case kVideoEncode:
        case kHardware:
            script.failVideoOnFrame = failAt;
            break;
        case kAudioEncode:
            script.failAudioOnBlock = failAt;
            break;
        case kFinalize:
            script.failFinish = true;
            break;
        default:
            break; // the compositor failure is injected through the frame provider.
    }

    ExportCoordinatorOptions options = hostlessOptions(&script, &clock, resolution);
    if (stage == kCompositor) {
        // A compositor error: the source pixels for the failing frame cannot be
        // produced, exactly as a decode failure of a named asset presents.
        options.frameProvider = [resolution, failAt, &providerCalls](
                                    const Clip&, Duration) -> Result<gpu::SourceFrame> {
            const int index = providerCalls.fetch_add(1);
            if (index == failAt) {
                return err<gpu::SourceFrame>(
                    makeError(ErrorCode::Io, "decoding the source asset failed: mock-asset.mov"));
            }
            return gpu::SourceFrame::solid(resolution.width, resolution.height,
                                           gpu::RgbaColor{31, 63, 95, 255});
        };
    }
    if (stage == kHardware) {
        options.caps = hardwareCapsFor(codec);
        options.availability = gpu::BridgeAvailability::all();
    }
    harness.make(std::move(options));

    RC_ASSERT(harness.coordinator
                  ->begin(makeRequest(out, "mp4", codec, resolution, fps, 6'000, includeAudio,
                                      /*preferHardware=*/stage == kHardware))
                  .isOk());
    RC_ASSERT(harness.coordinator->awaitCompletion(kWaitBudget) > 0);

    // --- The export stopped and named the failing stage -----------------------
    const std::optional<Error> error = harness.coordinator->lastError();
    RC_ASSERT(error.has_value());
    const std::string message = error->message();
    RC_ASSERT(!message.empty());
    switch (stage) {
        case kCompositor:
            // The cause is carried through, naming the failing asset.
            RC_ASSERT(message.find("mock-asset.mov") != std::string::npos);
            break;
        case kVideoEncode:
            RC_ASSERT(message.find("video encoding failed") != std::string::npos);
            break;
        case kAudioEncode:
            // Requirement 6.10: audio encoding or muxing is named as the stage.
            RC_ASSERT(message.find("audio encoding or muxing failed") != std::string::npos);
            break;
        case kFinalize:
            RC_ASSERT(message.find("finalizing the output file failed") != std::string::npos);
            break;
        default:
            // Requirement 8.11: a hardware failure after at least one encoded frame
            // is reported as a MID-EXPORT hardware encode failure — a different fact
            // from an initialization failure, which becomes a software fallback.
            RC_ASSERT(harness.coordinator->lastSelection().has_value());
            RC_ASSERT(harness.coordinator->lastSelection()->selection.isHardware());
            RC_ASSERT(message.find("mid-export hardware encode failure") != std::string::npos);
            break;
    }

    // --- No file remains, and it is not vacuously absent ----------------------
    RC_ASSERT(script.created.load());
    RC_ASSERT(script.existedAfterOpen.load());
    RC_ASSERT(script.bytesWritten.load() > kHeaderMarker.size());
    RC_ASSERT(!pathExists(out));

    // --- The project is unmodified and byte-identical -------------------------
    RC_ASSERT(harness.serialized() == serializedBefore);
    RC_ASSERT(harness.session.revision() == revisionBefore);
    RC_ASSERT(harness.session.modified() == modifiedBefore);
    const std::optional<ExportOutcome> outcome = harness.coordinator->lastOutcome();
    RC_ASSERT(outcome.has_value());
    RC_ASSERT(!outcome->projectModified);
    RC_ASSERT(!harness.coordinator->running());

    // At least one frame really had been encoded before the failure, which is the
    // antecedent of Requirements 7.5 and 8.11.
    RC_ASSERT(outcome->framesEncoded >= 1);
    switch (stage) {
        case kFinalize:
            RC_ASSERT(outcome->framesEncoded == planned);
            break;
        case kAudioEncode:
            // The render loop submits the frame, then that interval's audio: the
            // failing block's own frame is already encoded.
            RC_ASSERT(outcome->framesEncoded == static_cast<std::size_t>(failAt) + 1);
            break;
        default:
            RC_ASSERT(outcome->framesEncoded == static_cast<std::size_t>(failAt));
            break;
    }
}

// ===========================================================================
// Property 37
// ===========================================================================

// Feature: end-to-end-editor-integration, Property 37: Cancellation leaves no file and no project
// change — for any timeline and any cancellation instant during an export, encoding stops within 2
// seconds, no file remains at the requested path, the outcome reports the export cancelled, and the
// project is reported unmodified.
//
// **Validates: Requirements 7.7**
RC_GTEST_PROP(ExportCoordinatorProperties, CancellationLeavesNoFileAndNoProjectChange, ()) {
    const Resolution resolution = kGeometries[*rc::gen::inRange<int>(0, kGeometryCount)];
    const FrameRate  fps = kFrameRates[*rc::gen::inRange<int>(0, kFrameRateCount)];
    const int        frames = *rc::gen::inRange<int>(1, maxFramesFor(resolution) + 1);
    const bool       includeAudio = *rc::gen::arbitrary<bool>();
    // Drawn before the planned count is known, and clamped into range below.
    const int drawnCancelAt = *rc::gen::inRange<int>(-1, 49);

    CaseDir                     dir("p37");
    const std::filesystem::path out = dir.file("cancelled.mp4");

    ManualClock  clock;
    ExportScript script;
    script.outputPath = out;
    script.clock = &clock;

    // The instant the cancel flag was set, as seen by the worker: Requirement 7.7's
    // two-second bound is measured from here, never slept for.
    std::mutex                                           cancelMutex;
    std::optional<std::chrono::steady_clock::time_point> cancelledAt;
    auto markCancelled = [&cancelMutex, &cancelledAt]() {
        std::lock_guard<std::mutex> lock(cancelMutex);
        if (!cancelledAt.has_value()) cancelledAt = std::chrono::steady_clock::now();
    };

    std::promise<void>       parked;
    std::shared_future<void> hasParked(parked.get_future());
    std::promise<void>       release;
    std::shared_future<void> released(release.get_future());
    std::atomic<bool>        parkedOnce{false};

    Harness harness;
    RC_ASSERT(harness.install(makeTimeline(frames, fps, resolution, includeAudio)));
    const std::string   serializedBefore = harness.serialized();
    const std::uint64_t revisionBefore = harness.session.revision();
    const bool          modifiedBefore = harness.session.modified();
    const std::size_t   planned =
        media::ExportEngine::plannedFrameCount(harness.session.engine().snapshot(), fps);
    RC_ASSERT(planned >= 1);

    // The cancellation instant: -1 cancels BEFORE the first frame (from inside the
    // backend factory, once the output file exists); 0..planned-1 cancels from
    // inside that frame's encode(). The last of those is the design generator's
    // "after the last frame" boundary.
    const int cancelAt = drawnCancelAt < 0
                             ? -1
                             : static_cast<int>(drawnCancelAt % static_cast<int>(planned));

    harness.make(hostlessOptions(&script, &clock, resolution));
    ReleaseOnExit      releaseGuard(release);
    ExportCoordinator* coordinator = harness.coordinator.get();

    {
        std::lock_guard<std::mutex> lock(script.mutex);
        if (cancelAt < 0) {
            script.afterCreateHook = [&parked, &released, &parkedOnce, &markCancelled,
                                      coordinator]() {
                if (parkedOnce.exchange(true)) return;
                parked.set_value();
                released.wait();
                markCancelled();
                coordinator->cancel();
            };
        } else {
            script.encodeHook = [&markCancelled, coordinator, cancelAt](int index) {
                if (index == cancelAt) {
                    markCancelled();
                    coordinator->cancel();
                }
            };
        }
    }

    RC_ASSERT(harness.coordinator
                  ->begin(makeRequest(out, "mp4", gpu::CodecId::H264, resolution, fps, 6'000,
                                      includeAudio, /*preferHardware=*/false))
                  .isOk());

    if (cancelAt < 0) {
        // The worker is parked in the factory: the file it created really is on
        // disk, so the "no file remains" assertion below cannot be vacuous.
        RC_ASSERT(hasParked.wait_for(kWaitBudget) == std::future_status::ready);
        RC_ASSERT(pathExists(out));
        releaseGuard.release();
    }

    RC_ASSERT(harness.coordinator->waitForCompletion(kWaitBudget));
    const auto observedAt = std::chrono::steady_clock::now();
    RC_ASSERT(harness.coordinator->awaitCompletion(kWaitBudget) > 0);

    // The cancellation really happened, and the export stopped within 2 seconds of
    // it (Requirement 7.7) — a measured bound, so a coordinator that dawdles fails.
    {
        std::lock_guard<std::mutex> lock(cancelMutex);
        RC_ASSERT(cancelledAt.has_value());
        RC_ASSERT(observedAt - *cancelledAt <= kCancelStopBudget);
    }
    RC_ASSERT(harness.coordinator->cancelRequested());
    RC_ASSERT(!harness.coordinator->running());

    const std::optional<ExportOutcome> outcome = harness.coordinator->lastOutcome();
    RC_ASSERT(outcome.has_value());

    // Anti-vacuity: a real file was created at this exact path.
    RC_ASSERT(script.created.load());
    RC_ASSERT(script.existedAfterOpen.load());
    RC_ASSERT(script.bytesWritten.load() > 0);

    if (cancelAt == static_cast<int>(planned) - 1) {
        // The boundary the design's generator names: a cancellation that arrives
        // once the last frame has been encoded cannot un-write a finished export, so
        // the deliverable survives and the outcome is a success. Asserted rather
        // than skipped, because it is the edge of the guarantee.
        RC_ASSERT(!outcome->cancelled);
        RC_ASSERT(outcome->framesEncoded == planned);
        RC_ASSERT(pathExists(out));
    } else {
        RC_ASSERT(outcome->cancelled);
        RC_ASSERT(!harness.coordinator->lastError().has_value());
        // Encoding stopped at the frame boundary following the cancellation.
        RC_ASSERT(outcome->framesEncoded == static_cast<std::size_t>(cancelAt + 1));
        RC_ASSERT(outcome->framesEncoded < planned);
        // No file remains at the requested path.
        RC_ASSERT(!pathExists(out));
    }

    // The project is unmodified either way.
    RC_ASSERT(!outcome->projectModified);
    RC_ASSERT(harness.serialized() == serializedBefore);
    RC_ASSERT(harness.session.revision() == revisionBefore);
    RC_ASSERT(harness.session.modified() == modifiedBefore);
}

// ===========================================================================
// Property 39
// ===========================================================================

// Feature: end-to-end-editor-integration, Property 39: Invalid export requests are rejected before
// any file exists — for any export request violating exactly one constraint — output path longer
// than 4096 characters, a parent directory that does not exist or is not writable, an unsupported
// container or codec, a resolution, frame rate or bit rate outside its range, or an existing
// destination without an overwrite acknowledgement — the request is rejected before encoding begins,
// no file is created at the requested path, any pre-existing file at that path is byte-for-byte
// preserved, and the error names the rejected parameter together with its accepted range or
// supported values.
//
// **Validates: Requirements 7.6, 7.9, 7.11**
RC_GTEST_PROP(ExportCoordinatorProperties, InvalidExportRequestsAreRejectedBeforeAnyFileExists,
              ()) {
    enum Perturbation {
        kOverlongPath = 0,
        kMissingParent = 1,
        kUnsupportedContainer = 2,
        kUnsupportedCodec = 3,
        kResolutionOutOfRange = 4,
        kFrameRateOutOfRange = 5,
        kBitrateOutOfRange = 6,
        kExistingDestination = 7,
        kEmptyTimeline = 8,
        kUnwritableParent = 9,
    };
    // The unwritable-parent perturbation is generated only where the operating
    // system lets this process observe it: running as root bypasses the permission
    // bits entirely, and asserting a rejection that cannot happen would make the
    // property false rather than strong.
    const int perturbationCount = unwritableDirectoriesAreObservable() ? 10 : 9;
    const int perturbation = *rc::gen::inRange<int>(0, perturbationCount);

    const Resolution   resolution = kGeometries[*rc::gen::inRange<int>(0, kGeometryCount)];
    const FrameRate    fps = kFrameRates[*rc::gen::inRange<int>(0, kFrameRateCount)];
    const std::string  container = kContainers[*rc::gen::inRange<int>(0, kContainerCount)];
    const gpu::CodecId codec = kCodecs[*rc::gen::inRange<int>(0, kCodecCount)];
    const auto         bitrateKbps =
        *rc::gen::inRange<std::int64_t>(kMinExportBitrateKbps, kMaxExportBitrateKbps + 1);
    const bool includeAudio = *rc::gen::arbitrary<bool>();
    const int  frames = *rc::gen::inRange<int>(1, 5);

    CaseDir                     dir("p39");
    const std::filesystem::path out = dir.file("rejected.mp4");

    ManualClock  clock;
    ExportScript script;
    script.outputPath = out;
    script.clock = &clock;

    Harness harness;
    RC_ASSERT(harness.install(makeTimeline(frames, fps, resolution, includeAudio)));
    harness.make(hostlessOptions(&script, &clock, resolution));

    // The unperturbed request, which MUST be admissible: the rejection below is then
    // attributable to the single generated perturbation and to nothing else.
    const ExportRequest2 base = makeRequest(out, container, codec, resolution, fps, bitrateKbps,
                                            includeAudio, /*preferHardware=*/false);
    RC_ASSERT(ExportCoordinator::validate(base).isOk());
    RC_ASSERT(ExportCoordinator::validateTimeline(harness.session.engine().snapshot()).isOk());

    ExportRequest2 request = base;
    ErrorCode      expectedCode = ErrorCode::InvalidArgument;
    std::string    expectedInMessage;

    /// The bytes an existing destination must still hold afterwards.
    std::string           preExistingBytes;
    std::filesystem::path unwritableDir;

    switch (perturbation) {
        case kOverlongPath: {
            request.outputPath = dir.file(std::string(kMaxExportPathLength + 1, 'p'));
            expectedCode = ErrorCode::OutOfRange;
            expectedInMessage = std::to_string(kMaxExportPathLength);
            break;
        }
        case kMissingParent: {
            request.outputPath = dir.path() / "no_such_directory" / "out.mp4";
            expectedCode = ErrorCode::NotFound;
            expectedInMessage = "does not exist";
            break;
        }
        case kUnsupportedContainer: {
            request.container = *rc::gen::element<std::string>("avi", "flv", "ogg", "mpg", "");
            expectedCode = ErrorCode::Unsupported;
            expectedInMessage = "mp4"; // the supported containers are quoted back.
            break;
        }
        case kUnsupportedCodec: {
            request.codec =
                *rc::gen::element(gpu::CodecId::AV1, gpu::CodecId::MPEG2, gpu::CodecId::Unknown);
            expectedCode = ErrorCode::Unsupported;
            expectedInMessage = "H.264"; // the supported codecs are quoted back.
            break;
        }
        case kResolutionOutOfRange: {
            switch (*rc::gen::inRange<int>(0, 4)) {
                case 0:
                    request.resolution = Resolution{kMinExportWidth - 1, resolution.height};
                    break;
                case 1:
                    request.resolution = Resolution{kMaxExportWidth + 2, resolution.height};
                    break;
                case 2:
                    request.resolution = Resolution{resolution.width, kMinExportHeight - 1};
                    break;
                default:
                    request.resolution = Resolution{resolution.width, kMaxExportHeight + 2};
                    break;
            }
            expectedCode = ErrorCode::OutOfRange;
            expectedInMessage = "out of range";
            break;
        }
        case kFrameRateOutOfRange: {
            const bool tooFast = *rc::gen::arbitrary<bool>();
            request.frameRate = tooFast ? FrameRate{*rc::gen::inRange<std::int64_t>(121, 481), 1}
                                        : FrameRate{1, *rc::gen::inRange<std::int64_t>(2, 61)};
            expectedCode = ErrorCode::OutOfRange;
            expectedInMessage = "out of range";
            break;
        }
        case kBitrateOutOfRange: {
            const bool tooHigh = *rc::gen::arbitrary<bool>();
            request.bitrateKbps =
                tooHigh ? *rc::gen::inRange<std::int64_t>(kMaxExportBitrateKbps + 1,
                                                          kMaxExportBitrateKbps * 4)
                        : *rc::gen::inRange<std::int64_t>(0, kMinExportBitrateKbps);
            expectedCode = ErrorCode::OutOfRange;
            expectedInMessage = "out of range";
            break;
        }
        case kExistingDestination: {
            preExistingBytes =
                *rc::gen::nonEmpty(rc::gen::container<std::string>(rc::gen::arbitrary<char>()));
            std::ofstream file(request.outputPath, std::ios::binary);
            file.write(preExistingBytes.data(),
                       static_cast<std::streamsize>(preExistingBytes.size()));
            file.close();
            RC_ASSERT(pathExists(request.outputPath));
            request.overwrite = false;
            expectedCode = ErrorCode::AlreadyExists;
            expectedInMessage = "already exists";
            break;
        }
        case kEmptyTimeline: {
            // Requirement 7.6: a timeline with zero media segments. The request
            // itself stays valid, so the rejection is about the project alone.
            Project emptied = harness.session.engine().snapshot();
            for (Track& track : emptied.tracks) track.clips.clear();
            RC_ASSERT(harness.session.engine().reset(emptied).isOk());
            expectedCode = ErrorCode::FailedPrecondition;
            expectedInMessage = "empty";
            break;
        }
        default: {
            unwritableDir = dir.path() / "readonly_parent";
            std::error_code ec;
            std::filesystem::create_directories(unwritableDir, ec);
            std::filesystem::permissions(unwritableDir,
                                         std::filesystem::perms::owner_read |
                                             std::filesystem::perms::owner_exec,
                                         std::filesystem::perm_options::replace, ec);
            request.outputPath = unwritableDir / "out.mp4";
            expectedCode = ErrorCode::PermissionDenied;
            expectedInMessage = "not writable";
            break;
        }
    }

    const std::string   serializedBefore = harness.serialized();
    const std::uint64_t revisionBefore = harness.session.revision();

    // --- Rejected before anything is created ---------------------------------
    const Result<void> begun = harness.coordinator->begin(request);
    RC_ASSERT(begun.isError());
    RC_ASSERT(begun.error().code() == expectedCode);
    RC_ASSERT(begun.error().message().find(expectedInMessage) != std::string::npos);

    // The empty-timeline rejection is the one that is a fact about the PROJECT;
    // every other perturbation is also rejected by the pure static validate(),
    // which creates nothing whatsoever.
    if (perturbation == kEmptyTimeline) {
        RC_ASSERT(ExportCoordinator::validate(request).isOk());
    } else {
        const Result<void> validated = ExportCoordinator::validate(request);
        RC_ASSERT(validated.isError());
        RC_ASSERT(validated.error().code() == expectedCode);
    }

    // No worker was started and no encoder — hence no file — was ever created.
    RC_ASSERT(!harness.coordinator->running());
    RC_ASSERT(script.factoryCalls.load() == 0);
    RC_ASSERT(!script.created.load());
    RC_ASSERT(!harness.coordinator->lastOutcome().has_value());

    if (perturbation == kExistingDestination) {
        // Requirement 7.11: byte-for-byte, not merely "still there".
        RC_ASSERT(pathExists(request.outputPath));
        RC_ASSERT(readAllBytes(request.outputPath) == preExistingBytes);
        RC_ASSERT(std::filesystem::file_size(request.outputPath) == preExistingBytes.size());
        // ...and the acknowledgement is what unlocks it, so the rejection is the
        // absence of consent rather than an unusable path.
        ExportRequest2 acknowledged = request;
        acknowledged.overwrite = true;
        RC_ASSERT(ExportCoordinator::validate(acknowledged).isOk());
        RC_ASSERT(readAllBytes(request.outputPath) == preExistingBytes);
    } else {
        RC_ASSERT(!pathExists(request.outputPath));
    }

    // The project is untouched by the rejection itself.
    RC_ASSERT(harness.serialized() == serializedBefore);
    RC_ASSERT(harness.session.revision() == revisionBefore);

    if (!unwritableDir.empty()) {
        std::error_code ec;
        std::filesystem::permissions(unwritableDir, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace, ec);
    }
}

} // namespace palmier::services
