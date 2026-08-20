// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/services/generative_lifecycle_property_test.cpp — the generation
// lifecycle properties (task 10.6; Requirements 12.3, 12.9).
//
//   * Property 65: A successful generation is one undoable edit
//     — Validates: Requirements 12.3
//   * Property 66: Invalid generation requests never reach the network
//     — Validates: Requirements 12.9
//
// What is under test, and why it is assembled this way
// ---------------------------------------------------
// Both properties are stated about the ONE path every generation request takes.
// Requirement 12.1 says the tool surface, the MCP endpoint and the in-app agent
// all dispatch `generation.generate` through the same registry, and task 10.5
// added the backend the hook routes through. So the rig below is that whole path,
// assembled out of product code:
//
//     McpToolExecutor  (declared-schema validation, then rollback policy)
//       -> ToolRegistry `generation.generate` (the real declared schema)
//         -> the generate hook (GenerativeBackend::unmetPrecondition() first)
//           -> GenerativeMediaCoordinator (prompt + source-range validation)
//             -> AuthServiceGenerationGate over a real AuthenticationService
//               -> GenerativeClientRunner -> GenerativeClient
//                 -> the selected GenerativeBackend (task 10.5's registry)
//                   -> GenerativeHttpTransport   <-- the ONLY route to a socket
//
// The hook is the one part written here rather than reused: the composition
// root's `makeGenerateHook` lives in an anonymous namespace inside
// `app/ApplicationComposition.cpp`, and compiling that file in would drag the
// GPU, media and FFmpeg stacks into a service-layer test binary. The body below
// mirrors it argument for argument, including that the FIRST thing it does is ask
// the selected backend for its unmet precondition.
//
// The transport is the seam. `GenerativeHttpTransport` is the only member of that
// chain that can open a socket, so "this request never reached the network" is a
// statement about whether it was called — which is why Property 66 installs a
// `ForbiddenTransport` that calls `ADD_FAILURE()` if it is asked to send
// anything, and additionally counts the C library's socket entry points through
// the `dlsym(RTLD_NEXT, ...)` interposers below (the same technique task 10.5's
// suite and the offline interpreter suite use). Two mechanisms rather than one,
// because they fail for different reasons: the ForbiddenTransport catches a
// request that went through the seam, and the interposers catch one that went
// around it.
//
// Neither zero-count is allowed to be vacuous:
//
//   * `TheInterposersObserveARealSocketCall` arms the counters and opens a real
//     socket, so a build where the interposers were not linked in fails there
//     rather than silently reporting "no network activity" everywhere.
//   * Property 65 asserts the exchange count on its ScriptedTransport (submit +
//     every poll + fetch). The same wiring that must NOT call the transport in
//     Property 66 provably DOES call it for a valid request in Property 65, so
//     the seam is live and Property 66's zero is a refusal rather than a hole.
//
// What Property 66 covers for the track-identifier argument
// ------------------------------------------------------------
// Requirement 12.9 lists five offending arguments, and this file asserts all
// five: an absent or out-of-length prompt, a media kind outside {video, image},
// a negative timeline position, a malformed source range ("out-of-range
// duration"), and — as of the source change that added
// ITimelinePlacement::trackExists() — a track identifier absent from the
// current project. `GenerativeMediaCoordinator::generateAndPlace` now checks
// trackExists() immediately after the source-range check and BEFORE the
// entitlement gate, the runner, or the media library are ever reached, so an
// unknown (but syntactically valid) trackId is refused before submission in
// exactly the same way as the four schema-level cases. The malformed-UUID form
// is asserted separately below (refused by the declared schema, before dispatch
// even reaches the coordinator).
//
// No test in this file contacts a network, needs an endpoint, or writes a file.

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <dlfcn.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/Clip.hpp"
#include "core/CommandResult.hpp"
#include "core/ColorSpace.hpp"
#include "core/Duration.hpp"
#include "core/EditCommand.hpp"
#include "core/EditCommands.hpp"
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
#include "services/AuthenticationService.hpp"
#include "services/ByokCredentialManager.hpp"
#include "services/ByokCredentials.hpp"
#include "services/GenerationModelCatalog.hpp"
#include "services/GenerativeBackendRegistry.hpp"
#include "services/GenerativeClient.hpp"
#include "services/GenerativeHttpTransport.hpp"
#include "services/GenerativeMediaCoordinator.hpp"
#include "services/HostedGenerativeBackend.hpp"
#include "services/Json.hpp"
#include "services/McpToolExecutor.hpp"
#include "services/ProjectSession.hpp"
#include "services/ProjectStore.hpp"
#include "services/SecretStore.hpp"
#include "services/ToolRegistry.hpp"

// ===========================================================================
// Socket interposers (the "no network" proof, task 10.5's technique)
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

ssize_t sendto(int fd, const void* buffer, size_t length, int flags,
               const struct sockaddr* address, socklen_t addressLength) {
    noteSocketCall();
    using Fn = ssize_t (*)(int, const void*, size_t, int, const struct sockaddr*, socklen_t);
    static Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "sendto"));
    return real ? real(fd, buffer, length, flags, address, addressLength) : -1;
}

}  // extern "C"

namespace palmier {
namespace {

using services::AuthBackend;
using services::AuthServiceGenerationGate;
using services::AuthenticationService;
using services::BackendSession;
using services::ByokCredential;
using services::ByokCredentialManager;
using services::ByokProviderValidator;
using services::CatalogModel;
using services::GeneratedMediaPlacement;
using services::GenerationMediaType;
using services::GenerationModelCatalog;
using services::GenerationPlacement;
using services::GenerationRequest;
using services::GenerativeBackend;
using services::GenerativeBackendRequest;
using services::GenerativeBackendSelection;
using services::GenerativeClient;
using services::GenerativeClientRunner;
using services::GenerativeEndpoint;
using services::GenerativeHttpRequest;
using services::GenerativeHttpResponse;
using services::GenerativeHttpTransport;
using services::GenerativeMediaCoordinator;
using services::HostedGenerativeBackend;
using services::InMemorySecretStore;
using services::InvocationSource;
using services::Json;
using services::LoginCredentials;
using services::McpToolExecutor;
using services::ProjectSession;
using services::selectGenerativeBackend;
using services::serializeProject;
using services::TimelineEnginePlacer;
using services::Tool;
using services::ToolRegistry;
using services::ToolRegistryHooks;

constexpr const char* kGenerateTool = "generation.generate";

/// A location, not a credential. `.invalid` is reserved by RFC 2606, so even a
/// bug that did send a request could not reach a real service.
constexpr const char* kEndpointBase = "https://generative.invalid";

/// The secret-store scope the hosted client reads its credential under.
constexpr const char* kUserScope = "default";

/// Placeholder credential values, each NAMING ITSELF — both a readability aid and
/// the reason task 10.8's repository-hygiene checker reads them as descriptions of
/// a secret rather than as one.
constexpr const char* kStoredHostedCredential = "stored-hosted-account-token-placeholder";
constexpr const char* kStoredProviderKey = "stored-byok-provider-key-placeholder";

/// The two models the rig's BYOK credentials authorize. The gate derives the
/// provider from the request's model id, which is what makes an EMPTY model a
/// refusal rather than an unauthenticated round trip.
constexpr const char* kVideoModel = "sota-video-1";
constexpr const char* kImageModel = "sota-image-1";

constexpr Duration ms(std::int64_t value) { return Duration::fromMilliseconds(value); }

// ---------------------------------------------------------------------------
// Transports
// ---------------------------------------------------------------------------

/// Replays scripted responses and records every request. This is the point of the
/// seam: the real client is driven to a successful completion with no endpoint, no
/// TLS and no socket.
class ScriptedTransport final : public GenerativeHttpTransport {
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

/// Fails the test if it is ever asked to send anything. Installed wherever a
/// "this must not reach the network" claim is being checked. `sends` is kept so a
/// property can assert the count as well as rely on the failure, which keeps the
/// diagnostic useful when RapidCheck shrinks.
class ForbiddenTransport final : public GenerativeHttpTransport {
public:
    [[nodiscard]] Result<GenerativeHttpResponse> send(
        const GenerativeHttpRequest& request) override {
        ++sends;
        ADD_FAILURE() << "an invalid generation request reached the network seam: "
                      << request.method << ' ' << request.url;
        return err<GenerativeHttpResponse>(makeError(ErrorCode::Internal, "forbidden"));
    }

