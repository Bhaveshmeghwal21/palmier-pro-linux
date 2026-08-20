// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/services/offline_mode_availability_test.cpp — the Offline_Mode
// availability sweep (task 10.9; Requirement 12.5).
//
// Requirement 12.5: "WHILE Offline_Mode applies, THE Editor_Shell SHALL keep
// timeline editing, playback, project save and open, export and the MCP_Endpoint
// available with the behaviour specified in Requirements 1 through 10, and SHALL
// surface an indication that generation is unavailable that requires no user
// dismissal and blocks no other editor command."
//
// design.md lists this as one of the surfaces that is deliberately NOT
// property-tested ("Offline-mode availability sweep — one representative
// operation from each of edit, playback, save, open, export and `tools/call`").
// Repeating those six operations over generated inputs would add cost without
// adding coverage: each of them already has its own property suite, and what is
// unproven here is not their behaviour on arbitrary input but their AVAILABILITY
// while the generative backend is the offline stub. So this file is example-based
// by design, and it asserts availability rather than re-asserting semantics.
//
// What Offline_Mode is modelled as, and why there are two of it
// ---------------------------------------------------------------------------
// The glossary defines Offline_Mode as "operation with no reachable network and
// no authenticated account". Task 10.5's registry expresses exactly that state in
// two distinguishable ways, and both are swept here because they produce
// different indications and a reader should not have to assume they behave alike:
//
//   * **Nothing configured** — the configuration names no backend, so
//     `selectGenerativeBackend` installs `offline` with NO startup error and the
//     generic "no reachable network, no authenticated account and no BYOK
//     credentials" precondition. This is the bare open-source editor.
//   * **Configured but unauthenticated** — the configuration names `hosted`, and
//     no hosted account is present. The registry falls back to `offline`
//     (Requirement 12.8), records a startup error naming the rejected id, and the
//     installed stub reports the more specific "no authenticated account".
//
// In both cases the thing standing between a `generation.generate` call and a
// socket is `GenerativeBackend::unmetPrecondition()`, which the tool hook consults
// FIRST — the same hook body the composition root installs.
//
// The six operations, and why each is the one chosen
// ---------------------------------------------------------------------------
// Every one runs through real product code over one `ProjectSession`; none is a
// hand-rolled stand-in for the operation it represents.
//
//   1. **edit**      — `timeline.add_clip` through `McpToolExecutor`. An ordinary
//                      undoable `AddClipCommand` on the real `TimelineEngine`.
//   2. **playback**  — `ui::PreviewController` play + pump: the component the
//                      composition root owns as the Requirement 1.1
//                      Playback_Engine, compositing through `gpu::Compositor` on
//                      the vendor-neutral software reference. Paced by an INJECTED
//                      `PlaybackClock` the test advances by hand, so nothing here
//                      sleeps or measures wall time.
//   3. **save**      — `project.save` through `McpToolExecutor`, writing a real
//                      `.palmier` document into a per-process temp directory.
//   4. **open**      — `project.open` through `McpToolExecutor`, reading that same
//                      document back. Save-then-open is deliberate: it makes the
//                      open leg non-vacuous (it reproduces the edit leg's clip)
//                      instead of needing a checked-in fixture.
//   5. **export**    — `timeline.export` through `McpToolExecutor`, over the real
//                      `services::makeExportToolHandler` and a real
//                      `ExportCoordinator`. The encode backend is supplied through
//                      the coordinator's own `encodeFactory` seam and writes a
//                      REAL file, exactly as `export_coordinator_test.cpp` drives
//                      it, so the export leg does not depend on a vendor encoder
//                      being present on the host. `ExportWithTheHostEncoder` below
//                      runs the same export through the PRODUCTION FFmpeg backend
//                      and skips, with a recorded reason, on a host that has no
//                      software H.264 encoder.
//   6. **tools/call**— `McpProtocolHandler`: `initialize`,
//                      `notifications/initialized`, then a `tools/call` of
//                      `timeline.add_track`. This is the MCP_Endpoint leg, so it
//                      goes through the JSON-RPC dispatcher rather than through
//                      the executor directly, and it is a MUTATING call so that
//                      "the endpoint is available" means an edit really landed.
//
// "No network request", and how that is made checkable
// ---------------------------------------------------------------------------
// `GenerativeHttpTransport` is the only member of the generative chain that can
// open a socket, so the rig installs a `ForbiddenTransport` that calls
// `ADD_FAILURE()` if it is ever asked to send. It stays installed for the whole
// life of the rig, so it covers the refusals AND the six operations.
//
// A transport that is never reached proves nothing on its own, so two things guard
// against vacuity:
//
//   * `TheTransportSeamIsLive` builds the SAME rig with the credentials present,
//     which installs the hosted client, and shows that a `generation.generate`
//     call then provably DOES reach the transport. The zero in the offline cases
//     is therefore a refusal, not a hole in the wiring.
//   * The `dlsym(RTLD_NEXT, ...)` socket interposers below count the C library's
//     socket entry points for the whole process (the technique tasks 10.5 and 10.6
//     use), and `TheInterposersObserveARealSocketCall` arms them and opens a real
//     socket so a build that failed to link them fails there rather than silently
//     reporting "no network activity". The counters are armed only around the
//     generation refusal, which is the precise claim being made; the
//     ForbiddenTransport is what covers everything else.
//
// "Requires no dismissal and blocks no other editor command"
// ---------------------------------------------------------------------------
// Stated as three checkable facts rather than as an intention:
//
//   * **Nothing to dismiss.** The indication is a VALUE that is read on demand —
//     `unmetPrecondition()` and `GenerativeBackendSelection::startupError` — not an
//     event that must be consumed. Reading it repeatedly yields the same string,
//     there is no acknowledge/clear/dismiss operation anywhere on the type, and
//     the tool surface advertises no such tool. It is still readable after the
//     sweep, so it is a standing status readout rather than a one-shot notice.
//   * **Blocks nothing.** The full six-operation sweep is run AFTER a refused
//     `generation.generate`, with NO intervening call of any kind, and every one of
//     the six still succeeds. It is run again after several consecutive refusals,
//     so a refusal cannot accumulate blocking state either.
//   * **Costs nothing.** A refusal leaves the session revision, the undo depth,
//     the media library and the clip count exactly as they were (Requirement 12.4's
//     "no media library entry, no clip and no undo-history entry"), which is what
//     makes "the next command is unaffected" true of the project and not just of
//     the return value.
//
// Nothing in this file sleeps, waits on wall time, needs a GPU, a vendor SDK, a
// sound device, Qt, or a network. Every file it writes is an absolute path inside
// a per-process temp directory.

#include <gtest/gtest.h>

#include <dlfcn.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/Clip.hpp"
#include "core/ColorSpace.hpp"
#include "core/CommandResult.hpp"
#include "core/Duration.hpp"
#include "core/Error.hpp"
#include "core/FrameRate.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/MediaManager.hpp"
#include "core/Project.hpp"
#include "core/Resolution.hpp"
#include "core/Result.hpp"
#include "core/TimelineEngine.hpp"
#include "core/Track.hpp"
#include "core/Uuid.hpp"
#include "gpu/Compositor.hpp"
#include "gpu/GpuContext.hpp"
#include "gpu/GpuTypes.hpp"
#include "media/AudioSink.hpp"
#include "media/DecoderTeardownQueue.hpp"
#include "media/ExportEngine.hpp"
#include "media/MediaEncoder.hpp"
#include "services/AuthenticationService.hpp"
#include "services/ByokCredentialManager.hpp"
#include "services/ByokCredentials.hpp"
#include "services/ExportCoordinator.hpp"
#include "services/GenerativeBackendRegistry.hpp"
#include "services/GenerativeClient.hpp"
#include "services/GenerativeHttpTransport.hpp"
#include "services/GenerativeMediaCoordinator.hpp"
#include "services/HostedGenerativeBackend.hpp"
#include "services/Json.hpp"
#include "services/McpProtocolHandler.hpp"
#include "services/McpSessionRegistry.hpp"
#include "services/McpToolExecutor.hpp"
#include "services/ProjectSession.hpp"
#include "services/SecretStore.hpp"
#include "services/ToolRegistry.hpp"
#include "ui/PreviewController.hpp"

