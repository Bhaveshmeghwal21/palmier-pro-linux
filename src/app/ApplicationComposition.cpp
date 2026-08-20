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
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <algorithm>
#include <utility>
#include <vector>

#include "core/Duration.hpp"
#include "core/Error.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/MediaManager.hpp"
#include "core/Project.hpp"
#include "core/Resolution.hpp"
#include "core/Result.hpp"
#include "core/TimelineEngine.hpp"
#include "core/Uuid.hpp"

#include "gpu/CodecBridge.hpp"
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
#include "services/ExportCoordinator.hpp"
#include "services/GenerativeClient.hpp"
#include "services/GenerationModelCatalog.hpp"
#include "services/GenerativeMediaCoordinator.hpp"
#include "services/OpenSslGenerativeHttpTransport.hpp"
#include "services/Json.hpp"
#include "services/MediaImportService.hpp"
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

// The generative backend is no longer a local stub. Task 10.5 replaced this
// file's private `OfflineGenerativeBackend` with
// `services::selectGenerativeBackend()`, which compiles all three backends in —
// `offline`, `hosted`, `byok` — selects one by configuration string with no
// recompilation, and falls back to the offline STUB (a named, tested component
// that holds no transport at all) whenever the requested id is unknown or its
// credentials are absent (Requirements 12.1, 12.2, 12.8). See the construction
// site below.

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
// Every `generation.generate` invocation — from the tool surface, from the MCP
// endpoint and from the in-app agent — arrives HERE, because all three dispatch
// through the one `ToolRegistry` this hook is registered on (Requirement 12.1).
// That is why the Requirement 12.4 rejection is implemented as the hook's first
// act rather than deeper down: asking the SELECTED backend for its unmet
// precondition costs one secret-store read and no network activity, and returning
// before the coordinator runs is what makes "no media library entry, no clip and
// no undo-history entry" true by construction rather than by inspection.
//
// `backend` is null only when the caller injected a raw `IGenerativeBackend`; in
// that case the injected backend answers for itself and the coordinator runs as
// before.
[[nodiscard]] services::Tool::Handler makeGenerateHook(
    services::GenerativeMediaCoordinator& coordinator,
    const services::GenerativeBackend* backend) {
    return [&coordinator, backend](const services::Json& input) -> Result<services::Json> {
        using namespace palmier::services;

        if (backend != nullptr) {
            const std::string unmet = backend->unmetPrecondition();
            if (!unmet.empty()) {
                return err<Json>(failedPrecondition(unmet));
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
        request.mode = generationModeFromStringView(input.stringOr("mode", "generate"))
                          .value_or(GenerationMode::Generate);
        if (const std::optional<Uuid> sourceAssetId =
                Uuid::parse(input.stringOr("sourceAssetId"));
            sourceAssetId.has_value()) {
            request.sourceAssetId = *sourceAssetId;
        }
        const std::int64_t targetWidth = input.intOr("targetWidth", 0);
        const std::int64_t targetHeight = input.intOr("targetHeight", 0);
        if (targetWidth > 0 && targetHeight > 0) {
            request.targetResolution = Resolution{static_cast<std::uint32_t>(targetWidth),
                                                  static_cast<std::uint32_t>(targetHeight)};
        }
        request.requestedDuration =
            Duration::fromNanoseconds(input.intOr("requestedDurationTicks", 0));
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

/// The `generation.list_models` hook (usable-editor Phase 2 task 7; PR 406):
/// render the catalog's models grouped by provider. Pure and network-free — the
/// catalog is fixed, in-tree data — so this hook needs no precondition check the
/// way `makeGenerateHook` needs the backend's `unmetPrecondition()`: listing what
/// models EXIST is independent of whether the currently selected backend can
/// currently reach one of them.
[[nodiscard]] services::Tool::Handler makeListModelsHook(
    const services::GenerationModelCatalog& catalog) {
    return [&catalog](const services::Json&) -> Result<services::Json> {
        using namespace palmier::services;

        // Group by provider while preserving each provider's first-seen order,
        // so the response is deterministic for a fixed catalog without depending
        // on any particular sort. Grouped in a plain map first (Json::find has no
        // mutable overload) and converted to the response's Json shape once.
        std::vector<std::pair<std::string, Json::Array>> byProvider;
        for (const CatalogModel& model : catalog.listModels()) {
            Json entry = Json::object();
            entry.set("id", model.id);
            entry.set("mediaType", std::string(toStringView(model.mediaType)));
            entry.set("servesUpscale", model.servesUpscale);
            if (model.audioDurationRange.has_value()) {
                const auto& [minDuration, maxDuration] = *model.audioDurationRange;
                entry.set("minDurationTicks",
                         static_cast<std::int64_t>(minDuration.ticks()));
                entry.set("maxDurationTicks",
                         static_cast<std::int64_t>(maxDuration.ticks()));
            }

            const auto existing =
                std::find_if(byProvider.begin(), byProvider.end(),
                            [&model](const auto& p) { return p.first == model.provider; });
            if (existing != byProvider.end()) {
                existing->second.push_back(std::move(entry));
            } else {
                byProvider.emplace_back(model.provider, Json::Array{std::move(entry)});
            }
        }

        Json providers = Json::object();
        for (auto& [provider, models] : byProvider) {
            providers.set(provider, Json::array(std::move(models)));
        }

        Json out = Json::object();
        out.set("providers", std::move(providers));
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

    // --- Export_Coordinator (task 9.7; Requirements 1.1, 7.1-7.11) ---------
    //
    // Exactly ONE coordinator, over the one session, the one live GpuContext and the
    // one decoder teardown queue. Constructing it here rather than per export is
    // what makes Requirement 7.10 — "an export is already in progress" — a fact
    // about the application rather than about one caller: the GUI dialog and the
    // `timeline.export` tool hold the same object and therefore the same single
    // export slot.
    //
    // The coordinator still isolates each export exactly as before: the worker
    // takes a VALUE-COPY project snapshot and builds its own GPU context,
    // compositor, decoders and encoder, so nothing constructed above is shared with
    // it and no export can perturb playback or the project (Requirement 7.2).
    //
    // The one collaborator worth binding here is the audio mix: absent an override,
    // the export renders its audio through the composed AudioEngine's `renderRange`,
    // so an export contains the same mix playback produces (Requirement 6.5). It is
    // only a default — a caller that supplied its own renderer keeps it.
    services::ExportCoordinatorOptions exportOptions = std::move(config.exportOptions);
    if (!exportOptions.audioRenderer) {
        exportOptions.audioRenderer = [engine = audioEngine_.get()](
                                          const Project& project, Duration from, Duration to) {
            return engine->renderRange(project, from, to);
        };
    }
    exportCoordinator_ = std::make_unique<services::ExportCoordinator>(
        *session_, *gpu_, *decoderTeardown_, std::move(exportOptions));

    // --- Media_Import_Service (task 9.7; Requirements 1.1, 2.1-2.9) --------
    //
    // Exactly one service, importing into the one session's media library. The
    // probe backend is injectable so `media.import` is exercisable with no media
    // fixture; absent an injection it is the real FFmpeg prober.
    mediaImportService_ = std::make_unique<services::MediaImportService>(
        *session_,
        config.mediaProbeBackend ? config.mediaProbeBackend : media::ffmpegProbeBackend());

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

    // --- Generative backend selection (task 10.5; Requirements 12.1, 12.2, 12.8)
    //
    // Exactly one implementation, named by a configuration string, exposed through
    // `generativeBackendId()`. All three are compiled into this binary, so moving
    // between them is a configuration change and never a rebuild
    // (Requirement 12.2).
    //
    // An explicitly injected `config.generativeBackend` still wins — that is the
    // seam the existing tests use — and in that case the reported id is the
    // configured one, exactly as the agent interpreter behaves for an injected
    // implementation.
    //
    // Otherwise the registry decides, and it never fails: an unknown id, or a
    // `hosted`/`byok` whose credentials are absent, installs the offline stub,
    // records the reason in `startupErrors()` naming the rejected id and the unmet
    // requirement, and leaves every other component constructed (Requirement 12.8).
    // Requirement 12.5 is the other half: with the stub in force, timeline editing,
    // playback, save, open, export and the MCP endpoint are untouched by any of
    // this — nothing below reaches into them.
    //
    // The credential probe reads the composed auth stack, never the network, and is
    // the same question the agent interpreter's probe asks.
    // At a cold start nobody has signed in, so the auth-stack answer is "no
    // credentials" and both registries fall back. `config.featureCredentials` is the
    // override a shell that restored a session — or a test that wants the selected
    // client rather than the fallback — supplies instead.
    std::function<bool(std::string_view)> credentialProbe = config.featureCredentials;
    if (!credentialProbe) {
        credentialProbe = [this, providers = config.byokProviders](
                              std::string_view id) -> bool {
            if (id == services::kGenerativeBackendHosted ||
                id == services::kAgentInterpreterHosted) {
                const std::optional<services::Session>& current = auth_->currentSession();
                return current.has_value() &&
                       current->entitlement == services::EntitlementStatus::Active;
            }
            if (id == services::kGenerativeBackendByok ||
                id == services::kAgentInterpreterByok) {
                for (const std::string& provider : providers) {
                    if (auth_->isByokAuthorized(provider)) return true;
                }
            }
            return false;
        };
    }

    if (config.generativeBackend != nullptr) {
        generativeBackend_ = config.generativeBackend;
        generativeBackendId_ = config.generativeBackendId.empty()
                                   ? std::string(services::defaultGenerativeBackendId())
                                   : config.generativeBackendId;
    } else {
        services::GenerativeBackendRequest backendRequest;
        backendRequest.id = config.generativeBackendId;
        backendRequest.credentials = credentialProbe;
        backendRequest.secretStore = secretStore_;
        // usable-editor spec Phase 2, task 6 (Requirement 11.5): install a real
        // HTTPS transport by default rather than leaving `hosted`/`byok`
        // permanently unreachable. A caller that injected its OWN transport
        // (config.generativeTransport != nullptr — every existing test that
        // exercises this path) is honoured unchanged; only the previously-dead
        // "nothing was injected" default changes, and only on a build compiled
        // with PALMIER_HAVE_OPENSSL, so a build without TLS support keeps
        // falling back to the unavailable transport exactly as before
        // (Requirement 11.5's own "falls back... only where the build excludes
        // TLS support").
        if (config.generativeTransport != nullptr) {
            backendRequest.transport = config.generativeTransport;
        } else if (services::openSslGenerativeHttpTransportAvailable()) {
            ownedGenerativeTransport_ = services::makeOpenSslGenerativeHttpTransport();
            backendRequest.transport = ownedGenerativeTransport_.get();
        }
        backendRequest.endpoint = config.generativeEndpoint;
        // The provider whose stored key `byok` reads: the first configured one that
        // is authorized. No key VALUE is involved here, only a provider name.
        for (const std::string& provider : config.byokProviders) {
            if (auth_->isByokAuthorized(provider)) {
                backendRequest.byokProvider = provider;
                break;
            }
        }
        if (backendRequest.byokProvider.empty() && !config.byokProviders.empty()) {
            // None is authorized yet — a cold start, or an injected probe answered
            // for the auth stack. Name the first configured provider so the client
            // reads the right key once one is filed, rather than reporting "no
            // provider is configured" for a build that plainly configured one.
            backendRequest.byokProvider = config.byokProviders.front();
        }

        services::GenerativeBackendSelection selection =
            services::selectGenerativeBackend(backendRequest);
        generativeBackendId_ = selection.id;
        if (!selection.startupError.empty()) {
            startupErrors_.push_back(selection.startupError);
        }
        selectedGenerativeBackend_ = selection.backend.get();
        ownedGenerativeBackend_ = std::move(selection.backend);
        generativeBackend_ = ownedGenerativeBackend_.get();
    }

    genClient_ = std::make_unique<services::GenerativeClient>(*generativeBackend_);
    genGate_ = std::make_unique<services::AuthServiceGenerationGate>(*auth_);
    genRunner_ = std::make_unique<services::GenerativeClientRunner>(*genClient_);
    placer_ = std::make_unique<services::TimelineEnginePlacer>(session_->engine());
    genCatalog_ = std::make_unique<services::GenerationModelCatalog>();
    genCoordinator_ = std::make_unique<services::GenerativeMediaCoordinator>(
        *genGate_, *genRunner_, session_->mediaLibrary(), *placer_, genCatalog_.get());

    // --- Shared tool surface (UI == MCP == agent path; Property P4) --------
    // Three hooks, all bound to the single instances constructed above: the
    // generative coordinator, the Export_Coordinator (task 9.7) and the
    // Media_Import_Service. With these wired, the whole headless sequence of
    // Requirement 3.6 — project.create, media.import, timeline.add_track,
    // timeline.add_clip, project.save, project.open, timeline.export — runs through
    // this one registry, which is the same registry the GUI and the MCP endpoint
    // dispatch through (Requirements 3.1, 3.6, 7.2).
    //
    // The export hook is `services::makeExportToolHandler`, so the translation from
    // tool arguments to an `ExportRequest2` and from an `ExportOutcome` to the tool
    // result lives beside the coordinator and is the same for every caller.
    services::ToolRegistryHooks hooks;
    hooks.generate = makeGenerateHook(*genCoordinator_, selectedGenerativeBackend_);
    hooks.listModels = makeListModelsHook(*genCatalog_);
    hooks.exportTimeline = services::makeExportToolHandler(*exportCoordinator_, *session_,
                                                           config.exportToolOptions);
    hooks.importMedia = [service = mediaImportService_.get()](const std::filesystem::path& path) {
        return service->import(path);
    };
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
    // It is the SAME probe the generative backend selection above used, because it
    // is the same question about the same auth stack — two copies of it could
    // disagree, and then `hosted` would be selectable for one feature and not the
    // other for no stated reason.
    interpreterRequest.credentials = credentialProbe;

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

// ---------------------------------------------------------------------------
// Codec backend report (task 9.7; Requirement 8.7)
// ---------------------------------------------------------------------------
//
// Two booleans per backend, and nothing else happens. `compiledIn` is the
// build-time `PALMIER_HAVE_*` state, read through
// `gpu::BridgeAvailability::fromBuildConfig()`. `usableOnHost` is that AND the
// capabilities the ONE GpuContext already probed at construction: the vendor it
// selected and whether that device reports hardware encode. The FFmpeg software
// backend is reported as both compiled in and usable unconditionally, because
// software encoding is what every other path degrades to — a build in which it
// were unavailable could not export at all.
//
// This deliberately runs NO probe of its own. `media::EncoderSelector` owns the
// bounded capability probe, and starting a second one here would be both slower
// than the requirement's 3000 ms ceiling deserves and a second definition of "is
// this device usable". Reading already-probed capabilities is what makes the two
// halves of Requirement 8.7 — inside 3000 ms, and changing no encoder selection or
// export state — true by construction rather than by care: this function is const,
// mentions neither the coordinator nor the selector, and allocates only its answer.

std::vector<ApplicationComposition::CodecBackendStatus>
ApplicationComposition::codecBackendReport() const {
    const gpu::BridgeAvailability availability = gpu::BridgeAvailability::fromBuildConfig();
    const gpu::GpuCaps            caps =
        gpu_ ? gpu_->capabilities() : gpu::GpuCaps::software();

    // One shared explanation, so all three vendor entries answer the same question
    // the same way: compiled in? then is this device that vendor, and does it report
    // hardware encode?
    const auto vendorStatus = [&caps](std::string name, bool compiledIn,
                                      const char* sdkName,
                                      std::initializer_list<gpu::GpuVendor> vendors) {
        CodecBackendStatus status;
        status.name = std::move(name);
        status.compiledIn = compiledIn;
        if (!compiledIn) {
            status.usableOnHost = false;
            status.detail = std::string(sdkName) +
                            " was not found at configure time, so this path is not compiled in";
            return status;
        }
        bool vendorMatches = false;
        for (const gpu::GpuVendor vendor : vendors) {
            if (caps.vendorId == vendor) vendorMatches = true;
        }
        if (!vendorMatches) {
            status.usableOnHost = false;
            status.detail = "compiled in, but the selected device vendor is " + caps.vendor;
            return status;
        }
        if (!caps.hwEncode) {
            status.usableOnHost = false;
            status.detail = "compiled in, but the selected " + caps.vendor +
                            " device reports no hardware encode";
            return status;
        }
        status.usableOnHost = true;
        status.detail = "compiled in and usable on the selected " + caps.vendor + " device";
        return status;
    };

    std::vector<CodecBackendStatus> report;
    report.reserve(4);
    report.push_back(vendorStatus("vaapi", availability.vaapi, "libva",
                                  {gpu::GpuVendor::AMD, gpu::GpuVendor::Intel}));
    report.push_back(vendorStatus("nvenc-nvdec", availability.nvenc && availability.nvdec,
                                  "nv-codec-headers", {gpu::GpuVendor::NVIDIA}));
    report.push_back(vendorStatus("qsv", availability.quickSync, "oneVPL/libmfx",
                                  {gpu::GpuVendor::Intel}));

    CodecBackendStatus software;
    software.name = "ffmpeg-software";
    // Requirement 8.7: always both. Asserted from the availability value as well,
    // so a build that somehow reported otherwise would be visible rather than
    // papered over by this hard-coded true.
    software.compiledIn = true;
    software.usableOnHost = true;
    software.detail = availability.ffmpegSoftware
                          ? "always compiled in and always usable: the CPU encode fallback"
                          : "always compiled in and always usable: the CPU encode fallback "
                            "(build reported the software bridge as absent, which cannot "
                            "disable software encoding)";
    report.push_back(std::move(software));
    return report;
}

std::string ApplicationComposition::gpuUnavailableNotice() const {
    if (gpu_) {
        const std::optional<std::string>& notice = gpu_->unavailableNotice();
        if (notice.has_value()) return *notice;
    }
    return {};
}

std::string ApplicationComposition::generationUnmetPrecondition() const {
    // One secret-store read at most, no network. Safe to call from a paint path,
    // which is what Requirement 12.5's non-dismissable indication needs.
    if (selectedGenerativeBackend_ == nullptr) return {};
    return selectedGenerativeBackend_->unmetPrecondition();
}

}  // namespace palmier::app