    std::size_t sends = 0;
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

/// The hosted login backend is never reached: the rig authorizes through BYOK, so
/// `login()` is never called. Being asked to authenticate would mean the gate
/// took a route this test does not model, so it fails the test.
class UnusedAuthBackend final : public AuthBackend {
public:
    [[nodiscard]] Result<BackendSession> authenticate(const LoginCredentials&) override {
        ADD_FAILURE() << "the rig authorizes through BYOK and must never log in";
        return err<BackendSession>(makeError(ErrorCode::Internal, "unused"));
    }
};

// ---------------------------------------------------------------------------
// Seed project
// ---------------------------------------------------------------------------

Clip makeClip(ClipId id, const MediaAssetRef& asset, Duration start, Duration in,
              Duration out) {
    Clip clip;
    clip.id = id;
    clip.assetRef = asset;
    clip.timelineStart = start;
    clip.sourceIn = in;
    clip.sourceOut = out;
    return clip;
}

/// A valid starting project: `videoTracks` video lanes each seeded with one clip,
/// one audio lane, and one EMPTY video lane at the back which is the generation
/// target. The target is empty so that any in-range position is a legal placement
/// and the property is about the undo semantics rather than about clip packing.
/// Every seeded clip's asset is registered in `Project.assets`, so the serialized
/// snapshot Property 65 compares is a document the store would accept.
Project makeSeedProject(int videoTracks) {
    Project project;
    project.id = Uuid::generateV4();
    project.name = "generative-lifecycle";
    project.timelineFps = FrameRate::fps30();
    project.canvas = Resolution::hd1080();

    const auto addSeededTrack = [&project](TrackKind kind) {
        Track track;
        track.id = Uuid::generateV4();
        track.kind = kind;
        const MediaAssetRef asset(Uuid::generateV4(), "mem://seed");
        project.assets.push_back(asset);
        track.clips.push_back(makeClip(Uuid::generateV4(), asset, ms(0), ms(0), ms(1000)));
        project.tracks.push_back(std::move(track));
    };

    for (int i = 0; i < videoTracks; ++i) addSeededTrack(TrackKind::Video);
    addSeededTrack(TrackKind::Audio);

    Track target;
    target.id = Uuid::generateV4();
    target.kind = TrackKind::Video;
    project.tracks.push_back(std::move(target));
    return project;
}

// ---------------------------------------------------------------------------
// The generate hook (mirrors app/ApplicationComposition.cpp's makeGenerateHook)
// ---------------------------------------------------------------------------

[[nodiscard]] Tool::Handler makeGenerateHook(GenerativeMediaCoordinator& coordinator,
                                            const GenerativeBackend* backend) {
    return [&coordinator, backend](const Json& input) -> Result<Json> {
        // Requirement 12.4: the selected backend answers first, so a request that
        // cannot possibly be submitted is refused before the coordinator runs and
        // therefore adds no library entry, no clip and no undo entry.
        if (backend != nullptr) {
            const std::string unmet = backend->unmetPrecondition();
            if (!unmet.empty()) {
                return err<Json>(makeError(ErrorCode::FailedPrecondition, unmet));
            }
        }

        GenerationRequest request;
        request.model = input.stringOr("model");
        request.prompt = input.stringOr("prompt");
        const std::string mediaTypeText = input.stringOr("mediaType", "video");
        if (mediaTypeText == "image") {
            request.mediaType = GenerationMediaType::Image;
        } else if (mediaTypeText == "audio") {
            request.mediaType = GenerationMediaType::Audio;
        } else {
            request.mediaType = GenerationMediaType::Video;
        }
        request.mode = services::generationModeFromStringView(input.stringOr("mode", "generate"))
                          .value_or(services::GenerationMode::Generate);
        if (const std::optional<Uuid> sourceAssetId =
                Uuid::parse(input.stringOr("sourceAssetId"));
            sourceAssetId.has_value()) {
            request.sourceAssetId = *sourceAssetId;
        }
        const std::int64_t targetWidth = input.intOr("targetWidth", 0);
        const std::int64_t targetHeight = input.intOr("targetHeight", 0);
        if (targetWidth > 0 && targetHeight > 0) {
            request.targetResolution =
                Resolution{static_cast<std::uint32_t>(targetWidth),
                          static_cast<std::uint32_t>(targetHeight)};
        }
        request.requestedDuration =
            Duration::fromNanoseconds(input.intOr("requestedDurationTicks", 0));

        GenerationPlacement placement;
        const std::optional<Uuid> trackId = Uuid::parse(input.stringOr("trackId"));
        if (!trackId.has_value()) {
            return err<Json>(makeError(ErrorCode::InvalidArgument,
                                       "generation.generate: 'trackId' must be a valid UUID"));
        }
        placement.trackId = *trackId;
        placement.framePosition = input.intOr("framePosition", 0);
        placement.sourceIn = Duration::fromNanoseconds(input.intOr("sourceInTicks", 0));
        placement.sourceOut = Duration::fromNanoseconds(input.intOr("sourceOutTicks", 0));

        Result<GeneratedMediaPlacement> placed =
            coordinator.generateAndPlace(request, placement);
        if (placed.isError()) {
            return err<Json>(placed.error());
        }

        const GeneratedMediaPlacement& result = placed.value();
        Json out = Json::object();
        out.set("assetId", result.asset.assetId.toString());
        out.set("sourcePath", result.asset.sourcePath);
        out.set("clipId", result.clipId.toString());
        out.set("timelineStartTicks",
                static_cast<std::int64_t>(result.timelineStart.ticks()));
        return out;
    };
}

// ---------------------------------------------------------------------------
// The rig: the whole generation.generate path over one injected transport
// ---------------------------------------------------------------------------

/// Members are declared in construction order; everything that needs a reference
/// to an earlier member is built in the constructor body.
class GenerationRig {
public:
    /// `catalog`, when non-null, must outlive the rig (matching every other
    /// reference GenerativeMediaCoordinator's constructor takes). Left null for
    /// every property in this file that predates the catalog's existence, so
    /// their behaviour is unaffected by its addition.
    GenerationRig(GenerativeHttpTransport& transport, int videoTracks,
                 const GenerationModelCatalog* catalog = nullptr) {
        // A hosted credential in the store, so the selected backend's precondition
        // is MET and the hook does not short-circuit. Requirement 12.9 is about a
        // backend that could have submitted and did not.
        const Result<void> stored =
            secretStore_.store(HostedGenerativeBackend::credentialKey(kUserScope),
                               kStoredHostedCredential);
        EXPECT_TRUE(stored.isOk());

        // BYOK authorization for exactly the two models the valid requests name.
        // The real gate derives the provider from `request.model`, so this is what
        // makes an empty model id a pre-submission refusal.
        auth_.setByokManager(byok_);
        for (const char* model : {kVideoModel, kImageModel}) {
            const Result<void> saved =
                auth_.saveByokCredentials(ByokCredential{model, kStoredProviderKey});
            EXPECT_TRUE(saved.isOk());
        }

        // PR 406/396/395: a real composition authorizes whatever models the
        // installed catalog declares, not merely the two fixture models above
        // — the gate derives the provider from `request.model`, so an upscale
        // or audio test naming a catalog model needs it authorized here, the
        // same way kVideoModel/kImageModel are, or every such request refuses
        // at the entitlement gate before the catalog check it means to exercise
        // is ever reached.
        if (catalog != nullptr) {
            for (const CatalogModel& model : catalog->listModels()) {
                const Result<void> saved = auth_.saveByokCredentials(
                    ByokCredential{model.id, kStoredProviderKey});
                EXPECT_TRUE(saved.isOk());
            }
        }

        // Task 10.5's registry selects the backend; the injected transport is the
        // only route from it to a socket.
        GenerativeBackendRequest selection;
        selection.id = std::string(services::kGenerativeBackendHosted);
        selection.endpoint.baseUrl = kEndpointBase;
        selection.secretStore = &secretStore_;
        selection.transport = &transport;
        selection.userId = kUserScope;
        selection.credentials = [](std::string_view) { return true; };
        selection_ = selectGenerativeBackend(selection);
        EXPECT_EQ(selection_.id, "hosted");
        EXPECT_TRUE(selection_.startupError.empty()) << selection_.startupError;
        EXPECT_TRUE(selection_.backend->unmetPrecondition().empty());

        client_ = std::make_unique<GenerativeClient>(*selection_.backend);
        runner_ = std::make_unique<GenerativeClientRunner>(*client_);

        // The session owns the one engine and the one media library. Seed the
        // project through reset(), which also clears the undo history, so the
        // baseline undo depth a property observes is the one it built itself.
        const CommandResult seeded = session_.engine().reset(makeSeedProject(videoTracks));
        EXPECT_TRUE(seeded.changed()) << seeded.message();

        placer_ = std::make_unique<TimelineEnginePlacer>(session_.engine());
        placer_->setMediaLibrary(session_.mediaLibrary());
        coordinator_ = std::make_unique<GenerativeMediaCoordinator>(
            gate_, *runner_, session_.mediaLibrary(), *placer_, catalog);

        ToolRegistryHooks hooks;
        hooks.generate = makeGenerateHook(*coordinator_, selection_.backend.get());
        registry_ = buildDefaultToolRegistry(&session_, std::move(hooks));
        executor_ = std::make_unique<McpToolExecutor>(registry_, &session_);
    }