// ===========================================================================
// Socket interposers (tasks 10.5/10.6's technique)
// ===========================================================================
//
// Ordinary strong definitions in this executable, so the dynamic linker resolves
// calls from every object in this binary to them. Each records the call while
// armed and then forwards to glibc through dlsym(RTLD_NEXT, ...), so a test that
// legitimately opens a socket still works.

namespace {

std::atomic<bool> gNetworkArmed{false};
std::atomic<std::size_t> gSocketCalls{0};

void noteSocketCall() {
    if (gNetworkArmed.load(std::memory_order_relaxed)) {
        gSocketCalls.fetch_add(1, std::memory_order_relaxed);
    }
}

/// Arms the interposers for one operation and reports what happened.
class NetworkWatch {
public:
    NetworkWatch() {
        gSocketCalls.store(0, std::memory_order_relaxed);
        gNetworkArmed.store(true, std::memory_order_relaxed);
    }
    ~NetworkWatch() { gNetworkArmed.store(false, std::memory_order_relaxed); }

    NetworkWatch(const NetworkWatch&) = delete;
    NetworkWatch& operator=(const NetworkWatch&) = delete;

    [[nodiscard]] std::size_t calls() const {
        return gSocketCalls.load(std::memory_order_relaxed);
    }
};

}  // namespace

extern "C" {

int socket(int domain, int type, int protocol) {
    noteSocketCall();
    using Fn = int (*)(int, int, int);
    static Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "socket"));
    return real ? real(domain, type, protocol) : -1;
}

int connect(int fd, const struct sockaddr* address, socklen_t length) {
    noteSocketCall();
    using Fn = int (*)(int, const struct sockaddr*, socklen_t);
    static Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "connect"));
    return real ? real(fd, address, length) : -1;
}

int getaddrinfo(const char* node, const char* service, const struct addrinfo* hints,
                struct addrinfo** result) {
    noteSocketCall();
    using Fn = int (*)(const char*, const char*, const struct addrinfo*, struct addrinfo**);
    static Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "getaddrinfo"));
    return real ? real(node, service, hints, result) : EAI_FAIL;
}

}  // extern "C"

namespace palmier::services {
namespace {

using namespace std::chrono_literals;

using ui::PlaybackClock;
using ui::PlaybackState;
using ui::PreviewController;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

constexpr const char* kGenerateTool = "generation.generate";
constexpr const char* kAddClipTool = "timeline.add_clip";
constexpr const char* kAddTrackTool = "timeline.add_track";
constexpr const char* kSaveTool = "project.save";
constexpr const char* kOpenTool = "project.open";
constexpr const char* kExportTool = "timeline.export";

/// A location, not a credential. `.invalid` is reserved by RFC 2606, so even a bug
/// that did send a request could not reach a real service.
constexpr const char* kEndpointBase = "https://generative.invalid";

/// The secret-store scope the hosted client reads its credential under.
constexpr const char* kUserScope = "default";

/// A placeholder credential value that NAMES ITSELF — both a readability aid and
/// the reason task 10.8's repository-hygiene checker reads it as a description of a
/// secret rather than as one.
constexpr const char* kStoredHostedCredential = "stored-hosted-account-token-placeholder";
constexpr const char* kStoredProviderKey = "stored-byok-provider-key-placeholder";

/// The model the non-vacuity case names.
constexpr const char* kVideoModel = "sota-video-1";

/// A small canvas: the whole point is that the software compositor and the export
/// planner run for real, not that they run at broadcast resolution.
constexpr Resolution kCanvas{320, 180};

/// Frames of seeded video, at 30 fps. Long enough that playback presents several
/// frames before reaching the end of the timeline.
constexpr int kSeedFrames = 30;

/// The bounded wait for one export. Nothing sleeps for it; it exists so a
/// coordinator that fails to finish fails a test instead of hanging.
constexpr std::chrono::milliseconds kExportBudget = 30s;

constexpr Duration ns(std::int64_t value) { return Duration::fromNanoseconds(value); }

// ---------------------------------------------------------------------------
// Scratch files
// ---------------------------------------------------------------------------

/// One temp directory per process. gtest_discover_tests runs one process per case
/// and ctest runs those in parallel, so the process id is what keeps two cases
/// from colliding on a path.
[[nodiscard]] const std::filesystem::path& scratchRoot() {
    static const std::filesystem::path root = [] {
        std::filesystem::path dir =
            std::filesystem::temp_directory_path() /
            ("palmier_offline_mode_" + std::to_string(static_cast<long long>(::getpid())));
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir;
    }();
    return root;
}

[[nodiscard]] std::filesystem::path scratchPath(const std::string& stem,
                                                const std::string& extension) {
    static std::atomic<unsigned> counter{0};
    return scratchRoot() /
           (stem + "_" + std::to_string(counter.fetch_add(1)) + extension);
}

[[nodiscard]] std::uintmax_t fileSize(const std::filesystem::path& path) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    return ec ? 0u : size;
}

// ---------------------------------------------------------------------------
// Injected clocks — the only sources of time in this file
// ---------------------------------------------------------------------------

/// The playback wall clock. Advanced explicitly by the playback leg.
class ManualPlaybackClock final : public PlaybackClock {
public:
    [[nodiscard]] Duration now() const override { return now_; }
    void advance(Duration by) noexcept { now_ += by; }

private:
    Duration now_{Duration::zero()};
};

/// The export progress-cadence clock. Shared with the export worker, so reads and
/// writes go through a mutex. Never advanced here: this file asserts nothing about
/// the ≤1 s cadence (task 9.4/9.5 own that), it only needs the cadence to be driven
/// by something other than the host's wall clock.
class ManualSteadyClock {
public:
    [[nodiscard]] media::SteadyClock fn() {
        return [this]() {
            std::lock_guard<std::mutex> lock(mutex_);
            return now_;
        };
    }

private:
    std::mutex mutex_{};
    std::chrono::steady_clock::time_point now_{};
};

// ---------------------------------------------------------------------------
// Transports
// ---------------------------------------------------------------------------

/// Fails the test if it is ever asked to send anything. Installed for the whole
/// life of an offline rig: `GenerativeHttpTransport` is the only member of the
/// generative chain that can open a socket, so "Offline_Mode issued no network
/// request" is a statement about whether this was called.
class ForbiddenTransport final : public GenerativeHttpTransport {
public:
    [[nodiscard]] Result<GenerativeHttpResponse> send(
        const GenerativeHttpRequest& request) override {
        ++sends;
        ADD_FAILURE() << "Offline_Mode reached the network seam: " << request.method << ' '
                      << request.url;
        return err<GenerativeHttpResponse>(makeError(ErrorCode::Internal, "forbidden"));
    }

