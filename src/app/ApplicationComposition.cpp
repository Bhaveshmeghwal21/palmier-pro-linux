// SPDX-License-Identifier: GPL-3.0-or-later
//
// app/ApplicationComposition.cpp — implementation of the application composition
// root (task 21.1; Requirements 1.6, 7.1, 7.2, 7.9, 13.3).
//
// Constructs every component in dependency order and connects them:
//   secret store -> BYOK manager -> auth service
//   auth service -> generation gate + agent gate
//   generative backend -> generative client -> generation runner
//   project session (engine + media library) -> timeline placer -> generative
//     coordinator
//   project session (+ generate hook) -> shared ToolRegistry
//   ToolRegistry -> McpToolExecutor -> McpServer (handler) and AgentOrchestrator
//
// Network-facing backends absent from the config get an OFFLINE default that
// reports the feature as unconfigured without touching the network, so the
// editor and the MCP server come up and run with no network connection
// (Requirement 13.3). The composition itself performs no I/O beyond the GPU
// device probe (which degrades to software) and reading the localization seams.

#include "app/ApplicationComposition.hpp"

#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "core/Duration.hpp"
#include "core/Error.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/MediaManager.hpp"
#include "core/Project.hpp"
#include "core/Result.hpp"
#include "core/TimelineEngine.hpp"
#include "core/Uuid.hpp"

#include "gpu/Compositor.hpp"
#include "gpu/GpuContext.hpp"

#include "media/AudioEngine.hpp"
#include "media/AudioSinkSelector.hpp"
#include "media/DecoderClipFrameProvider.hpp"
#include "media/DecoderTeardownQueue.hpp"
#include "media/MediaDecoder.hpp"

#include "ui/PreviewController.hpp"

#include "services/AgentOrchestrator.hpp"
#include "services/AuthenticationService.hpp"
#include "services/ByokCredentialManager.hpp"
#include "services/ByokCredentials.hpp"
#include "services/GenerativeClient.hpp"
#include "services/GenerativeMediaCoordinator.hpp"
#include "services/Json.hpp"
#include "services/LocalizationManager.hpp"
#include "services/McpServer.hpp"
#include "services/McpToolExecutor.hpp"
#include "services/MentionResolver.hpp"
#include "services/ProjectSaveService.hpp"
#include "services/ProjectSession.hpp"
#include "services/RemoteAccessGate.hpp"
#include "services/SecretStore.hpp"
#include "services/ToolRegistry.hpp"