    [[nodiscard]] TimelineEngine& engine() noexcept { return session_.engine(); }
    [[nodiscard]] MediaManager& library() noexcept { return session_.mediaLibrary(); }
    [[nodiscard]] McpToolExecutor& executor() noexcept { return *executor_; }
    [[nodiscard]] const ToolRegistry& registry() const noexcept { return registry_; }

    /// The generation target: the empty video lane at the back of the seed.
    [[nodiscard]] Uuid targetTrackId() { return session_.engine().snapshot().tracks.back().id; }

    /// The first seeded lane, used for the prior edits that vary the undo depth.
    [[nodiscard]] Uuid seededTrackId() { return session_.engine().snapshot().tracks.front().id; }

private:
    UnusedAuthBackend authBackend_;
    AcceptingProviderValidator validator_;
    InMemorySecretStore secretStore_;
    ByokCredentialManager byok_{validator_, secretStore_, kUserScope};
    AuthenticationService auth_{authBackend_};
    AuthServiceGenerationGate gate_{auth_};
    GenerativeBackendSelection selection_;
    std::unique_ptr<GenerativeClient> client_;
    std::unique_ptr<GenerativeClientRunner> runner_;
    ProjectSession session_;
    std::unique_ptr<TimelineEnginePlacer> placer_;
    std::unique_ptr<GenerativeMediaCoordinator> coordinator_;
    ToolRegistry registry_;
    std::unique_ptr<McpToolExecutor> executor_;
};

// ---------------------------------------------------------------------------
// Helpers shared by the properties
// ---------------------------------------------------------------------------

/// The timeline end of a track, used to append prior edits without overlapping.
Duration trackEnd(const Project& project, const Uuid& trackId) {
    Duration end = Duration::zero();
    for (const Track& track : project.tracks) {
        if (track.id != trackId) continue;
        for (const Clip& clip : track.clips) {
            if (clip.timelineEnd() > end) end = clip.timelineEnd();
        }
    }
    return end;
}

std::size_t countClips(const Project& project, const Uuid& clipId) {
    std::size_t found = 0;
    for (const Track& track : project.tracks) {
        for (const Clip& clip : track.clips) {
            if (clip.id == clipId) ++found;
        }
    }
    return found;
}

std::size_t countAssets(const Project& project, const Uuid& assetId) {
    return static_cast<std::size_t>(
        std::count_if(project.assets.begin(), project.assets.end(),
                      [&assetId](const MediaAssetRef& a) { return a.assetId == assetId; }));
}

/// A valid `generation.generate` argument object. Every invalid case in Property
/// 66 is this object with exactly one field corrupted, so a refusal is always
/// attributable to that one field.
Json validArguments(const Uuid& trackId, bool isVideo, const std::string& prompt,
                    std::int64_t framePosition, std::int64_t sourceOutTicks) {
    Json args = Json::object();
    args.set("prompt", prompt);
    args.set("model", isVideo ? kVideoModel : kImageModel);
    args.set("mediaType", isVideo ? "video" : "image");
    args.set("trackId", trackId.toString());
    args.set("framePosition", framePosition);
    args.set("sourceInTicks", static_cast<std::int64_t>(0));
    args.set("sourceOutTicks", sourceOutTicks);
    return args;
}

/// Drops one member from an argument object (used for the "omits the prompt" and
/// "omits the model" cases, which the declared schema refuses as missing).
Json without(const Json& args, std::string_view field) {
    Json out = Json::object();
    for (const auto& [key, value] : args.asObject()) {
        if (key != field) out.set(key, value);
    }
    return out;
}

/// The scripted hosted exchange for one successful generation: accept the submit,
/// report `nonTerminalPolls` in-flight transitions, then succeed and hand back the
/// produced media (Requirement 12.3's queued/running/succeeded progression).
void scriptSuccessfulGeneration(ScriptedTransport& transport, const std::string& jobId,
                                const Uuid& assetId, const std::string& sourcePath,
                                bool isVideo, int nonTerminalPolls,
                                std::string_view mediaTypeOverride = {}) {
    transport.responses.push_back({201, std::string(R"({"id":")") + jobId + R"("})"});
    for (int i = 0; i < nonTerminalPolls; ++i) {
        const char* phase = (i % 2 == 0) ? "queued" : "running";
        transport.responses.push_back(
            {200, std::string(R"({"status":")") + phase + R"(","progress":)" +
                      std::to_string(10 * (i + 1)) + "}"});
    }
    transport.responses.push_back({200, R"({"status":"succeeded","progress":100})"});
    const std::string responseMediaType = mediaTypeOverride.empty()
                                              ? (isVideo ? "video" : "image")
                                              : std::string(mediaTypeOverride);
    transport.responses.push_back(
        {200, std::string(R"({"assetId":")") + assetId.toString() + R"(","sourcePath":")" +
                  sourcePath + R"(","mediaType":")" + responseMediaType +
                  R"("})"});
}

/// A per-case scratch path for the produced media. Absolute, and unique per
/// process and per case; nothing in this suite opens it.
std::string generatedMediaPath(const Uuid& assetId, bool isVideo) {
    return "/var/tmp/palmier-generated-" + std::to_string(static_cast<long>(::getpid())) +
           "-" + assetId.toString() + (isVideo ? ".mp4" : ".png");
}

// ===========================================================================
// Non-vacuity of the interposers
// ===========================================================================

TEST(GenerativeLifecycleNetworkSeam, TheInterposersObserveARealSocketCall) {
    // Guards every zero-count assertion in this file. If the interposers were not
    // linked into this binary, "no socket was opened" would be true of a test that
    // opened one, and Property 66 would prove nothing. Arming and then opening a
    // real socket must be seen, and the interposer must forward to glibc.
    NetworkWatch watch;
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_GE(fd, 0) << "the interposer must forward to the real socket()";
    if (fd >= 0) ::close(fd);
    EXPECT_GE(watch.calls(), 1u);
}

TEST(GenerativeLifecycleNetworkSeam, TheGenerateToolIsTheOneRoutedEntryPoint) {
    // The rig is only meaningful if it really is the shared surface: the tool must
    // be advertised, and its declared schema must carry the constraints Property 66
    // relies on (Requirement 12.9's closed media-kind set and prompt bounds).
    ForbiddenTransport transport;
    GenerationRig rig(transport, 1);

    const Tool* tool = rig.registry().find(kGenerateTool);
    ASSERT_NE(tool, nullptr);
    const services::ArgSpec* prompt = tool->schema.find("prompt");
    ASSERT_NE(prompt, nullptr);
    EXPECT_TRUE(prompt->required);
    EXPECT_EQ(prompt->minLength.value_or(0u), 1u);
    EXPECT_EQ(prompt->maxLength.value_or(0u), 2000u);

    const services::ArgSpec* mediaType = tool->schema.find("mediaType");
    ASSERT_NE(mediaType, nullptr);
    // PR 395 widened the closed set to admit "audio"; "video"/"image" remain
    // Requirement 12.9's original two, which is what Property 66 still relies on
    // ("names a media kind other than video or image" is refused before this
    // widening AND after it, since the coordinator's own catalog-driven check
    // still refuses an audio request with no catalog able to authorize it —
    // see the no-catalog branch of checkAgainstCatalog).
    EXPECT_EQ(mediaType->enumValues, std::vector<std::string>({"video", "image", "audio"}));
    EXPECT_EQ(transport.sends, 0u);
}

// ===========================================================================
// Property 65: A successful generation is one undoable edit
// ===========================================================================

// Feature: end-to-end-editor-integration, Property 65: A successful generation is
// one undoable edit — on the `succeeded` status the produced media is registered
// in the media library and placed on the requested track at the requested
// position as a single undoable edit that one undo action reverses completely.
// Validates: Requirements 12.3
RC_GTEST_PROP(GenerativeLifecycleProperties, ASuccessfulGenerationIsOneUndoableEdit, ()) {
    // --- generated inputs, all inside Requirement 12.3's antecedent ---------
    const int videoTracks = *rc::gen::inRange(1, 4);
    const int priorEdits = *rc::gen::inRange(0, 4);       // varies the baseline undo depth
    const int nonTerminalPolls = *rc::gen::inRange(0, 4); // queued/running transitions
    const bool isVideo = *rc::gen::element(true, false);  // a media kind of video or image
    const int promptLength = *rc::gen::inRange(1, 2001);  // a prompt of 1..2000 characters
    const std::int64_t clipSeconds = *rc::gen::inRange(1, 6);

    ScriptedTransport transport;
    GenerationRig rig(transport, videoTracks);
    TimelineEngine& engine = rig.engine();

    // Prior edits on a SEEDED lane (never the target), so the undo depth this
    // generation adds to is an arbitrary number rather than always zero. Appending
    // past the track end keeps every invariant satisfied.
    const Uuid seededTrack = rig.seededTrackId();
    for (int i = 0; i < priorEdits; ++i) {
        const Project before = engine.snapshot();
        const MediaAssetRef asset(Uuid::generateV4(), "mem://prior");
        const Duration start = trackEnd(before, seededTrack) + ms(*rc::gen::inRange(1, 500));
        const CommandResult applied = engine.apply(std::make_unique<AddClipCommand>(
            seededTrack, makeClip(Uuid::generateV4(), asset, start, ms(0), ms(500))));
        RC_ASSERT(applied.changed());
    }

    // A placement inside [0, current timeline duration] on the empty target lane.
    const Uuid targetTrack = rig.targetTrackId();
    const Project staged = engine.snapshot();
    const std::int64_t maxFrames =
        staged.timelineFps.framesForDuration(timelineDuration(staged));
    const std::int64_t framePosition =
        *rc::gen::inRange<std::int64_t>(0, maxFrames > 0 ? maxFrames + 1 : 1);

    // The hosted exchange that succeeds, scripted over the seam.
    const Uuid producedAssetId = Uuid::generateV4();
    const std::string sourcePath = generatedMediaPath(producedAssetId, isVideo);
    scriptSuccessfulGeneration(transport, "job-" + producedAssetId.toString(),
                               producedAssetId, sourcePath, isVideo, nonTerminalPolls);

    // --- the exact pre-generation state ------------------------------------
    const std::string beforeDocument = serializeProject(engine.snapshot());
    const std::size_t undoBefore = engine.undoDepth();
    const std::size_t libraryBefore = rig.library().assetCount();
    RC_ASSERT(undoBefore == static_cast<std::size_t>(priorEdits));

    const Json arguments =
        validArguments(targetTrack, isVideo, std::string(static_cast<std::size_t>(promptLength), 'p'),
                       framePosition, clipSeconds * 1000000000);

    const Result<Json> executed =
        rig.executor().executeTool(kGenerateTool, arguments, InvocationSource::Mcp);
    RC_ASSERT(executed.isOk());

    // The generation really did travel through the seam: submit + every poll +
    // fetch. This is what makes Property 66's zero-count a refusal rather than a
    // hole in the wiring.
    RC_ASSERT(transport.requests.size() ==
              static_cast<std::size_t>(nonTerminalPolls) + 3u);

    const std::optional<Uuid> placedClip = Uuid::parse(executed.value().stringOr("clipId"));
    RC_ASSERT(placedClip.has_value());
    RC_ASSERT(executed.value().stringOr("sourcePath") == sourcePath);

    // --- ONE undoable edit, not one-or-more --------------------------------
    // Requirement 12.3's "as a single undoable edit": the history grew by exactly
    // one entry. A pipeline that recorded the library registration and the clip
    // placement as two commands would show +2 here and would need two undos below.
    RC_ASSERT(engine.undoDepth() == undoBefore + 1);

    // The edit did both halves of the work: the clip is on the requested track at
    // the requested position, and the produced media is in the library — both the
    // project's asset table and the session's MediaManager.
    const Project afterGeneration = engine.snapshot();
    RC_ASSERT(countClips(afterGeneration, *placedClip) == 1u);
    RC_ASSERT(countAssets(afterGeneration, producedAssetId) == 1u);
    RC_ASSERT(rig.library().assetCount() == libraryBefore + 1u);
    RC_ASSERT(rig.library().hasAsset(producedAssetId));
    for (const Track& track : afterGeneration.tracks) {
        for (const Clip& clip : track.clips) {
            if (clip.id != *placedClip) continue;
            RC_ASSERT(track.id == targetTrack);
            RC_ASSERT(clip.timelineStart == staged.timelineFps.durationForFrames(framePosition));
            RC_ASSERT(clip.assetRef.assetId == producedAssetId);
        }
    }
    // Sanity: the generation is observable in the document, so the comparison
    // after the undo below is a real restoration and not a tautology.
    RC_ASSERT(serializeProject(afterGeneration) != beforeDocument);

    // --- ONE undo reverses it COMPLETELY ------------------------------------
    const CommandResult undone = engine.undo();
    RC_ASSERT(undone.changed());

    // The whole claim, as a document comparison rather than as spot checks: after
    // exactly one undo the project serializes byte-for-byte to what it was before
    // the generation. The clip and its media-library entry went away together,
    // because they were one command.
    const Project afterUndo = engine.snapshot();
    RC_ASSERT(serializeProject(afterUndo) == beforeDocument);
    RC_ASSERT(countClips(afterUndo, *placedClip) == 0u);
    RC_ASSERT(countAssets(afterUndo, producedAssetId) == 0u);

    // And the history is back where it started — so the single entry was consumed
    // by the single undo, with nothing of the generation left to undo.
    RC_ASSERT(engine.undoDepth() == undoBefore);
    RC_ASSERT(engine.canUndo() == (undoBefore > 0));
    RC_ASSERT(engine.redoDepth() == 1u);
}

// ===========================================================================
// Property 66: Invalid generation requests never reach the network
// ===========================================================================

/// One invalid request: the arguments, and whether the product's refusal names
/// the offending field (every declared-schema rejection does; the empty-model
/// refusal comes from the entitlement gate and names the entitlement instead).
struct InvalidCase {
    std::string label;
    Json arguments;
    std::string offendingField;
    bool namesField = true;
};

// Feature: end-to-end-editor-integration, Property 66: Invalid generation
// requests never reach the network — a request that omits the prompt, carries a
// prompt outside 1..2000 characters, names a media kind other than video, image,
// or audio, or carries an out-of-range position/duration is rejected before
// submission, issues no network request, and leaves the project and media library
// unchanged.
// Validates: Requirements 12.9
RC_GTEST_PROP(GenerativeLifecycleProperties, InvalidGenerationRequestsNeverReachTheNetwork,
              ()) {
    const int videoTracks = *rc::gen::inRange(1, 4);
    const bool isVideo = *rc::gen::element(true, false);
    const std::int64_t validFramePosition = *rc::gen::inRange<std::int64_t>(0, 30);
    const std::int64_t validSourceOut = *rc::gen::inRange<std::int64_t>(1, 5) * 1000000000;

    // Generated magnitudes for each corruption, so the property covers the whole
    // invalid region rather than one representative value per kind.
    const int overLongPrompt = *rc::gen::inRange(2001, 2600);
    const std::string unknownMediaKind =
        *rc::gen::element<std::string>("text", "model3d", "VIDEO", "Image", "");
    const std::int64_t negativePosition = -*rc::gen::inRange<std::int64_t>(1, 100000);
    const std::int64_t nonPositiveDuration = -*rc::gen::inRange<std::int64_t>(0, 100000);
    const std::int64_t collapsedIn = *rc::gen::inRange<std::int64_t>(1, 1000000000);
    const std::string malformedTrackId =
        *rc::gen::element<std::string>("not-a-uuid", "", "1234", "{}", "0000");

    ForbiddenTransport transport;
    GenerationRig rig(transport, videoTracks);
    TimelineEngine& engine = rig.engine();

    const Json valid = validArguments(rig.targetTrackId(), isVideo,
                                      std::string(static_cast<std::size_t>(
                                                      *rc::gen::inRange(1, 2001)), 'p'),
                                      validFramePosition, validSourceOut);

    // Every case is `valid` with exactly ONE field corrupted.
    std::vector<InvalidCase> cases;

    // Requirement 12.9: "omits the prompt".
    cases.push_back({"prompt omitted", without(valid, "prompt"), "prompt", true});

    // "carries a prompt outside 1 to 2000 characters" — both ends.
    Json emptyPrompt = valid;
    emptyPrompt.set("prompt", std::string{});
    cases.push_back({"prompt empty", emptyPrompt, "prompt", true});

    Json longPrompt = valid;
    longPrompt.set("prompt", std::string(static_cast<std::size_t>(overLongPrompt), 'p'));
    cases.push_back({"prompt over 2000 characters", longPrompt, "prompt", true});

    // "names a media kind other than `video` or `image`".
    Json badMediaType = valid;
    badMediaType.set("mediaType", unknownMediaKind);
    cases.push_back({"media kind outside the closed set", badMediaType, "mediaType", true});

    // "names a negative timeline position".
    Json negativeFrame = valid;
    negativeFrame.set("framePosition", negativePosition);
    cases.push_back({"negative timeline position", negativeFrame, "framePosition", true});

    // An out-of-range duration, in both forms: a non-positive declared source
    // out-point (refused by the declared minimum), and a source range that
    // collapses or inverts (the cross-field rule the coordinator owns, which the
    // schema cannot express).
    Json nonPositiveOut = valid;
    nonPositiveOut.set("sourceOutTicks", nonPositiveDuration);
    cases.push_back({"non-positive source duration", nonPositiveOut, "sourceOutTicks", true});

    Json collapsedRange = valid;
    collapsedRange.set("sourceInTicks", collapsedIn);
    collapsedRange.set("sourceOutTicks", collapsedIn);
    cases.push_back({"collapsed source range", collapsedRange, "sourceOut", true});

    // An empty model id: the model names no provider the backend serves, so the
    // real entitlement gate refuses it before submission. Its message names the
    // missing entitlement rather than the argument, which is why `namesField` is
    // false here — the pre-submission refusal is the part Requirement 12.9 shares.
    Json emptyModel = valid;
    emptyModel.set("model", std::string{});
    cases.push_back({"empty model id", emptyModel, "model", false});

    cases.push_back({"model omitted", without(valid, "model"), "model", true});

    // A track identifier that is not a track, in the form the declared schema
    // refuses before dispatch: not a well-formed UUID at all.
    Json badTrack = valid;
    badTrack.set("trackId", malformedTrackId);
    cases.push_back({"malformed track identifier", badTrack, "trackId", true});

    // "names a track identifier absent from the current project" (the fifth
    // Requirement 12.9 argument): syntactically a valid UUID, but not one of the
    // rig's actual tracks. GenerativeMediaCoordinator::generateAndPlace's
    // trackExists() check refuses this before the entitlement gate, the runner,
    // or the media library are ever reached — the source change task 10.6's
    // notes called for. The refusal names "target track", not the argument
    // "trackId", so `namesField` is false here for the same reason as the empty
    // model id case above.
    Json unknownTrack = valid;
    unknownTrack.set("trackId", Uuid::generateV4().toString());
    cases.push_back({"unknown (but well-formed) track identifier", unknownTrack, "trackId", false});

    // --- the exact pre-request state ---------------------------------------
    const std::string beforeDocument = serializeProject(engine.snapshot());
    const std::size_t undoBefore = engine.undoDepth();
    const std::size_t libraryBefore = rig.library().assetCount();

    for (const InvalidCase& invalid : cases) {
        // Both mechanisms armed for the one call: the ForbiddenTransport fails the
        // test if the request goes THROUGH the seam, the interposers if it goes
        // AROUND it.
        NetworkWatch watch;
        const Result<Json> executed =
            rig.executor().executeTool(kGenerateTool, invalid.arguments, InvocationSource::Mcp);

        // Refused, before submission.
        RC_ASSERT(executed.isError());

        // No network request was issued — proven twice, at the seam and at the C
        // library entry points.
        RC_ASSERT(transport.sends == 0u);
        RC_ASSERT(watch.calls() == 0u);

        // The project and the media library are untouched, and no undo entry was
        // recorded: the refusal happened before anything could be applied.
        RC_ASSERT(serializeProject(engine.snapshot()) == beforeDocument);
        RC_ASSERT(rig.library().assetCount() == libraryBefore);
        RC_ASSERT(engine.undoDepth() == undoBefore);

        // The error names the offending argument wherever the product names it.
        if (invalid.namesField) {
            RC_ASSERT(executed.error().message().find(invalid.offendingField) !=
                      std::string::npos);
        }
    }

    // Nothing above reached the transport, and the rig was the one that DOES reach
    // it for a valid request (Property 65 asserts that on the same wiring).
    RC_ASSERT(transport.sends == 0u);
}

// ===========================================================================
// PR 406 / PR 396 / PR 395 — the model catalog, upscale mode, and audio
// generation (usable-editor spec, Phase 2 task 7; docs/PORT_BACKLOG.md)
// ===========================================================================
//
// Each test below drives EXACTLY the acceptance check its own PORT_BACKLOG.md
// entry declares, over the same GenerationRig/ToolExecutor path every property
// above uses — the catalog and its two dependent features are additive to that
// path, not a parallel one, so proving them here is proving the real
// generation.generate / generation.list_models tools, not a stand-in.

TEST(GenerationModelCatalogTools, ListModelsGroupsEveryModelUnderItsProviderWithAtLeastTwoProviders) {
    // PR 406's check: "a catalog listing at least two providers with at least
    // one model each" / "the returned listing groups every model under its
    // provider".
    const GenerationModelCatalog catalog;
    const std::vector<CatalogModel>& models = catalog.listModels();

    std::set<std::string> providers;
    for (const CatalogModel& model : models) providers.insert(model.provider);
    ASSERT_GE(providers.size(), 2u) << "the catalog does not list at least two providers";

    // Every provider seen has at least one model (trivially true of any grouping
    // built from a non-empty models list, asserted directly rather than assumed).
    for (const std::string& provider : providers) {
        const auto count = std::count_if(models.begin(), models.end(),
                                         [&provider](const CatalogModel& m) {
                                             return m.provider == provider;
                                         });
        EXPECT_GE(count, 1) << "provider '" << provider << "' lists no model";
    }
}

TEST(GenerationModelCatalogTools, GenerateAcceptsAModelIdThatIsInTheCatalog) {
    // PR 406's check, positive half: "generation.generate accepts the selected
    // model id ... from a named provider".
    const GenerationModelCatalog catalog;
    ScriptedTransport transport;
    GenerationRig rig(transport, 1, &catalog);

    const std::string knownModel = catalog.listModels().front().id;
    const Uuid producedAssetId = Uuid::generateV4();
    scriptSuccessfulGeneration(transport, "job-known-model", producedAssetId,
                               generatedMediaPath(producedAssetId, true), true, 0);

    const Json arguments = validArguments(rig.targetTrackId(), true, "a prompt", 0, 1'000'000'000);
    Json withKnownModel = arguments;
    withKnownModel.set("model", knownModel);

    const Result<Json> executed =
        rig.executor().executeTool(kGenerateTool, withKnownModel, InvocationSource::Mcp);
    ASSERT_TRUE(executed.isOk()) << executed.error().message();
}

TEST(GenerationModelCatalogTools, GenerateRefusesAModelIdAbsentFromTheCatalogNamingIt) {
    // PR 406's check, negative half: "refuses an id absent from the catalog with
    // an error naming the rejected id".
    const GenerationModelCatalog catalog;
    ForbiddenTransport transport;
    GenerationRig rig(transport, 1, &catalog);

    const std::string unknownModel = "not-a-catalog-model-id";
    Json arguments = validArguments(rig.targetTrackId(), true, "a prompt", 0, 1'000'000'000);
    arguments.set("model", unknownModel);

    const Result<Json> executed =
        rig.executor().executeTool(kGenerateTool, arguments, InvocationSource::Mcp);
    ASSERT_TRUE(executed.isError());
    EXPECT_NE(executed.error().message().find(unknownModel), std::string::npos)
        << executed.error().message();
    // Refused before ever reaching the transport (ForbiddenTransport would have
    // failed this test outright if it had been asked to send anything).
    EXPECT_EQ(transport.sends, 0u);
}

TEST(GenerationModelCatalogTools, ListModelsToolReturnsTheCatalogGroupedByProvider) {
    // The SAME claim as ListModelsGroupsEveryModelUnderItsProviderWithAtLeastTwoProviders,
    // but through the actual generation.list_models tool a caller invokes, over a
    // hook that mirrors the composition root's makeListModelsHook exactly (the
    // registry itself has no default implementation for this tool, matching every
    // other capability-specific hook in this rig). A bare ProjectSession is enough
    // here: every tool in this registry is session-guarded (guardedHookHandler
    // refuses a null session before the hook itself ever runs), and this test
    // needs no generative-coordinator wiring at all.
    const GenerationModelCatalog catalog;

    ToolRegistryHooks hooks;
    hooks.listModels = [&catalog](const Json&) -> Result<Json> {
        std::map<std::string, Json> byProvider;
        for (const CatalogModel& model : catalog.listModels()) {
            Json entry = Json::object();
            entry.set("id", model.id);
            Json& list = byProvider[model.provider];
            if (!list.isArray()) list = Json::array();
            list.push_back(std::move(entry));
        }
        Json providers = Json::object();
        for (auto& [provider, list] : byProvider) providers.set(provider, std::move(list));
        Json out = Json::object();
        out.set("providers", std::move(providers));
        return out;
    };
    ProjectSession session;
    ToolRegistry registryWithListModels = buildDefaultToolRegistry(&session, std::move(hooks));
    const Tool* listModelsTool = registryWithListModels.find("generation.list_models");
    ASSERT_NE(listModelsTool, nullptr) << "generation.list_models is not published";
    ASSERT_TRUE(static_cast<bool>(listModelsTool->handler));

    const Result<Json> result = listModelsTool->handler(Json::object());
    ASSERT_TRUE(result.isOk()) << result.error().message();
    const Json* providers = result.value().find("providers");
    ASSERT_NE(providers, nullptr);
    ASSERT_TRUE(providers->isObject());
    EXPECT_GE(providers->asObject().size(), 2u);
}

TEST(GenerationUpscaleProperties, AnUpscaleRequestForAnExistingClipCompletesAsOneUndoableEdit) {
    // PR 396's check: given a project with one clip in the media library and a
    // generative backend that serves an upscale-capable model, when
    // generation.generate is invoked with mode upscale and a target resolution
    // larger than the source, then the schema lists 'upscale' among mode's
    // permitted values, the request is accepted, and a successful job registers
    // exactly one new asset at the requested resolution as one undoable edit.
    const GenerationModelCatalog catalog;
    const std::string upscaleModel = [&catalog] {
        for (const CatalogModel& model : catalog.listModels()) {
            if (model.servesUpscale) return model.id;
        }
        return std::string{};
    }();
    ASSERT_FALSE(upscaleModel.empty()) << "the catalog declares no upscale-capable model";

    ScriptedTransport transport;
    GenerationRig rig(transport, 1, &catalog);

    // Schema check: 'upscale' is among mode's permitted values.
    const Tool* tool = rig.registry().find(kGenerateTool);
    ASSERT_NE(tool, nullptr);
    const services::ArgSpec* modeArg = tool->schema.find("mode");
    ASSERT_NE(modeArg, nullptr);
    EXPECT_NE(std::find(modeArg->enumValues.begin(), modeArg->enumValues.end(), "upscale"),
             modeArg->enumValues.end());

    // A project with one clip in the media library — an EXISTING clip, per PR
    // 396's own framing, registered directly (matching how every other property
    // in this file seeds prior state) rather than through a real media.import.
    const MediaAssetRef sourceAsset(Uuid::generateV4(), "mem://source-clip.mp4");
    ASSERT_TRUE(rig.library().importAsset(sourceAsset).isOk());

    const Uuid producedAssetId = Uuid::generateV4();
    const std::string sourcePath = generatedMediaPath(producedAssetId, true);
    scriptSuccessfulGeneration(transport, "job-upscale-1", producedAssetId, sourcePath, true, 0);

    const std::size_t undoBefore = rig.engine().undoDepth();
    const std::size_t libraryBefore = rig.library().assetCount();

    Json arguments = validArguments(rig.targetTrackId(), true, "unused for upscale", 0,
                                    1'000'000'000);
    arguments.set("model", upscaleModel);
    arguments.set("mode", "upscale");
    arguments.set("sourceAssetId", sourceAsset.assetId.toString());
    arguments.set("targetWidth", static_cast<std::int64_t>(3840));
    arguments.set("targetHeight", static_cast<std::int64_t>(2160));

    const Result<Json> executed =
        rig.executor().executeTool(kGenerateTool, arguments, InvocationSource::Mcp);
    ASSERT_TRUE(executed.isOk()) << executed.error().message();

    // Exactly one new asset registered, as one undoable edit.
    EXPECT_EQ(rig.library().assetCount(), libraryBefore + 1u);
    EXPECT_TRUE(rig.library().hasAsset(producedAssetId));
    EXPECT_EQ(rig.engine().undoDepth(), undoBefore + 1);

    const CommandResult undone = rig.engine().undo();
    EXPECT_TRUE(undone.changed());
    EXPECT_EQ(rig.library().assetCount(), libraryBefore);
}

TEST(GenerationUpscaleProperties, UpscaleIsRefusedByNameForAModelThatDoesNotServeIt) {
    const GenerationModelCatalog catalog;
    const std::string nonUpscaleModel = [&catalog] {
        for (const CatalogModel& model : catalog.listModels()) {
            if (!model.servesUpscale) return model.id;
        }
        return std::string{};
    }();
    ASSERT_FALSE(nonUpscaleModel.empty())
        << "every catalog model serves upscale, so this negative control is vacuous";

    ForbiddenTransport transport;
    GenerationRig rig(transport, 1, &catalog);

    const MediaAssetRef sourceAsset(Uuid::generateV4(), "mem://source-clip.mp4");
    ASSERT_TRUE(rig.library().importAsset(sourceAsset).isOk());

    Json arguments = validArguments(rig.targetTrackId(), true, "unused for upscale", 0,
                                    1'000'000'000);
    arguments.set("model", nonUpscaleModel);
    arguments.set("mode", "upscale");
    arguments.set("sourceAssetId", sourceAsset.assetId.toString());
    arguments.set("targetWidth", static_cast<std::int64_t>(3840));
    arguments.set("targetHeight", static_cast<std::int64_t>(2160));

    const Result<Json> executed =
        rig.executor().executeTool(kGenerateTool, arguments, InvocationSource::Mcp);
    ASSERT_TRUE(executed.isError());
    EXPECT_NE(executed.error().message().find(nonUpscaleModel), std::string::npos)
        << executed.error().message();
    EXPECT_EQ(transport.sends, 0u);
}

TEST(GenerationAudioProperties, AudioGenerationFromASourceAndFromAPromptBothCompleteWithinTheDeclaredRange) {
    // PR 395's check: given a project with one audio track and a generative
    // backend that serves an audio model with a declared duration range, when
    // generation.generate is invoked twice for audio — once with a source clip
    // and once with a prompt only, each requesting a duration inside the
    // declared range — then both requests are accepted and each registers
    // exactly one audio asset of the requested duration as one undoable edit.
    const GenerationModelCatalog catalog;
    const CatalogModel* audioModel = nullptr;
    for (const CatalogModel& model : catalog.listModels()) {
        if (model.audioDurationRange.has_value()) {
            audioModel = &model;
            break;
        }
    }
    ASSERT_NE(audioModel, nullptr) << "the catalog declares no audio model";
    const auto [minDuration, maxDuration] = *audioModel->audioDurationRange;
    ASSERT_LT(minDuration, maxDuration) << "the audio model's declared range is degenerate";
    const Duration insideRange =
        Duration::fromNanoseconds((minDuration.ticks() + maxDuration.ticks()) / 2);
    ASSERT_GE(insideRange, minDuration);
    ASSERT_LE(insideRange, maxDuration);

    ScriptedTransport transport;
    GenerationRig rig(transport, 1, &catalog);

    // --- request 1: audio generated FROM a source clip ---------------------
    const MediaAssetRef sourceAsset(Uuid::generateV4(), "mem://source-audio.wav");
    ASSERT_TRUE(rig.library().importAsset(sourceAsset).isOk());

    const Uuid firstAssetId = Uuid::generateV4();
    // Script the response as audio as well: the coordinator enforces the
    // contract that a completed provider asset matches the requested media kind.
    scriptSuccessfulGeneration(transport, "job-audio-source", firstAssetId,
                               "/var/tmp/generated-audio-1.wav", false, 0, "audio");

    const std::size_t undoAfterSeed = rig.engine().undoDepth();
    const std::size_t libraryAfterSeed = rig.library().assetCount();

    Json fromSource = validArguments(rig.targetTrackId(), false, "unused with a source",
                                     0, 1'000'000'000);
    fromSource.set("model", audioModel->id);
    fromSource.set("mediaType", "audio");
    fromSource.set("sourceAssetId", sourceAsset.assetId.toString());
    fromSource.set("requestedDurationTicks", static_cast<std::int64_t>(insideRange.ticks()));

    const Result<Json> executedFromSource =
        rig.executor().executeTool(kGenerateTool, fromSource, InvocationSource::Mcp);
    ASSERT_TRUE(executedFromSource.isOk()) << executedFromSource.error().message();
    EXPECT_EQ(rig.library().assetCount(), libraryAfterSeed + 1u);
    EXPECT_TRUE(rig.library().hasAsset(firstAssetId));
    EXPECT_EQ(rig.engine().undoDepth(), undoAfterSeed + 1);

    // --- request 2: audio generated from a PROMPT ONLY, at a fresh position
    // AFTER the first clip (the timeline's duration grows to accommodate the
    // first placement, so this position must be derived from the project's
    // actual state after it, not merely "some large frame number").
    const Uuid secondAssetId = Uuid::generateV4();
    scriptSuccessfulGeneration(transport, "job-audio-prompt", secondAssetId,
                               "/var/tmp/generated-audio-2.wav", false, 0, "audio");

    const std::size_t undoAfterFirst = rig.engine().undoDepth();
    const std::size_t libraryAfterFirst = rig.library().assetCount();

    const Project afterFirst = rig.engine().snapshot();
    const std::int64_t secondFramePosition =
        afterFirst.timelineFps.framesForDuration(timelineDuration(afterFirst));

    Json fromPrompt = validArguments(rig.targetTrackId(), false, "an audio prompt",
                                     secondFramePosition, 1'000'000'000);
    fromPrompt.set("model", audioModel->id);
    fromPrompt.set("mediaType", "audio");
    fromPrompt.set("requestedDurationTicks", static_cast<std::int64_t>(insideRange.ticks()));

    const Result<Json> executedFromPrompt =
        rig.executor().executeTool(kGenerateTool, fromPrompt, InvocationSource::Mcp);
    ASSERT_TRUE(executedFromPrompt.isOk()) << executedFromPrompt.error().message();
    EXPECT_EQ(rig.library().assetCount(), libraryAfterFirst + 1u);
    EXPECT_TRUE(rig.library().hasAsset(secondAssetId));
    EXPECT_EQ(rig.engine().undoDepth(), undoAfterFirst + 1);
}

TEST(GenerationAudioProperties, ADurationOutsideTheDeclaredRangeIsRefusedNamingThePermittedRange) {
    const GenerationModelCatalog catalog;
    const CatalogModel* audioModel = nullptr;
    for (const CatalogModel& model : catalog.listModels()) {
        if (model.audioDurationRange.has_value()) {
            audioModel = &model;
            break;
        }
    }
    ASSERT_NE(audioModel, nullptr) << "the catalog declares no audio model";
    const auto [minDuration, maxDuration] = *audioModel->audioDurationRange;
    const Duration tooLong = maxDuration + Duration::fromSeconds(60.0);

    ForbiddenTransport transport;
    GenerationRig rig(transport, 1, &catalog);

    Json arguments = validArguments(rig.targetTrackId(), false, "an audio prompt", 0,
                                    1'000'000'000);
    arguments.set("model", audioModel->id);
    arguments.set("mediaType", "audio");
    arguments.set("requestedDurationTicks", static_cast<std::int64_t>(tooLong.ticks()));

    const Result<Json> executed =
        rig.executor().executeTool(kGenerateTool, arguments, InvocationSource::Mcp);
    ASSERT_TRUE(executed.isError());
    // The error names the permitted range (the check's own wording).
    const std::string& message = executed.error().message();
    EXPECT_NE(message.find(std::to_string(minDuration.seconds())), std::string::npos) << message;
    EXPECT_NE(message.find(std::to_string(maxDuration.seconds())), std::string::npos) << message;
    EXPECT_EQ(transport.sends, 0u);
}

}  // namespace
}  // namespace palmier