    std::size_t sends = 0;
};

/// Records every request and replays scripted responses. Used only by the
/// non-vacuity case, to show the seam the offline rig never reaches is a seam the
/// same wiring provably does reach once the precondition is met.
class RecordingTransport final : public GenerativeHttpTransport {
public:
    [[nodiscard]] Result<GenerativeHttpResponse> send(
        const GenerativeHttpRequest& request) override {
        requests.push_back(request);
        if (responses.empty()) {
            return err<GenerativeHttpResponse>(
                makeError(ErrorCode::Internal, "the test script ran out of responses"));
        }
        GenerativeHttpResponse next = responses.front();
        responses.erase(responses.begin());
        return Result<GenerativeHttpResponse>(std::move(next));
    }

    std::vector<GenerativeHttpRequest> requests;
    std::vector<GenerativeHttpResponse> responses;
};

// ---------------------------------------------------------------------------
// Offline collaborators for the real auth gate
// ---------------------------------------------------------------------------

/// Accepts any well-formed BYOK credential. Validation is a seam precisely so it
/// can be answered locally; nothing here contacts a provider.
class AcceptingProviderValidator final : public ByokProviderValidator {
public:
    [[nodiscard]] Result<void> validate(const ByokCredential& credential) override {
        if (!credential.isWellFormed()) {
            return err(makeError(ErrorCode::InvalidArgument,
                                 "a BYOK credential needs a provider and a key"));
        }
        return ok();
    }
};

/// The hosted login backend is never reached: no test here logs in. Being asked to
/// authenticate would mean the gate took a route this file does not model.
class UnusedAuthBackend final : public AuthBackend {
public:
    [[nodiscard]] Result<BackendSession> authenticate(const LoginCredentials&) override {
        ADD_FAILURE() << "no test in this file authenticates";
        return err<BackendSession>(makeError(ErrorCode::Internal, "unused"));
    }
};

// ---------------------------------------------------------------------------
// The export encode backend
// ---------------------------------------------------------------------------

/// What the encode backend did, shared with the export worker.
struct EncodeRecord {
    std::mutex mutex{};
    std::filesystem::path outputPath{};
    std::atomic<int> created{0};
    std::atomic<int> videoFrames{0};
};

/// Writes a REAL file the moment it is built, exactly as the FFmpeg backend does
/// when it opens the muxer, so "the export produced a file" is checked against the
/// filesystem rather than against a return value.
class FileWritingEncodeBackend final : public media::IEncodeBackend {
public:
    explicit FileWritingEncodeBackend(EncodeRecord* record) : record_(record) {
        std::lock_guard<std::mutex> lock(record_->mutex);
        out_.open(record_->outputPath, std::ios::binary | std::ios::trunc);
        out_ << "PALMIER-OFFLINE-SWEEP-HEADER";
        out_.flush();
    }

    [[nodiscard]] Result<void> encode(const media::EncoderInputFrame&) override {
        record_->videoFrames.fetch_add(1);
        out_ << 'V';
        out_.flush();
        return ok();
    }

    [[nodiscard]] Result<void> encodeAudio(const media::EncoderInputAudio&) override {
        out_ << 'A';
        out_.flush();
        return ok();
    }