namespace palmier::app {

namespace {

// ---------------------------------------------------------------------------
// Offline default backends
// ---------------------------------------------------------------------------
//
// These stand in for the closed-source hosted services when no real backend is
// injected. Each reports the feature as unconfigured WITHOUT performing any
// network activity, so the open-source editor + MCP server run offline
// (Requirements 13.3/13.4) while generative/auth features cleanly report
// "unavailable" until real backends are wired by the caller.

[[nodiscard]] Error offlineError(const char* feature) {
    return makeError(ErrorCode::Unsupported,
                     std::string(feature) +
                         " backend is not configured (offline build); no network was contacted");
}

class OfflineAuthBackend final : public services::AuthBackend {
public:
    [[nodiscard]] Result<services::BackendSession> authenticate(
        const services::LoginCredentials&) override {
        return err<services::BackendSession>(offlineError("authentication"));
    }
};

class OfflineGenerativeBackend final : public services::IGenerativeBackend {
public:
    [[nodiscard]] Result<services::JobId> submit(const services::GenerationRequest&,
                                                 std::string_view) override {
        return err<services::JobId>(offlineError("generative"));
    }
    [[nodiscard]] Result<services::GenerationStatus> poll(const services::JobId&,
                                                          std::string_view) override {
        return err<services::GenerationStatus>(offlineError("generative"));
    }
    [[nodiscard]] Result<services::MediaAsset> fetchResult(const services::JobId&,
                                                           std::string_view) override {
        return err<services::MediaAsset>(offlineError("generative"));
    }
    [[nodiscard]] Result<void> cancel(const services::JobId&, std::string_view) override {
        // Cancelling an unknown/never-submitted job is a harmless no-op.
        return ok();
    }
};

class OfflineByokValidator final : public services::ByokProviderValidator {
public:
    [[nodiscard]] Result<void> validate(const services::ByokCredential&) override {
        return err<void>(offlineError("BYOK validation"));
    }
};

// The agent interpreter is no longer defaulted to an inert
// "not configured" stub. Task 10.1 replaced `makeUnconfiguredInterpreter()` with
// `services::selectAgentInterpreter()`, which always installs a working
// implementation: the configured id when it is selectable, and otherwise the
// built-in `OfflineIntentInterpreter` plus a startup error naming the rejected id
// (design.md D9; Requirements 11.1, 11.3, 11.8). See the construction site below.

// ---------------------------------------------------------------------------
// generation.generate tool hook
// ---------------------------------------------------------------------------
//
// Adapts the shared tool surface's `generation.generate` handler onto the
// composed GenerativeMediaCoordinator (Requirement 6). It maps the JSON tool
// arguments to a GenerationRequest + GenerationPlacement, runs the coordinator's
// validate -> gate -> generate -> place pipeline, and renders the placement back
// as JSON. Any coordinator error (unconfigured backend, unauthorized, invalid
// prompt/bounds) is forwarded verbatim so the executor's rollback/error policy
// applies uniformly.
[[nodiscard]] services::Tool::Handler makeGenerateHook(
    services::GenerativeMediaCoordinator& coordinator) {
    return [&coordinator](const services::Json& input) -> Result<services::Json> {
        using namespace palmier::services;

        GenerationRequest request;
        request.model = input.stringOr("model");
        request.prompt = input.stringOr("prompt");
        request.mediaType = (input.stringOr("mediaType", "video") == "image")
                                ? GenerationMediaType::Image
                                : GenerationMediaType::Video;
        if (const Json* params = input.find("params"); params != nullptr && params->isObject()) {
            for (const auto& [key, value] : params->asObject()) {
                if (value.isString()) request.params.emplace(key, value.asString());
            }
        }

        GenerationPlacement placement;
        const std::optional<Uuid> trackId = Uuid::parse(input.stringOr("trackId"));
        if (!trackId.has_value()) {
            return err<Json>(invalidArgument(
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

}  // namespace

// ---------------------------------------------------------------------------
// Construction — wire everything together (no network activity).
// ---------------------------------------------------------------------------

ApplicationComposition::ApplicationComposition(AppConfig config)
    : mcpHost_(config.mcpHost.empty() ? std::string(services::McpServer::kDefaultHost)
                                      : config.mcpHost),
      mcpPort_(config.mcpPort) {
    // --- GPU abstraction layer (software fallback guaranteed) --------------
    // GpuContext::create never fails for "no GPU"; it degrades to the CPU path.
    Result<gpu::GpuContext> gpuResult = gpu::GpuContext::create(config.gpuPolicy);
    if (gpuResult.isError()) {
        gpu_ = std::make_unique<gpu::GpuContext>(gpu::GpuContext::softwareFallback());
    } else {
        gpu_ = std::make_unique<gpu::GpuContext>(std::move(gpuResult).value());
    }

    // --- The one project session (Requirement 1.1; design.md D1) -----------
    // It owns the single TimelineEngine and the current project's MediaManager
    // library for the application's lifetime, and starts on an empty project at
    // the documented defaults (Requirement 1.10). Every consumer below takes a
    // reference to those owned components rather than constructing its own.
    session_ = std::make_unique<services::ProjectSession>();

    // --- Playback_Engine (task 7.5; Requirements 1.1, 5.2-5.10) ------------
    // Exactly one Compositor over the one GpuContext; exactly one decoder
    // teardown queue (shared with the audio decoder from stage 8 onward, upstream
    // PR 405); exactly one decoder-backed clip frame provider installed as the
    // compositor's source of pixels; exactly one PreviewController transport,
    // reading the current project from the one session's engine snapshot so a
    // project.create / project.open is picked up with no rewiring.
    compositor_ = std::make_unique<gpu::Compositor>(*gpu_);
    decoderTeardown_ = std::make_unique<media::DecoderTeardownQueue>();
    clipFrameProvider_ = std::make_unique<media::DecoderClipFrameProvider>(
        *decoderTeardown_, media::ffmpegDecodeBackendFactory());
    compositor_->setFrameProvider(clipFrameProvider_->asProvider());
    playbackEngine_ = std::make_unique<ui::PreviewController>(
        *compositor_, *gpu_,
        [session = session_.get()]() { return session->engine().snapshot(); });

    // --- Audio_Engine (task 8.7; Requirements 1.1, 6.3, 6.7) ---------------
    // Startup sink selection in the design's order PipeWire -> ALSA -> Null. A
    // candidate is selected only if it actually opens, so a build with PipeWire
    // compiled in on a host with no daemon still falls through to ALSA and then to
    // the null sink; the notice explains which and why, and is non-empty exactly
    // when no real device was opened (Requirement 6.7).
    const double audioFps = session_->engine().snapshot().timelineFps.toDouble();
    media::AudioSinkSelectorOptions sinkOptions;
    sinkOptions.projectFrameRateFps = audioFps;
    media::AudioSinkSelection sinkSelection = media::selectAudioSink(std::move(sinkOptions));

    audioSinkName_ = sinkSelection.name;
    audioOutputAvailable_ = sinkSelection.realDevice;
    audioUnavailableNotice_ = sinkSelection.notice.value_or(std::string{});

    // The engine's ProjectProvider is a BORROW of the current project, refreshed
    // from the one session on every read. It is a callable rather than a
    // `ProjectSession&` on purpose: `Palmier::services` links `Palmier::media`, so
    // taking the session type inside the audio engine would invert that dependency
    // (see media/AudioEngine.hpp). Re-reading per mixed quantum is what bounds the
    // mute/gain latency of Requirement 6.4 without moving the playhead.
    audioProject_ = std::make_unique<Project>();
    media::AudioEngineOptions audioOptions;
    audioOptions.quantumFrames = sinkSelection.config.quantumFrames;
    audioEngine_ = std::make_unique<media::AudioEngine>(
        [session = session_.get(), project = audioProject_.get()]() -> const Project* {
            *project = session->engine().snapshot();
            return project;
        },
        std::move(sinkSelection.sink), *decoderTeardown_, media::ffmpegDecodeBackendFactory(),
        audioOptions);

    // The sink is the master clock (design.md D7; Requirement 6.3). The clock is
    // installed as an OPTIONAL seam and reports a position only while the engine is
    // running, so a halted transport — and a session where audio output was
    // unavailable and the engine was never started — paces video off the wall clock
    // exactly as it did before this stage.
    playbackEngine_->setAudioMasterClock(
        [engine = audioEngine_.get()]() -> std::optional<Duration> {
            if (!engine->running()) return std::nullopt;
            return engine->presentationPosition();
        });

    // --- Project I/O -------------------------------------------------------
    saveService_ = std::make_unique<services::ProjectSaveService>();

    // --- Auth stack: secret store -> BYOK manager -> auth service ----------
    if (config.secretStore != nullptr) {
        secretStore_ = config.secretStore;
    } else {
        ownedSecretStore_ = std::make_unique<services::InMemorySecretStore>();
        secretStore_ = ownedSecretStore_.get();
    }

    if (config.byokValidator != nullptr) {
        byokValidator_ = config.byokValidator;
    } else {
        ownedByokValidator_ = std::make_unique<OfflineByokValidator>();
        byokValidator_ = ownedByokValidator_.get();
    }

    byokManager_ =
        std::make_unique<services::ByokCredentialManager>(*byokValidator_, *secretStore_);

    if (config.authBackend != nullptr) {
        authBackend_ = config.authBackend;
    } else {
        ownedAuthBackend_ = std::make_unique<OfflineAuthBackend>();
        authBackend_ = ownedAuthBackend_.get();
    }

    auth_ = std::make_unique<services::AuthenticationService>(*authBackend_);
    auth_->setByokManager(*byokManager_);

    // --- Generative pipeline ----------------------------------------------
    if (config.generativeBackend != nullptr) {
        generativeBackend_ = config.generativeBackend;
    } else {
        ownedGenerativeBackend_ = std::make_unique<OfflineGenerativeBackend>();
        generativeBackend_ = ownedGenerativeBackend_.get();
    }

    genClient_ = std::make_unique<services::GenerativeClient>(*generativeBackend_);
    genGate_ = std::make_unique<services::AuthServiceGenerationGate>(*auth_);
    genRunner_ = std::make_unique<services::GenerativeClientRunner>(*genClient_);
    placer_ = std::make_unique<services::TimelineEnginePlacer>(session_->engine());
    genCoordinator_ = std::make_unique<services::GenerativeMediaCoordinator>(
        *genGate_, *genRunner_, session_->mediaLibrary(), *placer_);

    // --- Shared tool surface (UI == MCP == agent path; Property P4) --------
    // The generate tool is wired to the composed coordinator; the export tool is
    // left to the Export Engine (task 10.x) — absent that hook the tool is still
    // advertised and reports Unsupported when invoked, keeping the tool surface
    // identical across configurations.
    services::ToolRegistryHooks hooks;
    hooks.generate = makeGenerateHook(*genCoordinator_);
    toolRegistry_ = std::make_unique<services::ToolRegistry>(
        services::buildDefaultToolRegistry(*session_, std::move(hooks)));

    // --- MCP execution policy + HTTP transport -----------------------------
    // The executor is pointed at the SAME session the registry's handlers resolve
    // their engine from, so both always act on the current project (design.md D1).
    executor_ = std::make_unique<services::McpToolExecutor>(*toolRegistry_, session_.get());

    // The server delegates each well-formed request to the executor's JSON
    // envelope entry point; the executor enforces the timeout/rollback policy.
    services::McpToolExecutor* executor = executor_.get();
    mcpServer_ = std::make_unique<services::McpServer>(
        [executor](const services::Json& request) { return executor->execute(request); });

    // --- Remote access gate (task 6.3; Requirements 10.1-10.13) ------------
    // The gate is constructed unconditionally and wired into the transport
    // unconditionally, because that is what makes the compatibility guarantee
    // structural rather than conditional: with remote access off (the default) its
    // bind decision is loopback-only and `admit()` allows every request, so the
    // endpoint behaves exactly as it did before this stage — a request carrying
    // neither Authorization nor Origin is served, and no 401 or 403 can be produced
    // (Requirements 10.1, 10.10).
    rejectionLog_ = std::make_unique<services::StreamRejectionLog>(std::clog);
    remoteAccessGate_ =
        std::make_unique<services::RemoteAccessGate>(config.remote, *rejectionLog_);
    mcpServer_->setRemoteAccessGate(remoteAccessGate_.get());

    // --- Agent interpreter selection (task 10.1; Requirements 11.1, 11.8) --
    //
    // Exactly one implementation, named by a configuration string, exposed through
    // `agentInterpreterId()`. An explicitly injected `config.agentInterpreter`
    // still wins — that is the seam tests and the GUI shell use to supply a
    // scripted or model-backed interpreter directly — and in that case the reported
    // id is the configured one, because the injected implementation is what the
    // configuration asked for.
    //
    // Otherwise the registry decides. It never fails: an unknown id, or a
    // `hosted`/`byok` whose credentials are absent, installs the offline
    // interpreter and records the reason in `startupErrors()` while every other
    // component is still constructed (the policy design.md D9 states, and the same
    // one Requirement 12.8 states for the generative backend).
    services::AgentInterpreterRequest interpreterRequest;
    interpreterRequest.id = config.agentInterpreterId;
    interpreterRequest.offlineOptions.context =
        services::makeSessionEditorContextProvider(*session_);
    // The credential probe reads the composed auth stack rather than the network:
    // `hosted` needs an active subscription entitlement, `byok` an authorized
    // provider credential among the configured ids. Neither call contacts anything.
    interpreterRequest.credentials = [this, providers = config.byokProviders](
                                         std::string_view id) -> bool {
        if (id == services::kAgentInterpreterHosted) {
            const std::optional<services::Session>& current = auth_->currentSession();
            return current.has_value() &&
                   current->entitlement == services::EntitlementStatus::Active;
        }
        if (id == services::kAgentInterpreterByok) {
            for (const std::string& provider : providers) {
                if (auth_->isByokAuthorized(provider)) return true;
            }
        }
        return false;
    };

    services::AgentInterpreterSelection interpreter =
        services::selectAgentInterpreter(interpreterRequest);
    if (config.agentInterpreter) {
        agentInterpreterId_ = config.agentInterpreterId.empty()
                                  ? std::string(services::defaultAgentInterpreterId())
                                  : config.agentInterpreterId;
    } else {
        agentInterpreterId_ = interpreter.id;
        if (!interpreter.startupError.empty()) {
            startupErrors_.push_back(interpreter.startupError);
        }
    }

    // --- In-app agent chat: reuse the SAME executor + registry -------------
    agentGate_ = std::make_unique<services::AuthServiceAgentGate>(*auth_, config.byokProviders);
    agent_ = std::make_unique<services::AgentOrchestrator>(
        *executor_, *agentGate_,
        config.agentInterpreter ? config.agentInterpreter : std::move(interpreter.interpreter),
        services::makeMentionPreprocessor(session_->mediaLibrary()));

    // --- Localization ------------------------------------------------------
    services::LocalizationManager::Config locConfig;
    locConfig.translations = std::move(config.translations);
    locConfig.store = config.languageStore;
    locConfig.systemLanguage = config.systemLanguage
                                   ? config.systemLanguage
                                   : services::makeEnvSystemLanguageProvider();
    localization_ = std::make_unique<services::LocalizationManager>(std::move(locConfig));
}

ApplicationComposition::~ApplicationComposition() {
    // Stop the MCP server (join its accept thread) before the executor and the
    // rest of the graph are torn down (Requirement 7.9).
    stop();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

Result<void> ApplicationComposition::start() {
    const services::BindDecision decision = remoteAccessGate_->validate();

    // Requirement 10.3/10.12: an enabled-but-incomplete remote configuration is a
    // startup ERROR that names every unmet prerequisite — and never the token — yet
    // it does not stop the editor: the endpoint simply comes up on loopback.
    remoteAccessStartupError_.clear();
    if (!decision.unmetPrerequisites.empty()) {
        remoteAccessStartupError_ =
            "Remote MCP access is enabled but was not started: unmet prerequisites — ";
        for (std::size_t i = 0; i < decision.unmetPrerequisites.size(); ++i) {
            if (i > 0) remoteAccessStartupError_ += "; ";
            remoteAccessStartupError_ += decision.unmetPrerequisites[i];
        }
        remoteAccessStartupError_ += ". The endpoint is bound to loopback instead.";
        std::clog << remoteAccessStartupError_ << '\n';
    }

    // Requirement 10.7: exactly one warning, emitted before the first request is
    // accepted — hence here, before the listener starts, and taken from the gate so
    // repeated starts cannot repeat it.
    if (std::optional<std::string> warning = remoteAccessGate_->takeStartupWarning();
        warning.has_value()) {
        remoteAccessWarning_ = *warning;
        std::clog << remoteAccessWarning_ << '\n';
    }

    if (decision.loopbackOnly) {
        // Requirements 7.1/7.2/7.3 unchanged: bind the configured loopback endpoint
        // and begin accepting; a port-in-use failure is forwarded and the server
        // stays stopped. The composition's own host/port is used here rather than the
        // decision's so an ephemeral test port keeps working.
        return mcpServer_->start(mcpHost_, mcpPort_);
    }

    if (decision.tlsEnabled) {
        // The gate has already established that the material loads and matches, so a
        // failure here would mean the two disagreed — refuse rather than downgrade.
        if (Result<void> material = mcpServer_->setTlsMaterial(
                *remoteAccessGate_->config().tlsCertificate,
                *remoteAccessGate_->config().tlsPrivateKey);
            material.isError()) {
            return material;
        }
    }
    return mcpServer_->start(decision);
}

void ApplicationComposition::stop() {
    if (mcpServer_) {
        mcpServer_->stop();
    }
}

bool ApplicationComposition::running() const noexcept {
    return mcpServer_ && mcpServer_->running();
}

std::uint16_t ApplicationComposition::mcpBoundPort() const noexcept {
    return mcpServer_ ? mcpServer_->boundPort() : 0;
}

// ---------------------------------------------------------------------------
// Component accessors that reach into the session (out of line so the header
// needs no ProjectSession definition).
// ---------------------------------------------------------------------------

services::ProjectSession& ApplicationComposition::projectSession() noexcept {
    return *session_;
}

TimelineEngine& ApplicationComposition::timeline() noexcept {
    return session_->engine();
}

MediaManager& ApplicationComposition::mediaLibrary() noexcept {
    return session_->mediaLibrary();
}

const std::string& ApplicationComposition::softwareCompositingNotice() const noexcept {
    static const std::string kEmpty;
    return playbackEngine_ ? playbackEngine_->softwareCompositingNotice() : kEmpty;
}

std::string ApplicationComposition::gpuUnavailableNotice() const {
    if (gpu_) {
        const std::optional<std::string>& notice = gpu_->unavailableNotice();
        if (notice.has_value()) return *notice;
    }
    return {};
}

}  // namespace palmier::app
