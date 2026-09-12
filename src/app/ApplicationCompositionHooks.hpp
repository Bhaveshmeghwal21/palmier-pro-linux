// SPDX-License-Identifier: GPL-3.0-or-later
//
// app/ApplicationCompositionHooks.hpp - the tool handlers the composition root injects,
// and the offline stand-ins it falls back to.
//
// Separated from app/ApplicationComposition.cpp so that file is only wiring. What lives here
// is adapter code: each hook translates a JSON tool invocation onto one composed component,
// and there is one per tool whose handler cannot be built without a component the composition
// root owns. They are declared in `detail` rather than an anonymous namespace only because
// they are now defined in a different translation unit from their single caller.

#ifndef PALMIER_APP_APPLICATIONCOMPOSITIONHOOKS_HPP
#define PALMIER_APP_APPLICATIONCOMPOSITIONHOOKS_HPP

#include "core/Error.hpp"
#include "core/Result.hpp"
#include "gpu/Compositor.hpp"
#include "services/AuthenticationService.hpp"
#include "services/ByokCredentialManager.hpp"
#include "services/ByokCredentials.hpp"
#include "services/GenerationModelCatalog.hpp"
#include "services/GenerativeBackendRegistry.hpp"
#include "services/GenerativeClient.hpp"
#include "services/GenerativeMediaCoordinator.hpp"
#include "services/MediaImportService.hpp"
#include "services/ProjectSession.hpp"
#include "services/StillFrameCapture.hpp"
#include "services/ToolRegistry.hpp"
#include "services/TranscriptionService.hpp"

namespace palmier::app::detail {

// ---------------------------------------------------------------------------
// Offline default backends
// ---------------------------------------------------------------------------
//
// These stand in for the closed-source hosted services when no real backend is
// injected. Each reports the feature as unconfigured WITHOUT performing any
// network activity, so the open-source editor + MCP server run offline
// (Requirements 13.3/13.4) while generative/auth features cleanly report
// "unavailable" until real backends are wired by the caller.

// The generative backend is no longer a local stub. Task 10.5 replaced this
// file's private `OfflineGenerativeBackend` with
// `services::selectGenerativeBackend()`, which compiles all three backends in —
// `offline`, `hosted`, `byok` — selects one by configuration string with no
// recompilation, and falls back to the offline STUB (a named, tested component
// that holds no transport at all) whenever the requested id is unknown or its
// credentials are absent (Requirements 12.1, 12.2, 12.8). See the construction
// site below.

// The agent interpreter is no longer defaulted to an inert
// "not configured" stub. Task 10.1 replaced `makeUnconfiguredInterpreter()` with
// `services::selectAgentInterpreter()`, which always installs a working
// implementation: the configured id when it is selectable, and otherwise the
// built-in `OfflineIntentInterpreter` plus a startup error naming the rejected id
// (design.md D9; Requirements 11.1, 11.3, 11.8). See the construction site below.

/// Reports `feature` as unconfigured without performing any network activity.
[[nodiscard]] Error offlineError(const char* feature);

class OfflineAuthBackend final : public services::AuthBackend {
public:
    [[nodiscard]] Result<services::BackendSession> authenticate(
        const services::LoginCredentials&) override {
        return err<services::BackendSession>(offlineError("authentication"));
    }
};

class OfflineByokValidator final : public services::ByokProviderValidator {
public:
    [[nodiscard]] Result<void> validate(const services::ByokCredential&) override {
        return err<void>(offlineError("BYOK validation"));
    }
};

// ---------------------------------------------------------------------------
// Injected tool handlers
// ---------------------------------------------------------------------------
//
// Each is documented at its definition in ApplicationCompositionHooks.cpp, where the
// behaviour being described is in view.

/// Adapts `generation.generate` onto the composed GenerativeMediaCoordinator.
[[nodiscard]] services::Tool::Handler makeGenerateHook(
    services::GenerativeMediaCoordinator& coordinator, const services::GenerativeBackend* backend);

/// Adapts `generation.list_models` onto the composed model catalog.
[[nodiscard]] services::Tool::Handler makeListModelsHook(
    const services::GenerationModelCatalog& catalog);

/// Adapts `text.transcribe_to_captions` onto the composed TranscriptionService.
[[nodiscard]] services::Tool::Handler makeTranscribeToCaptionsHook(
    services::TranscriptionService& transcription, services::ProjectSession& session);

/// Adapts `timeline.capture_frame` onto the composed Compositor.
[[nodiscard]] services::Tool::Handler makeCaptureFrameHook(gpu::Compositor& compositor,
                                                           services::ProjectSession& session,
                                                           services::ImageEncoder encoder);

}  // namespace palmier::app::detail

#endif  // PALMIER_APP_APPLICATIONCOMPOSITIONHOOKS_HPP