    [[nodiscard]] Result<void> finish() override {
        out_ << "PALMIER-OFFLINE-SWEEP-TRAILER";
        out_.flush();
        out_.close();
        return ok();
    }

private:
    EncodeRecord* record_;
    std::ofstream out_{};
};

[[nodiscard]] media::EncodeBackendFactory recordingEncodeFactory(EncodeRecord* record) {
    return [record](const media::EncodeSpec&, const gpu::CodecRoute&)
               -> Result<std::unique_ptr<media::IEncodeBackend>> {
        record->created.fetch_add(1);
        return std::unique_ptr<media::IEncodeBackend>(
            std::make_unique<FileWritingEncodeBackend>(record));
    };
}

// ---------------------------------------------------------------------------
// Seed project
// ---------------------------------------------------------------------------

[[nodiscard]] Clip makeClip(const MediaAssetRef& asset, Duration start, Duration length) {
    Clip clip;
    clip.id = Uuid::generateV4();
    clip.assetRef = asset;
    clip.timelineStart = start;
    clip.sourceIn = Duration::zero();
    clip.sourceOut = length;
    clip.opacity = 1.0;
    clip.gain = 1.0;
    return clip;
}

/// One video lane carrying `kSeedFrames` frames of media, one unmuted audio lane
/// over the same span, and the single asset both reference registered in
/// `Project.assets` so the saved document is one the store round-trips.
[[nodiscard]] Project makeSeedProject(Uuid& videoTrackId, Uuid& assetId) {
    const Duration span = FrameRate::fps30().durationForFrames(kSeedFrames);

    Project project;
    project.id = Uuid::generateV4();
    project.name = "offline-mode-availability";
    project.timelineFps = FrameRate::fps30();
    project.canvas = kCanvas;

    const MediaAssetRef asset(Uuid::generateV4(), "mem://offline-seed");
    project.assets.push_back(asset);

    Track video;
    video.id = Uuid::generateV4();
    video.kind = TrackKind::Video;
    video.clips.push_back(makeClip(asset, Duration::zero(), span));
    project.tracks.push_back(video);

    Track audio;
    audio.id = Uuid::generateV4();
    audio.kind = TrackKind::Audio;
    audio.clips.push_back(makeClip(asset, Duration::zero(), span));
    project.tracks.push_back(audio);

    videoTrackId = video.id;
    assetId = asset.assetId;
    return project;
}

// ---------------------------------------------------------------------------
// Project queries
// ---------------------------------------------------------------------------

[[nodiscard]] std::size_t clipCount(const Project& project) {
    std::size_t total = 0;
    for (const Track& track : project.tracks) total += track.clips.size();
    return total;
}

[[nodiscard]] std::optional<Uuid> firstVideoTrackId(const Project& project) {
    for (const Track& track : project.tracks) {
        if (track.kind == TrackKind::Video) return track.id;
    }
    return std::nullopt;
}

/// The timeline end of `trackId`, so an appended clip never overlaps and the edit
/// leg can be run repeatedly within one rig.
[[nodiscard]] Duration trackEnd(const Project& project, const Uuid& trackId) {
    Duration end = Duration::zero();
    for (const Track& track : project.tracks) {
        if (track.id != trackId) continue;
        for (const Clip& clip : track.clips) {
            if (clip.timelineEnd() > end) end = clip.timelineEnd();
        }
    }
    return end;
}

// ---------------------------------------------------------------------------
// The generate hook (mirrors app/ApplicationComposition.cpp's makeGenerateHook)
// ---------------------------------------------------------------------------
//
// The composition root's hook lives in an anonymous namespace inside
// `app/ApplicationComposition.cpp`; compiling that file in would drag the whole
// GPU/media/FFmpeg composition into this binary. The body below mirrors it
// argument for argument, including that the FIRST thing it does is ask the SELECTED
// backend for its unmet precondition — which is the mechanism Requirement 12.4
// specifies and the mechanism Requirement 12.5's "blocks no other command" rests
// on, because nothing downstream of the hook runs for a refused request.

[[nodiscard]] Tool::Handler makeGenerateHook(GenerativeMediaCoordinator& coordinator,
                                             const GenerativeBackend* backend) {
    return [&coordinator, backend](const Json& input) -> Result<Json> {
        if (backend != nullptr) {
            const std::string unmet = backend->unmetPrecondition();
            if (!unmet.empty()) {
                return err<Json>(makeError(ErrorCode::FailedPrecondition, unmet));
            }
        }

        GenerationRequest request;
        request.model = input.stringOr("model");
        request.prompt = input.stringOr("prompt");
        request.mediaType = (input.stringOr("mediaType", "video") == "image")
                                ? GenerationMediaType::Image
                                : GenerationMediaType::Video;

        GenerationPlacement placement;
        const std::optional<Uuid> trackId = Uuid::parse(input.stringOr("trackId"));
        if (!trackId.has_value()) {
            return err<Json>(makeError(ErrorCode::InvalidArgument,
                                       "generation.generate: 'trackId' must be a valid UUID"));
        }
        placement.trackId = *trackId;
        placement.framePosition = input.intOr("framePosition", 0);
        placement.sourceIn = ns(input.intOr("sourceInTicks", 0));
        placement.sourceOut = ns(input.intOr("sourceOutTicks", 0));

        Result<GeneratedMediaPlacement> placed = coordinator.generateAndPlace(request, placement);
        if (placed.isError()) return err<Json>(placed.error());

        const GeneratedMediaPlacement& result = placed.value();
        Json out = Json::object();
        out.set("assetId", result.asset.assetId.toString());
        out.set("sourcePath", result.asset.sourcePath);
        out.set("clipId", result.clipId.toString());
        out.set("timelineStartTicks", static_cast<std::int64_t>(result.timelineStart.ticks()));
        return out;
    };
}

// ---------------------------------------------------------------------------
// What the playback leg observed
// ---------------------------------------------------------------------------

struct PlaybackObservation {
    std::uint64_t presented{0};
    std::size_t frameSinkCalls{0};
    Duration playheadAfter{Duration::zero()};
    PlaybackState stateWhilePlaying{PlaybackState::Stopped};
    PlaybackState stateAfterPause{PlaybackState::Stopped};
    std::optional<Error> renderError{};
    std::uint32_t frameWidth{0};
    std::uint32_t frameHeight{0};
};

// ---------------------------------------------------------------------------
// The rig: one session, the shared tool surface, the endpoint, the export
// coordinator, the playback engine and one selected generative backend
// ---------------------------------------------------------------------------

/// How the generative backend should be configured. Both offline shapes and the
/// authorized shape are expressible, so the offline assertions and the non-vacuity
/// case are made against ONE rig rather than two arrangements that might differ.
struct BackendConfig {
    /// The configured id ("" means "configuration names nothing").
    std::string id{};
    /// Whether the credentials that id requires are present.
    bool credentialsPresent{false};
    /// The transport the registry hands the backend. Never null.
    GenerativeHttpTransport* transport{nullptr};
};

/// Members are declared in construction order; anything needing a reference to an
/// earlier member is built in the constructor body.
class OfflineRig {
public:
    explicit OfflineRig(const BackendConfig& config) {
        // A hosted credential in the store. Present in every configuration,
        // because "the store holds a value" is NOT what Offline_Mode is about:
        // Offline_Mode is the absence of an authenticated account, which the
        // registry learns from the `credentials` probe. Keeping the store stocked
        // in both shapes means the offline refusal cannot be an accident of an
        // empty store.
        const Result<void> stored = secretStore_.store(
            HostedGenerativeBackend::credentialKey(kUserScope), kStoredHostedCredential);
        EXPECT_TRUE(stored.isOk());

        // BYOK authorization for the one model every generation request in this file
        // names. This is what makes the offline refusals ATTRIBUTABLE: the
        // entitlement gate downstream of the hook would have passed the request, so
        // the only thing that can refuse it is the selected backend's unmet
        // precondition. It does not weaken Offline_Mode — a key sitting in a local
        // secret store does not make a network reachable, and what the registry
        // consults to decide whether an account is authenticated is the
        // `credentials` probe below, which reports absence.
        auth_.setByokManager(byok_);
        const Result<void> authorized =
            auth_.saveByokCredentials(ByokCredential{kVideoModel, kStoredProviderKey});
        EXPECT_TRUE(authorized.isOk());

        GenerativeBackendRequest selection;
        selection.id = config.id;
        selection.endpoint.baseUrl = kEndpointBase;
        selection.secretStore = &secretStore_;
        selection.transport = config.transport;
        selection.userId = kUserScope;
        const bool present = config.credentialsPresent;
        selection.credentials = [present](std::string_view) { return present; };
        selection_ = selectGenerativeBackend(selection);

        client_ = std::make_unique<GenerativeClient>(*selection_.backend);
        runner_ = std::make_unique<GenerativeClientRunner>(*client_);

        // The session owns the one TimelineEngine (the undo history) and the one
        // MediaManager (the media library). Seeding through reset() also clears the
        // undo history, so a baseline undo depth is the one a test built itself.
        const CommandResult seeded =
            session_.engine().reset(makeSeedProject(videoTrackId_, assetId_));
        EXPECT_TRUE(seeded.changed()) << seeded.message();

        placer_ = std::make_unique<TimelineEnginePlacer>(session_.engine());
        placer_->setMediaLibrary(session_.mediaLibrary());
        coordinator_ = std::make_unique<GenerativeMediaCoordinator>(
            gate_, *runner_, session_.mediaLibrary(), *placer_);

        // The export coordinator, over the live software context. Its encode
        // backend, clip pixels, export-local GPU context and progress clock are all
        // supplied through its OWN seams, which is what lets the export leg run on
        // a host with no GPU, no vendor SDK and no encoder.
        ExportCoordinator::Options exportOptions;
        exportOptions.clock = exportClock_.fn();
        exportOptions.encodeFactory = recordingEncodeFactory(&encodeRecord_);
        exportOptions.frameProvider = [](const Clip&, Duration) -> Result<gpu::SourceFrame> {
            return gpu::SourceFrame::solid(kCanvas.width, kCanvas.height,
                                           gpu::RgbaColor{24, 48, 72, 255});
        };
        exportOptions.gpuContextFactory = []() -> Result<gpu::GpuContext> {
            return gpu::GpuContext::softwareFallback();
        };
        exporter_ = std::make_unique<ExportCoordinator>(session_, context_, teardown_,
                                                       std::move(exportOptions));

        // The one tool surface. `generation.generate` and `timeline.export` are the
        // two hooks the composition root supplies, and both are supplied here by the
        // same product code the composition root uses.
        ToolRegistryHooks hooks;
        hooks.generate = makeGenerateHook(*coordinator_, selection_.backend.get());
        hooks.exportTimeline = makeExportToolHandler(*exporter_, session_,
                                                     ExportToolOptions{kExportBudget});
        registry_ = buildDefaultToolRegistry(&session_, std::move(hooks));
        executor_ = std::make_unique<McpToolExecutor>(registry_, &session_);
        handler_ = std::make_unique<McpProtocolHandler>(registry_, *executor_, sessions_,
                                                       inlineMainThreadInvoker());

        // The Playback_Engine: the same PreviewController the composition root owns,
        // compositing the LIVE project through the software reference.
        compositor_.setFrameProvider([](const Clip&, Duration) -> Result<gpu::SourceFrame> {
            return gpu::SourceFrame::solid(kCanvas.width, kCanvas.height,
                                           gpu::RgbaColor{24, 48, 72, 255});
        });
        preview_ = std::make_unique<PreviewController>(
            compositor_, context_, [this]() { return session_.engine().snapshot(); },
            playbackClock_);
    }

    // --- Accessors ---------------------------------------------------------

    [[nodiscard]] ProjectSession& session() noexcept { return session_; }
    [[nodiscard]] const ToolRegistry& registry() const noexcept { return registry_; }
    [[nodiscard]] McpToolExecutor& executor() noexcept { return *executor_; }
    [[nodiscard]] const GenerativeBackendSelection& selection() const noexcept {
        return selection_;
    }
    [[nodiscard]] const GenerativeBackend& backend() const noexcept {
        return *selection_.backend;
    }
    [[nodiscard]] const EncodeRecord& encodeRecord() const noexcept { return encodeRecord_; }
    [[nodiscard]] Project project() { return session_.engine().snapshot(); }
    [[nodiscard]] std::size_t undoDepth() { return session_.engine().undoDepth(); }
    [[nodiscard]] const Uuid& assetId() const noexcept { return assetId_; }

