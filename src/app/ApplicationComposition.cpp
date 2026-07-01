// SPDX-License-Identifier: GPL-3.0-or-later
//
// app/ApplicationComposition.cpp — implementation of the application composition
// root (task 21.1; Requirements 1.6, 7.1, 7.2, 7.9, 13.3).
//
// Constructs every component in dependency order and connects them:
//   secret store -> BYOK manager -> auth service
//   auth service -> generation gate + agent gate
//   generative backend -> generative client -> generation runner
//   timeline engine -> timeline placer -> generative coordinator
//   timeline engine (+ generate hook) -> shared ToolRegistry
//   ToolRegistry -> McpToolExecutor -> McpServer (handler) and AgentOrchestrator
//
// Network-facing backends absent from the config get an OFFLINE default that
// reports the feature as unconfigured without touching the network, so the
// editor and the MCP server come up and run with no network connection
// (Requirement 13.3). The composition itself performs no I/O beyond the GPU
// device probe (which degrades to software) and reading the localization seams.

#include "app/ApplicationComposition.hpp"

#include <map>
#include <string>
#include <string_view>
#include <utility>

#include "core/Duration.hpp"
#include "core/Error.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/MediaManager.hpp"
#include "core/Result.hpp"
#include "core/TimelineEngine.hpp"
#include "core/Uuid.hpp"

#include "gpu/GpuContext.hpp"

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

// The default agent interpreter: the natural-language -> tool mapping (an LLM in
// production) is not wired in a bare composition, so report it as unconfigured.
// The editor, MCP server, and manual/explicit tool calls remain fully functional.
[[nodiscard]] services::IntentInterpreter makeUnconfiguredInterpreter() {
    return [](std::string_view) -> Result<services::AgentIntent> {
        return err<services::AgentIntent>(makeError(
            ErrorCode::FailedPrecondition,
            "the in-app agent's intent interpreter is not configured"));
    };
}

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

    // --- Domain core -------------------------------------------------------
    timeline_ = std::make_unique<TimelineEngine>();
    mediaLibrary_ = std::make_unique<MediaManager>();

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
    placer_ = std::make_unique<services::TimelineEnginePlacer>(*timeline_);
    genCoordinator_ = std::make_unique<services::GenerativeMediaCoordinator>(
        *genGate_, *genRunner_, *mediaLibrary_, *placer_);

    // --- Shared tool surface (UI == MCP == agent path; Property P4) --------
    // The generate tool is wired to the composed coordinator; the export tool is
    // left to the Export Engine (task 10.x) — absent that hook the tool is still
    // advertised and reports Unsupported when invoked, keeping the tool surface
    // identical across configurations.
    services::ToolRegistryHooks hooks;
    hooks.generate = makeGenerateHook(*genCoordinator_);
    toolRegistry_ = std::make_unique<services::ToolRegistry>(
        services::buildDefaultToolRegistry(*timeline_, std::move(hooks)));

    // --- MCP execution policy + HTTP transport -----------------------------
    executor_ = std::make_unique<services::McpToolExecutor>(*toolRegistry_, timeline_.get());

    // The server delegates each well-formed request to the executor's JSON
    // envelope entry point; the executor enforces the timeout/rollback policy.
    services::McpToolExecutor* executor = executor_.get();
    mcpServer_ = std::make_unique<services::McpServer>(
        [executor](const services::Json& request) { return executor->execute(request); });

    // --- In-app agent chat: reuse the SAME executor + registry -------------
    agentGate_ = std::make_unique<services::AuthServiceAgentGate>(*auth_, config.byokProviders);
    agent_ = std::make_unique<services::AgentOrchestrator>(
        *executor_, *agentGate_,
        config.agentInterpreter ? config.agentInterpreter : makeUnconfiguredInterpreter(),
        services::makeMentionPreprocessor(*mediaLibrary_));

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
    // Requirements 7.1/7.2/7.3: bind the loopback endpoint and begin accepting;
    // a port-in-use failure is forwarded and the server stays stopped.
    return mcpServer_->start(mcpHost_, mcpPort_);
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

std::string ApplicationComposition::gpuUnavailableNotice() const {
    if (gpu_) {
        const std::optional<std::string>& notice = gpu_->unavailableNotice();
        if (notice.has_value()) return *notice;
    }
    return {};
}

}  // namespace palmier::app