    // --- The generation request Offline_Mode must refuse -------------------

    /// A `generation.generate` request that is valid in every respect Requirement
    /// 12.9 names, so the ONLY thing that can refuse it is the unmet precondition of
    /// Requirement 12.4. A request that was invalid anyway would prove nothing about
    /// Offline_Mode.
    [[nodiscard]] Json validGenerationArguments() {
        const Project current = session_.engine().snapshot();
        const std::optional<Uuid> track = firstVideoTrackId(current);
        Json args = Json::object();
        args.set("prompt", std::string("a slow pan over a quiet harbour at dawn"));
        args.set("model", std::string(kVideoModel));
        args.set("mediaType", std::string("video"));
        args.set("trackId", track.has_value() ? track->toString() : Uuid::generateV4().toString());
        args.set("framePosition", static_cast<std::int64_t>(0));
        args.set("sourceInTicks", static_cast<std::int64_t>(0));
        args.set("sourceOutTicks", static_cast<std::int64_t>(1'000'000'000));
        return args;
    }

    [[nodiscard]] Result<Json> requestGeneration() {
        return executor_->executeTool(kGenerateTool, validGenerationArguments(),
                                      InvocationSource::Gui);
    }

    // --- The six representative operations ---------------------------------

    /// (1) edit — an ordinary undoable AddClipCommand through the executor. The clip
    /// is appended at the current end of the video lane, so the leg can be run any
    /// number of times within one rig without an overlap rejection.
    [[nodiscard]] Result<Json> edit() {
        const Project current = session_.engine().snapshot();
        const std::optional<Uuid> track = firstVideoTrackId(current);
        if (!track.has_value()) {
            return err<Json>(makeError(ErrorCode::NotFound, "the project has no video track"));
        }
        Json args = Json::object();
        args.set("trackId", track->toString());
        args.set("assetId", assetId_.toString());
        args.set("sourcePath", std::string("mem://offline-seed"));
        args.set("timelineStartNs",
                 static_cast<std::int64_t>(trackEnd(current, *track).nanoseconds()));
        args.set("sourceInNs", static_cast<std::int64_t>(0));
        args.set("sourceOutNs",
                 static_cast<std::int64_t>(FrameRate::fps30().durationForFrames(10).nanoseconds()));
        return executor_->executeTool(kAddClipTool, args, InvocationSource::Gui);
    }

    /// (2) playback — play, then advance the INJECTED clock and pump, so frames are
    /// composited and the playhead advances without any wall-clock wait.
    [[nodiscard]] PlaybackObservation playback() {
        PlaybackObservation observed;
        std::size_t sinkCalls = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        preview_->setFrameSink(
            [&sinkCalls, &width, &height](const gpu::RenderedFrame& frame, ui::RenderPath) {
                ++sinkCalls;
                width = frame.width();
                height = frame.height();
            });

        preview_->seek(Duration::zero());
        preview_->play();
        observed.stateWhilePlaying = preview_->state();

        const Duration interval = preview_->frameInterval();
        const std::uint64_t before = preview_->presentedFrameCount();
        // Counted from HERE, so the frames a seek or a transport command presents on
        // their own account are not attributed to the pump loop below.
        const std::size_t sinkCallsBefore = sinkCalls;
        // Four frame intervals: enough to present several frames, and far short of
        // the seeded timeline's length, so the leg is about presenting frames rather
        // than about the auto-stop at the end.
        for (int i = 0; i < 4; ++i) {
            playbackClock_.advance(interval);
            (void)preview_->pump();
        }

        observed.presented = preview_->presentedFrameCount() - before;
        observed.frameSinkCalls = sinkCalls - sinkCallsBefore;
        observed.playheadAfter = preview_->playhead();
        observed.renderError = preview_->lastError();

        preview_->pause();
        observed.stateAfterPause = preview_->state();
        preview_->setFrameSink({});

        observed.frameWidth = width;
        observed.frameHeight = height;
        return observed;
    }

    /// (3) save — `project.save` to an absolute path in the scratch directory.
    [[nodiscard]] Result<Json> save(const std::filesystem::path& path) {
        Json args = Json::object();
        args.set("path", path.string());
        return executor_->executeTool(kSaveTool, args, InvocationSource::Gui);
    }

    /// (4) open — `project.open` on a document this rig wrote.
    [[nodiscard]] Result<Json> open(const std::filesystem::path& path) {
        Json args = Json::object();
        args.set("path", path.string());
        return executor_->executeTool(kOpenTool, args, InvocationSource::Gui);
    }

    /// (5) export — `timeline.export` through the real tool adapter over the real
    /// coordinator. `preferHardware` is false because this host has no vendor
    /// encoder and a hardware request would be answered by a documented fallback;
    /// the fallback itself is task 9.x's subject, not this file's.
    [[nodiscard]] Result<Json> exportTimeline(const std::filesystem::path& path) {
        {
            std::lock_guard<std::mutex> lock(encodeRecord_.mutex);
            encodeRecord_.outputPath = path;
        }
        Json args = Json::object();
        args.set("outputPath", path.string());
        args.set("format", std::string("mp4"));
        args.set("codec", std::string("h264"));
        args.set("width", static_cast<std::int64_t>(kCanvas.width));
        args.set("height", static_cast<std::int64_t>(kCanvas.height));
        args.set("preferHardware", false);
        return executor_->executeTool(kExportTool, args, InvocationSource::Gui);
    }

    /// The same `timeline.export` call, through the SAME tool surface shape, but
    /// with the coordinator's encode-backend seam left at its PRODUCTION default —
    /// so the bytes are produced by the host's own encoder. A second coordinator
    /// (and a second registry/executor over the same session) rather than a
    /// reconfiguration of the first, because `ExportCoordinator::Options` is fixed
    /// at construction and the sweep's coordinator must keep its injected factory.
    [[nodiscard]] Result<Json> exportWithHostEncoder(const std::filesystem::path& path) {
        ExportCoordinator::Options options;
        // Only the pixel source and the export-local GPU context are injected, and
        // neither of them is an encoder: `encodeFactory` is left empty, which is what
        // makes this the production FFmpeg path.
        options.frameProvider = [](const Clip&, Duration) -> Result<gpu::SourceFrame> {
            return gpu::SourceFrame::solid(kCanvas.width, kCanvas.height,
                                           gpu::RgbaColor{24, 48, 72, 255});
        };
        options.gpuContextFactory = []() -> Result<gpu::GpuContext> {
            return gpu::GpuContext::softwareFallback();
        };
        ExportCoordinator hostExporter(session_, context_, teardown_, std::move(options));

        ToolRegistryHooks hooks;
        hooks.exportTimeline =
            makeExportToolHandler(hostExporter, session_, ExportToolOptions{kExportBudget});
        const ToolRegistry hostRegistry = buildDefaultToolRegistry(&session_, std::move(hooks));
        McpToolExecutor hostExecutor(hostRegistry, &session_);

        Json args = Json::object();
        args.set("outputPath", path.string());
        args.set("format", std::string("mp4"));
        args.set("codec", std::string("h264"));
        args.set("width", static_cast<std::int64_t>(kCanvas.width));
        args.set("height", static_cast<std::int64_t>(kCanvas.height));
        args.set("preferHardware", false);
        // Video only: this case is about reaching a real video encoder, and requiring
        // an AAC encoder as well would make it skip on more hosts for a reason that
        // has nothing to do with Requirement 12.5.
        args.set("includeAudio", false);
        return hostExecutor.executeTool(kExportTool, args, InvocationSource::Gui);
    }

    /// (6) tools/call — the MCP_Endpoint leg: initialize, then a MUTATING
    /// `tools/call`, so "the endpoint is available" means an edit really landed.
    struct ToolsCallResult {
        int httpStatus{0};
        bool isError{true};
        std::string text{};
        Json envelope{};
    };

    [[nodiscard]] ToolsCallResult toolsCallAddTrack() {
        McpRequestContext initContext;
        initContext.sourceAddress = "127.0.0.1";
        Json initParams = Json::object();
        initParams.set("protocolVersion",
                       std::string(McpProtocolHandler::latestProtocolVersion()));
        Json initRequest = Json::object();
        initRequest.set("jsonrpc", std::string("2.0"));
        initRequest.set("id", static_cast<std::int64_t>(1));
        initRequest.set("method", std::string("initialize"));
        initRequest.set("params", std::move(initParams));

        const McpReply initialized = handler_->handle(initContext, initRequest.dump());
        ToolsCallResult result;
        if (initialized.httpStatus != 200 || !initialized.newSessionId.has_value()) {
            result.httpStatus = initialized.httpStatus;
            result.text = "initialize failed: " + initialized.body;
            return result;
        }
        const std::string sessionId = *initialized.newSessionId;

        McpRequestContext sessionContext;
        sessionContext.sourceAddress = "127.0.0.1";
        sessionContext.sessionId = sessionId;
        const McpReply notified = handler_->handle(
            sessionContext, R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
        if (notified.httpStatus != 202) {
            result.httpStatus = notified.httpStatus;
            result.text = "notifications/initialized failed: " + notified.body;
            return result;
        }

        Json callArguments = Json::object();
        callArguments.set("kind", std::string("audio"));
        Json callParams = Json::object();
        callParams.set("name", std::string(kAddTrackTool));
        callParams.set("arguments", std::move(callArguments));
        Json callRequest = Json::object();
        callRequest.set("jsonrpc", std::string("2.0"));
        callRequest.set("id", static_cast<std::int64_t>(2));
        callRequest.set("method", std::string("tools/call"));
        callRequest.set("params", std::move(callParams));

        const McpReply called = handler_->handle(sessionContext, callRequest.dump());
        result.httpStatus = called.httpStatus;
        Result<Json> parsed = Json::parse(called.body);
        if (parsed.isError()) {
            result.text = "reply was not JSON: " + called.body;
            return result;
        }
        result.envelope = std::move(parsed).value();
        const Json* toolResult = result.envelope.find("result");
        if (toolResult == nullptr) {
            result.text = "reply carried no result: " + called.body;
            return result;
        }
        result.isError = toolResult->boolOr("isError", true);
        if (const Json* content = toolResult->find("content");
            content != nullptr && content->isArray() && !content->asArray().empty()) {
            result.text = content->asArray().front().stringOr("text");
        }
        return result;
    }

private:
    // Generative collaborators.
    UnusedAuthBackend authBackend_;
    AcceptingProviderValidator validator_;
    InMemorySecretStore secretStore_;
    ByokCredentialManager byok_{validator_, secretStore_, kUserScope};
    AuthenticationService auth_{authBackend_};
    AuthServiceGenerationGate gate_{auth_};
    GenerativeBackendSelection selection_;
    std::unique_ptr<GenerativeClient> client_;
    std::unique_ptr<GenerativeClientRunner> runner_;

    // The one project.
    ProjectSession session_;
    Uuid videoTrackId_{};
    Uuid assetId_{};
    std::unique_ptr<TimelineEnginePlacer> placer_;
    std::unique_ptr<GenerativeMediaCoordinator> coordinator_;

    // Export.
    gpu::GpuContext context_{gpu::GpuContext::softwareFallback()};
    media::DecoderTeardownQueue teardown_{};
    ManualSteadyClock exportClock_{};
    EncodeRecord encodeRecord_{};
    std::unique_ptr<ExportCoordinator> exporter_;

    // The shared surface and the endpoint.
    ToolRegistry registry_;
    std::unique_ptr<McpToolExecutor> executor_;
    McpSessionRegistry sessions_{};
    std::unique_ptr<McpProtocolHandler> handler_;

    // Playback.
    gpu::Compositor compositor_{context_};
    ManualPlaybackClock playbackClock_{};
    std::unique_ptr<PreviewController> preview_;
};

// ---------------------------------------------------------------------------
// The sweep
// ---------------------------------------------------------------------------

/// Run all six representative operations against `rig` and assert every one of
/// them succeeded. `label` identifies the sweep in a failure message, because the
/// same helper is used before and after a refusal and the two must be
/// distinguishable.
///
/// Written as a helper taking the rig so that "the same six operations, again,
/// with nothing done in between" is literally the same code rather than a second
/// transcription of it.
void expectEverySurfaceAvailable(OfflineRig& rig, const std::string& label) {
    SCOPED_TRACE("offline-mode sweep: " + label);

    // (1) edit -------------------------------------------------------------
    const std::size_t clipsBefore = clipCount(rig.project());
    const std::size_t undoBefore = rig.undoDepth();
    const Result<Json> edited = rig.edit();
    ASSERT_TRUE(edited.isOk()) << "edit: " << edited.error().toString();
    EXPECT_EQ(clipCount(rig.project()), clipsBefore + 1);
    EXPECT_EQ(rig.undoDepth(), undoBefore + 1) << "the edit must be undoable";

    // (2) playback ---------------------------------------------------------
    const PlaybackObservation played = rig.playback();
    EXPECT_FALSE(played.renderError.has_value())
        << "playback: " << (played.renderError.has_value()
                                ? played.renderError->toString()
                                : std::string{});
    EXPECT_GE(played.presented, 1u) << "playback presented no frame";
    EXPECT_EQ(played.frameSinkCalls, played.presented)
        << "every presented frame must reach the surface";
    EXPECT_GT(played.playheadAfter.nanoseconds(), 0)
        << "playback did not advance the playhead";
    EXPECT_EQ(played.stateWhilePlaying, PlaybackState::Playing);
    EXPECT_EQ(played.stateAfterPause, PlaybackState::Paused);
    EXPECT_EQ(played.frameWidth, kCanvas.width);
    EXPECT_EQ(played.frameHeight, kCanvas.height);

    // (3) save -------------------------------------------------------------
    const std::filesystem::path document = scratchPath("offline_" + label, ".palmier");
    const Result<Json> saved = rig.save(document);
    ASSERT_TRUE(saved.isOk()) << "save: " << saved.error().toString();
    EXPECT_EQ(saved.value().stringOr("documentPath"), document.string());
    EXPECT_GT(saved.value().intOr("bytesWritten"), 0);
    ASSERT_TRUE(std::filesystem::exists(document));
    EXPECT_GT(fileSize(document), 0u);

    // (4) open -------------------------------------------------------------
    // The saved document carries the edit made above, so a successful open is
    // observable as the clip count coming back rather than as a bare ok().
    const std::size_t clipsSaved = clipCount(rig.project());
    const Result<Json> opened = rig.open(document);
    ASSERT_TRUE(opened.isOk()) << "open: " << opened.error().toString();
    EXPECT_EQ(clipCount(rig.project()), clipsSaved);
    EXPECT_TRUE(rig.session().documentPath().has_value());
    EXPECT_EQ(*rig.session().documentPath(), document);
    EXPECT_FALSE(rig.session().modified()) << "a freshly opened project is unmodified";

    // (5) export -----------------------------------------------------------
    const std::filesystem::path output = scratchPath("offline_" + label, ".mp4");
    const int encodersBefore = rig.encodeRecord().created.load();
    const Result<Json> exported = rig.exportTimeline(output);
    ASSERT_TRUE(exported.isOk()) << "export: " << exported.error().toString();
    EXPECT_EQ(exported.value().stringOr("outputPath"), output.string());
    EXPECT_GT(exported.value().intOr("framesEncoded"), 0);
    EXPECT_EQ(exported.value().intOr("framesEncoded"),
              exported.value().intOr("plannedFrames"));
    EXPECT_FALSE(exported.value().boolOr("projectModified", true));
    EXPECT_EQ(rig.encodeRecord().created.load(), encodersBefore + 1);
    ASSERT_TRUE(std::filesystem::exists(output));
    EXPECT_GT(fileSize(output), 0u);

    // (6) tools/call -------------------------------------------------------
    const std::size_t tracksBefore = rig.project().tracks.size();
    const OfflineRig::ToolsCallResult called = rig.toolsCallAddTrack();
    EXPECT_EQ(called.httpStatus, 200);
    EXPECT_FALSE(called.isError) << "tools/call: " << called.text;
    EXPECT_EQ(rig.project().tracks.size(), tracksBefore + 1)
        << "the tools/call edit did not reach the project";
}

// ---------------------------------------------------------------------------
// The two Offline_Mode configurations
// ---------------------------------------------------------------------------

/// Configuration names nothing: `offline` is installed as the default, with no
/// startup error and the generic precondition.
[[nodiscard]] BackendConfig nothingConfigured(ForbiddenTransport& transport) {
    BackendConfig config;
    config.id.clear();
    config.credentialsPresent = false;
    config.transport = &transport;
    return config;
}

/// Configuration names `hosted` and no hosted account is present: the registry
/// falls back to `offline` and records the startup error of Requirement 12.8.
[[nodiscard]] BackendConfig hostedWithoutAnAccount(ForbiddenTransport& transport) {
    BackendConfig config;
    config.id = std::string(kGenerativeBackendHosted);
    config.credentialsPresent = false;
    config.transport = &transport;
    return config;
}

// ---------------------------------------------------------------------------
// Host-encoder availability
// ---------------------------------------------------------------------------

/// Why this host cannot produce H.264 bytes through the production encode path, or
/// nullopt when it can. Asked through the PRODUCT's own entry point
/// (`media::MediaEncoder::create` with the default FFmpeg backend), so the answer
/// is the encoder the export would really have used rather than a guess about the
/// build. A build with no FFmpeg encode support reports FailedPrecondition here; a
/// build with FFmpeg but no H.264 encoder reports the backend's own error.
[[nodiscard]] std::optional<std::string> hostSoftwareEncoderSkipReason() {
    const std::filesystem::path probe = scratchPath("encoder_probe", ".mp4");
    media::EncodeSpec spec;
    spec.codec = gpu::CodecId::H264;
    spec.bitrateBitsPerSecond = 1'000'000;
    spec.resolution = kCanvas;
    spec.frameRate = FrameRate::fps30();
    spec.preferHardware = false;
    spec.caps = gpu::GpuCaps::software();
    spec.outputPath = probe;
    spec.containerFormat = "mp4";

    Result<media::MediaEncoder> encoder = media::MediaEncoder::create(spec);
    std::optional<std::string> reason;
    if (encoder.isError()) {
        reason = "no software H.264 encoder is available through the production encode "
                 "path on this host: " +
                 encoder.error().toString();
    } else {
        // Release the muxer's handle before the probe file is removed.
        (void)encoder.value().finish();
    }
    std::error_code ec;
    std::filesystem::remove(probe, ec);
    return reason;
}

// ===========================================================================
// Non-vacuity
// ===========================================================================

TEST(OfflineModeNetworkSeam, TheInterposersObserveARealSocketCall) {
    // Guards the zero-count assertion below. If the interposers were not linked
    // into this binary, "no socket was opened" would be true of a test that opened
    // one. Arming and then opening a real socket must be seen, and the interposer
    // must forward to glibc.
    NetworkWatch watch;
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_GE(fd, 0) << "the interposer must forward to the real socket()";
    if (fd >= 0) ::close(fd);
    EXPECT_GE(watch.calls(), 1u);
}

TEST(OfflineModeNetworkSeam, TheTransportSeamIsLive) {
    // The offline cases assert that the transport was never asked to send. That is
    // only meaningful if the SAME wiring does reach it once the precondition is
    // met, so here the credentials are present, the hosted client is installed, and
    // the identical `generation.generate` call provably reaches the seam.
    RecordingTransport transport;
    transport.responses.push_back({201, R"({"id":"job-offline-sweep-liveness"})"});

    BackendConfig config;
    config.id = std::string(kGenerativeBackendHosted);
    config.credentialsPresent = true;
    config.transport = &transport;

    OfflineRig rig(config);
    ASSERT_EQ(rig.selection().id, "hosted");
    EXPECT_TRUE(rig.selection().startupError.empty()) << rig.selection().startupError;
    // The precondition is MET, so the hook does not short-circuit.
    EXPECT_TRUE(rig.backend().unmetPrecondition().empty());

    // The request is submitted; the script then runs out, so the job fails on its
    // first poll. That is fine and deliberate: what is under test here is that the
    // seam was REACHED, not that the job succeeded.
    const Result<Json> generated = rig.requestGeneration();
    EXPECT_TRUE(generated.isError())
        << "the scripted transport supplies no poll response, so the job cannot succeed";
    EXPECT_GE(transport.requests.size(), 1u)
        << "an authorized generation request must reach the transport seam";
}

// ===========================================================================
// Requirement 12.5 — the availability sweep
// ===========================================================================

TEST(OfflineModeAvailability, EverySurfaceIsAvailableWithNoBackendConfigured) {
    ForbiddenTransport transport;
    OfflineRig rig(nothingConfigured(transport));

    // Offline_Mode really is in force, by the registry's own account.
    ASSERT_EQ(rig.selection().id, std::string(kGenerativeBackendOffline));
    ASSERT_FALSE(rig.backend().unmetPrecondition().empty());

    expectEverySurfaceAvailable(rig, "nothing_configured");
    EXPECT_EQ(transport.sends, 0u) << "Offline_Mode must issue no network request";
}

TEST(OfflineModeAvailability, EverySurfaceIsAvailableWithHostedConfiguredButUnauthenticated) {
    ForbiddenTransport transport;
    OfflineRig rig(hostedWithoutAnAccount(transport));

    // The fallback of Requirement 12.8 happened, and the startup error names the
    // rejected id — which is the other way Offline_Mode presents itself.
    ASSERT_EQ(rig.selection().id, std::string(kGenerativeBackendOffline));
    ASSERT_TRUE(rig.selection().fellBack());
    EXPECT_NE(rig.selection().startupError.find("hosted"), std::string::npos);

    expectEverySurfaceAvailable(rig, "hosted_unauthenticated");
    EXPECT_EQ(transport.sends, 0u) << "Offline_Mode must issue no network request";
}

// ===========================================================================
// Requirement 12.5 — the indication needs no dismissal and blocks nothing
// ===========================================================================

TEST(OfflineModeAvailability, TheGenerationUnavailableIndicationRequiresNoDismissal) {
    ForbiddenTransport transport;
    OfflineRig rig(hostedWithoutAnAccount(transport));

    // The indication is a VALUE read on demand, not an event that must be consumed:
    // reading it repeatedly yields the same string, so there is nothing that could
    // be "used up" by being displayed.
    const std::string first = rig.backend().unmetPrecondition();
    ASSERT_FALSE(first.empty());
    EXPECT_EQ(rig.backend().unmetPrecondition(), first);
    EXPECT_EQ(rig.backend().unmetPrecondition(), first);

    // It names the unmet precondition rather than merely reporting a failure, so it
    // is usable as the surfaced indication verbatim.
    EXPECT_NE(first.find("generation is unavailable"), std::string::npos) << first;
    EXPECT_NE(first.find("no authenticated account"), std::string::npos) << first;

    // There is no dismissal, acknowledgement or clearing operation anywhere on the
    // tool surface — a dismissible notice would have to be dismissed through one.
    for (const std::string_view forbidden :
         {"generation.dismiss", "generation.acknowledge", "generation.clear",
          "notice.dismiss", "notification.dismiss", "notice.acknowledge"}) {
        EXPECT_EQ(rig.registry().find(forbidden), nullptr)
            << "the tool surface advertises a dismissal operation: " << forbidden;
    }

    // The full sweep runs, and the indication is STILL readable afterwards and
    // unchanged: it is a standing status readout, not a one-shot notice that the
    // act of carrying on clears.
    expectEverySurfaceAvailable(rig, "indication_persists");
    EXPECT_EQ(rig.backend().unmetPrecondition(), first);
    EXPECT_EQ(transport.sends, 0u);
}

TEST(OfflineModeAvailability, ARefusedGenerationBlocksNoOtherCommand) {
    ForbiddenTransport transport;
    OfflineRig rig(nothingConfigured(transport));

    const std::size_t clipsBefore = clipCount(rig.project());
    const std::size_t undoBefore = rig.undoDepth();
    const std::uint64_t revisionBefore = rig.session().revision();
    const std::size_t libraryBefore = rig.session().mediaLibrary().assetCount();

    // The refusal. Requirement 12.4's wording is asserted only as far as
    // Requirement 12.5 depends on it: the request is refused, the precondition is
    // named, and nothing was added — which is what makes the next command
    // unaffected in the project as well as in the return value.
    {
        NetworkWatch watch;
        const Result<Json> refused = rig.requestGeneration();
        ASSERT_TRUE(refused.isError()) << "Offline_Mode must refuse generation";
        EXPECT_EQ(refused.error().code(), ErrorCode::FailedPrecondition);
        EXPECT_NE(refused.error().message().find("generation is unavailable"),
                  std::string::npos)
            << refused.error().message();
        EXPECT_EQ(watch.calls(), 0u) << "the refusal must open no socket";
    }
    EXPECT_EQ(transport.sends, 0u);
    EXPECT_EQ(clipCount(rig.project()), clipsBefore);
    EXPECT_EQ(rig.undoDepth(), undoBefore);
    EXPECT_EQ(rig.session().revision(), revisionBefore);
    EXPECT_EQ(rig.session().mediaLibrary().assetCount(), libraryBefore);

    // Now the whole sweep, with NOTHING done in between — no dismissal, no
    // acknowledgement, no reset. This is the "blocks no other editor command"
    // clause: all six surfaces still answer.
    expectEverySurfaceAvailable(rig, "after_one_refusal");

    // And a refusal cannot accumulate blocking state either: several more of them,
    // back to back, then the same sweep again.
    for (int i = 0; i < 5; ++i) {
        const Result<Json> refused = rig.requestGeneration();
        ASSERT_TRUE(refused.isError()) << "refusal " << i;
        EXPECT_EQ(refused.error().code(), ErrorCode::FailedPrecondition);
    }
    expectEverySurfaceAvailable(rig, "after_repeated_refusals");

    EXPECT_EQ(transport.sends, 0u) << "Offline_Mode must issue no network request";
}

TEST(OfflineModeAvailability, ARefusalThroughTheEndpointAlsoBlocksNothing) {
    // The same claim for the MCP_Endpoint, because Requirement 12.1 routes the
    // endpoint's `generation.generate` through the same backend and Requirement
    // 12.5 keeps the endpoint available: a refusal arrives as an ordinary
    // `isError` tool result on a session that stays usable for the next call.
    ForbiddenTransport transport;
    OfflineRig rig(nothingConfigured(transport));

    const std::size_t clipsBefore = clipCount(rig.project());
    const std::size_t undoBefore = rig.undoDepth();

    const Result<Json> refused = rig.requestGeneration();
    ASSERT_TRUE(refused.isError());
    EXPECT_EQ(clipCount(rig.project()), clipsBefore);
    EXPECT_EQ(rig.undoDepth(), undoBefore);

    // A fresh endpoint session, opened AFTER the refusal, still initializes and
    // still performs a mutating tools/call.
    const OfflineRig::ToolsCallResult called = rig.toolsCallAddTrack();
    EXPECT_EQ(called.httpStatus, 200);
    EXPECT_FALSE(called.isError) << called.text;

    // And `generation.generate` is still ADVERTISED rather than withdrawn: the tool
    // surface is identical offline, which is what keeps the endpoint's tool list
    // stable across configurations (Requirement 12.5's "available with the
    // behaviour specified in Requirements 1 through 10").
    EXPECT_NE(rig.registry().find(kGenerateTool), nullptr);
    EXPECT_EQ(transport.sends, 0u);
}

// ===========================================================================
// The same export through the host's own encoder
// ===========================================================================

TEST(OfflineModeAvailability, ExportWithTheHostEncoder) {
    // The sweep's export leg supplies the encode backend through the coordinator's
    // own `encodeFactory` seam, so it runs on any host. This case removes that seam
    // and runs the SAME `timeline.export` call through the PRODUCTION FFmpeg encode
    // backend, which is the only way to show that offline export reaches a real
    // encoder rather than only a test one.
    //
    // It therefore depends on the host, and it says so rather than asserting
    // something weaker: with no software H.264 encoder available it SKIPS with a
    // recorded reason, exactly as the hardware-versus-software comparison
    // (task 9.8) skips when no vendor encoder is present.
    ForbiddenTransport transport;
    OfflineRig rig(nothingConfigured(transport));
    ASSERT_EQ(rig.selection().id, std::string(kGenerativeBackendOffline));

    if (const std::optional<std::string> reason = hostSoftwareEncoderSkipReason();
        reason.has_value()) {
        GTEST_SKIP() << *reason;
    }

    const std::filesystem::path output = scratchPath("offline_host_encoder", ".mp4");
    const Result<Json> exported = rig.exportWithHostEncoder(output);
    ASSERT_TRUE(exported.isOk()) << "export: " << exported.error().toString();
    EXPECT_EQ(exported.value().stringOr("outputPath"), output.string());
    EXPECT_GT(exported.value().intOr("framesEncoded"), 0);
    EXPECT_EQ(exported.value().intOr("framesEncoded"),
              exported.value().intOr("plannedFrames"));
    EXPECT_FALSE(exported.value().boolOr("usedHardwareEncode", true))
        << "preferHardware was false, so a software encoder was asked for";
    EXPECT_FALSE(exported.value().boolOr("projectModified", true));

    // A real encoder wrote a real file.
    ASSERT_TRUE(std::filesystem::exists(output));
    EXPECT_GT(fileSize(output), 0u);

    EXPECT_EQ(transport.sends, 0u) << "Offline_Mode must issue no network request";
}

}  // namespace
}  // namespace palmier::services
