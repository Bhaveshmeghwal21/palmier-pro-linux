// SPDX-License-Identifier: GPL-3.0-or-later
//
// app/ApplicationComposition.hpp — the application composition root (task 21.1;
// Requirements 1.6, 7.1, 7.2, 7.9, 13.3).
//
// This is where every layer of Palmier Pro is finally wired together into one
// running application. Each component was built and unit-tested in isolation
// behind a narrow seam; the composition root is the single place that
// constructs the concrete instances and connects them:
//
//   * GpuContext            — the GPU abstraction layer, created with the
//                             software fallback guarantee (never fails for "no
//                             GPU"; Requirement 13.3 keeps the editor fully
//                             functional on the CPU path).
//   * ProjectSession        — the single owner of the current project
//                             (Requirement 1.1): one TimelineEngine, whose
//                             project the UI, the MCP server and the in-app agent
//                             all mutate through the SAME atomic/undoable command
//                             path, plus the project's MediaManager media
//                             library, its document path and its dirty flag. The
//                             `timeline()` and `mediaLibrary()` accessors are
//                             views onto that one session, so there is exactly one
//                             engine and one library in the process (design.md
//                             decision D1).
//   * ProjectSaveService    — Project I/O: crash-safe `.palmier` persistence.
//   * AuthenticationService — login / subscription entitlement + BYOK, backing
//                             the generative and agent auth gates.
//   * GenerativeClient +
//     GenerativeMediaCoordinator — the in-timeline generative pipeline.
//   * ToolRegistry +
//     McpToolExecutor       — the shared editor tool surface and its execution
//                             policy (Property P4: UI/MCP/agent equivalence).
//   * McpServer             — the loopback MCP HTTP endpoint at
//                             127.0.0.1:19789/mcp, STARTED on launch and STOPPED
//                             on close (Requirements 7.1, 7.2, 7.9).
//   * AgentOrchestrator     — the in-app agent chat, driving the same executor
//                             with @-mention resolution wired in.
//   * LocalizationManager   — interface-language policy (live switch, fallback,
//                             persistence).
//
// Design intent — Qt-free wiring
// ------------------------------
// The composition of the domain/service layer is deliberately Qt-free and
// therefore unit-testable on any host (including CI / the sandbox with no Qt,
// FFmpeg, Vulkan, shaderc, lcms2 or libsecret installed): all vendor-specific
// paths live behind the project's guard macros and degrade to software / stub
// implementations. main.cpp keeps only the thin Qt entry glue (behind
// PALMIER_HAVE_QT): it runs the platform-compatibility gate, then constructs an
// ApplicationComposition, starts it (which starts the MCP server), shows the Qt
// UI shell bound to the composed components, and stops it on close.
//
// Offline defaults
// ----------------
// The three network-facing backends (auth, generative, BYOK validation) and the
// platform secret store are injected through AppConfig so tests can supply
// deterministic fakes. When a backend is not supplied the composition installs a
// safe OFFLINE default that reports the feature as unconfigured rather than
// touching the network — so the editor and the MCP server come up and run with
// no network connection (Requirements 13.3), and generative/auth features simply
// report "unavailable" until real backends are wired.

#ifndef PALMIER_APP_APPLICATIONCOMPOSITION_HPP
#define PALMIER_APP_APPLICATIONCOMPOSITION_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/Result.hpp"
#include "gpu/GpuTypes.hpp"
#include "services/AgentOrchestrator.hpp"       // IntentInterpreter, IAgentAuthGate
#include "services/AuthenticationService.hpp"    // AuthBackend
#include "services/ByokCredentials.hpp"          // ByokProviderValidator
#include "services/GenerativeClient.hpp"         // IGenerativeBackend
#include "services/LocalizationManager.hpp"      // Catalog, SystemLanguageProvider
#include "services/McpProtocolHandler.hpp"        // MainThreadInvoker (design.md D5)
#include "services/McpServer.hpp"                // kDefaultHost/kDefaultPort
#include "services/RemoteAccessGate.hpp"         // RemoteAccessConfig (task 6.1)
#include "services/SecretStore.hpp"              // SecretStore

// Forward declarations for the concrete components the composition root owns by
// unique_ptr (their headers are pulled in only by the .cpp, keeping this header
// lean; the out-of-line destructor makes unique_ptr<incomplete-type> members
// well-formed).
namespace palmier {
class TimelineEngine;
class MediaManager;
struct Project;
}  // namespace palmier

namespace palmier::gpu {
class GpuContext;
class Compositor;
}  // namespace palmier::gpu

namespace palmier::media {
class DecoderTeardownQueue;
class DecoderClipFrameProvider;
class AudioEngine;
class IAudioSink;
}  // namespace palmier::media

namespace palmier::ui {
class PreviewController;
}  // namespace palmier::ui

namespace palmier::services {
class ProjectSession;
class ProjectSaveService;
class ByokCredentialManager;
class GenerativeClient;
class GenerativeMediaCoordinator;
class ToolRegistry;
class McpToolExecutor;
class IGenerationGate;
class IGenerationRunner;
class ITimelinePlacement;
}  // namespace palmier::services

namespace palmier::app {

// ---------------------------------------------------------------------------
// AppConfig — how to compose the application.
// ---------------------------------------------------------------------------

/// Configuration for building an ApplicationComposition. Every field has a
/// production-sensible default; tests override the pieces they need
/// (particularly the MCP port and the network-facing backends).
struct AppConfig {
    /// How the GPU device is chosen (Auto by default; always degrades to the
    /// software fallback when no compatible device is present — Requirement 13.3).
    gpu::GpuSelectionPolicy gpuPolicy = gpu::GpuSelectionPolicy::automatic();

    /// The MCP endpoint. Defaults to the canonical loopback host/port
    /// (127.0.0.1:19789) matching the original (Requirement 7.1). Tests set
    /// `mcpPort = 0` to bind an ephemeral port and exercise the lifecycle without
    /// contending for the well-known port.
    std::string   mcpHost = std::string(services::McpServer::kDefaultHost);
    std::uint16_t mcpPort = services::McpServer::kDefaultPort;

    /// Network-facing backends. When null, a safe offline default is installed
    /// (feature reports "unconfigured"; no network is touched — Requirement 13.3).
    services::AuthBackend*          authBackend = nullptr;
    services::IGenerativeBackend*   generativeBackend = nullptr;
    services::ByokProviderValidator* byokValidator = nullptr;

    /// Platform secret store for BYOK credentials. When null the composition uses
    /// an in-memory store (non-persistent); production wires the libsecret-backed
    /// store from makeSystemSecretStore().
    services::SecretStore* secretStore = nullptr;

    /// Localization catalog + seams (all optional). Empty catalog + null store =>
    /// English/default policy with no persistence.
    services::LocalizationManager::Catalog translations;
    services::LanguagePreferenceStore*     languageStore = nullptr;
    services::SystemLanguageProvider       systemLanguage;

    /// The in-app agent's natural-language -> tool-call interpreter. When empty a
    /// default is installed that reports the interpreter is unconfigured (the
    /// agent chat is inert until a model backend is wired) — the editor, MCP
    /// server, and manual tool calls remain fully functional regardless.
    services::IntentInterpreter agentInterpreter;

    /// BYOK provider ids that authorize the generative / agent features in
    /// addition to an active subscription (used by the auth gates).
    std::vector<std::string> byokProviders;

    /// Remote MCP access (task 6.1; Requirements 10.1-10.3, 16.3). Default
    /// constructed = disabled, i.e. loopback only. Resolved from configuration by
    /// `app::AppSettings`; validated and enforced by `services::RemoteAccessGate`
    /// (task 6.2). Nothing here binds a non-loopback address on its own.
    services::RemoteAccessConfig remote;

    /// How work is marshalled onto the thread that owns the project session
    /// (design.md D5; Requirement 9.16) — the seam declared alongside
    /// `McpProtocolHandler`, which is what consumes it. Left empty it means "use
    /// `services::inlineMainThreadInvoker()`", i.e. run the work on the calling
    /// thread, which is what headless drivers and tests want; the Qt shell
    /// supplies an invoker that posts to the main thread and waits with the 60 s
    /// budget. The substitution happens where the protocol handler is constructed.
    services::MainThreadInvoker mainThreadInvoker;
};

// ---------------------------------------------------------------------------
// ApplicationComposition — owns and wires every component.
// ---------------------------------------------------------------------------

/// The composition root. Construction wires the whole application together
/// (no network activity); start()/stop() manage the MCP server lifecycle
/// (Requirements 7.2/7.9). Non-copyable, non-movable (it owns OS resources and
/// cross-referencing components).
class ApplicationComposition {
public:
    /// Build and wire the application per `config`. Never touches the network;
    /// the GPU context degrades to software when no device is present. Does NOT
    /// start the MCP server — call start() for that.
    explicit ApplicationComposition(AppConfig config = {});

    ~ApplicationComposition();

    ApplicationComposition(const ApplicationComposition&) = delete;
    ApplicationComposition& operator=(const ApplicationComposition&) = delete;
    ApplicationComposition(ApplicationComposition&&) = delete;
    ApplicationComposition& operator=(ApplicationComposition&&) = delete;

    // --- Lifecycle ---------------------------------------------------------

    /// Start the MCP server, binding the loopback endpoint and beginning to
    /// accept connections (Requirements 7.1, 7.2). Returns the bind error
    /// unchanged when the port is already in use — the server refuses to start
    /// and the project is left unchanged (Requirement 7.3). Idempotent-safe: a
    /// second call while running returns a FailedPrecondition error from the
    /// server without disturbing the running endpoint.
    [[nodiscard]] Result<void> start();

    /// Stop the MCP server and join its accept thread (Requirement 7.9).
    /// Idempotent — a no-op when not running. Called automatically on destruction.
    void stop();

    /// True while the MCP server is bound and accepting connections.
    [[nodiscard]] bool running() const noexcept;

    /// The actual bound MCP port (useful when started with `mcpPort == 0`), or 0
    /// when not running.
    [[nodiscard]] std::uint16_t mcpBoundPort() const noexcept;

    // --- Component accessors (for the Qt UI shell and headless drivers) ----

    [[nodiscard]] gpu::GpuContext&               gpuContext() noexcept { return *gpu_; }

    /// The one Project_Session of the application (Requirement 1.1): the current
    /// project, its engine, its media library, its document path and its dirty
    /// flag. The reference is stable for the application's lifetime, including
    /// across a project create/open (design.md decision D1).
    [[nodiscard]] services::ProjectSession&      projectSession() noexcept;

    /// The session's timeline engine — the same object for the whole application
    /// lifetime. Defined out of line because the session's definition lives in the
    /// .cpp (this header stays free of the core/service headers it would need).
    [[nodiscard]] TimelineEngine&                timeline() noexcept;

    /// The current project's media library, owned by the same session.
    [[nodiscard]] MediaManager&                  mediaLibrary() noexcept;

    // --- Playback_Engine (task 7.5; Requirement 1.1) -----------------------
    //
    // The Playback_Engine of Requirement 1.1 is three collaborating objects, each
    // constructed exactly once here and each reachable through a non-null
    // reference for the process lifetime:
    //
    //   * the single `gpu::Compositor` over the single `gpu::GpuContext`;
    //   * the single `media::DecoderClipFrameProvider` installed as that
    //     compositor's clip frame provider, itself owning the decode worker pool
    //     and retiring decoders through the one `media::DecoderTeardownQueue`;
    //   * the single `ui::PreviewController` — the transport (play, pause, stop,
    //     seek-with-clamp, end-of-timeline halt, drop accounting, decode-failure
    //     pause and the software-compositing fallback) that the Qt preview surface
    //     drives with a timer in task 11.3.

    /// The one Compositor of the application: what playback composites through,
    /// and the object the clip frame provider is installed on.
    [[nodiscard]] gpu::Compositor& compositor() noexcept { return *compositor_; }

    /// The one Playback_Engine transport of the application (Requirement 1.1).
    [[nodiscard]] ui::PreviewController& playbackEngine() noexcept { return *playbackEngine_; }

    /// The decoder-backed clip frame provider feeding the compositor.
    [[nodiscard]] media::DecoderClipFrameProvider& clipFrameProvider() noexcept {
        return *clipFrameProvider_;
    }

    /// The one decoder teardown queue: retired decoders are closed on its thread
    /// so no close ever blocks playback, the audio callback or the Qt main thread
    /// (Requirement 14.8). Shared with the audio decoder from stage 8 onward.
    [[nodiscard]] media::DecoderTeardownQueue& decoderTeardownQueue() noexcept {
        return *decoderTeardown_;
    }

    // --- Audio_Engine (task 8.7; Requirements 1.1, 6.3, 6.7) ---------------
    //
    // Exactly one `media::AudioEngine`, constructed with:
    //
    //   * the sink chosen at startup by `media::selectAudioSink()` in the design's
    //     order PipeWire -> ALSA -> Null, so a host with no audio device gets the
    //     null sink, audio is suppressed, video keeps running and
    //     `audioUnavailableNotice()` is non-empty (Requirement 6.7);
    //   * the SAME `media::DecoderTeardownQueue` the video path uses, so an audio
    //     decoder close never blocks the mixing thread (Requirement 14.8, upstream
    //     PR 405 — the reason that queue had to land before this stage);
    //   * a `ProjectProvider` bound to the one session's project, re-read per
    //     mixed quantum, which is what bounds the mute/gain latency of
    //     Requirement 6.4 without moving the playhead.
    //
    // The engine's `presentationPosition()` is then installed as the
    // `PreviewController`'s OPTIONAL audio master clock, making the sink the clock
    // the whole presentation pipeline slews to (Requirement 6.3).

    /// The one Audio_Engine of the application (Requirement 1.1).
    [[nodiscard]] media::AudioEngine& audioEngine() noexcept { return *audioEngine_; }

    /// The sink selected at startup: "pipewire", "alsa" or "null".
    [[nodiscard]] const std::string& audioSinkName() const noexcept { return audioSinkName_; }

    /// True when startup selection opened a real output device. False means the
    /// null sink is in use — audio is suppressed and video is unaffected.
    ///
    /// This accessor, not `AudioEngine::outputAvailable()`, is the authority on
    /// device availability for the shell. The engine reports whether the sink it
    /// was GIVEN opened, which is necessarily true once selection has handed it a
    /// working `NullAudioSink`; only the selection knows that the sink it chose was
    /// the null fallback rather than a device. `AudioEngine::outputAvailable()`
    /// remains the right answer to its own question — "did my sink open?" — which
    /// is what makes its own runtime degradation path (Requirement 6.7) work.
    [[nodiscard]] bool audioOutputAvailable() const noexcept { return audioOutputAvailable_; }

    /// The status-bar notice for Requirement 6.7 ("audio output is unavailable"),
    /// naming why each candidate sink was not used, or empty when a real device
    /// was opened. Populated at construction — no transport start is needed for
    /// the shell to show it.
    [[nodiscard]] const std::string& audioUnavailableNotice() const noexcept {
        return audioUnavailableNotice_;
    }

    /// The status-bar notice stating that software compositing is in use after a
    /// runtime GPU compositing failure, or empty while the GPU path is in use
    /// (Requirement 5.6). Distinct from `gpuUnavailableNotice()`, which reports
    /// that no compatible device was found at startup (Requirement 1.6).
    [[nodiscard]] const std::string& softwareCompositingNotice() const noexcept;
    [[nodiscard]] services::ProjectSaveService&  projectSaveService() noexcept { return *saveService_; }
    [[nodiscard]] services::AuthenticationService& auth() noexcept { return *auth_; }
    [[nodiscard]] services::ByokCredentialManager& byokManager() noexcept { return *byokManager_; }
    [[nodiscard]] services::GenerativeClient&    generativeClient() noexcept { return *genClient_; }
    [[nodiscard]] services::GenerativeMediaCoordinator& generativeCoordinator() noexcept {
        return *genCoordinator_;
    }
    [[nodiscard]] services::ToolRegistry&        toolRegistry() noexcept { return *toolRegistry_; }
    [[nodiscard]] services::McpToolExecutor&     executor() noexcept { return *executor_; }
    [[nodiscard]] services::McpServer&           mcpServer() noexcept { return *mcpServer_; }
    [[nodiscard]] services::AgentOrchestrator&   agent() noexcept { return *agent_; }
    [[nodiscard]] services::LocalizationManager& localization() noexcept { return *localization_; }

    /// The one Remote_Access_Gate of the application (task 6.3): the bind-time
    /// decision the endpoint was started from and the per-request admission control
    /// the transport consults upstream of the protocol handler. Constructed for
    /// every composition — with remote access disabled (the default) it decides
    /// loopback and admits every request, so it is not an opt-in component
    /// (Requirements 10.1, 10.10).
    [[nodiscard]] services::RemoteAccessGate& remoteAccessGate() noexcept {
        return *remoteAccessGate_;
    }

    /// The startup error naming every unmet remote-access prerequisite, or empty
    /// when remote access is off or fully configured. Populated by `start()`. Never
    /// contains the configured bearer token (Requirements 10.3, 10.12).
    [[nodiscard]] const std::string& remoteAccessStartupError() const noexcept {
        return remoteAccessStartupError_;
    }

    /// The single "traffic is unencrypted" warning of Requirement 10.7, or empty
    /// when TLS is configured or remote access is off. Populated by `start()`, at
    /// most once for the lifetime of the composition.
    [[nodiscard]] const std::string& remoteAccessWarning() const noexcept {
        return remoteAccessWarning_;
    }

    /// The user-facing "GPU acceleration unavailable" notice from the GPU layer
    /// (Requirement 10.4), or empty when hardware acceleration is active. Surfaced
    /// here so the UI shell can show the non-blocking notification.
    [[nodiscard]] std::string gpuUnavailableNotice() const;

private:
    // Endpoint (captured from the config for start()).
    std::string   mcpHost_;
    std::uint16_t mcpPort_;

    // --- GPU + the one project session (engine + media library) ------------
    std::unique_ptr<gpu::GpuContext>            gpu_;
    std::unique_ptr<services::ProjectSession>   session_;

    // --- Playback_Engine: compositor -> provider -> transport (task 7.5) ---
    std::unique_ptr<gpu::Compositor>                    compositor_;
    std::unique_ptr<media::DecoderTeardownQueue>        decoderTeardown_;
    std::unique_ptr<media::DecoderClipFrameProvider>    clipFrameProvider_;
    std::unique_ptr<ui::PreviewController>              playbackEngine_;

    // --- Audio_Engine: selected sink -> engine -> master clock (task 8.7) --
    // `audioProject_` is the storage behind the engine's ProjectProvider: the
    // provider refreshes it from the session's engine snapshot and returns a
    // borrow, which is how the engine sees the current project without
    // `Palmier::media` having to depend on `Palmier::services`.
    std::unique_ptr<Project>                            audioProject_;
    std::unique_ptr<media::AudioEngine>                 audioEngine_;
    std::string                                         audioSinkName_;
    bool                                                audioOutputAvailable_ = false;
    std::string                                         audioUnavailableNotice_;

    // --- Project I/O -------------------------------------------------------
    std::unique_ptr<services::ProjectSaveService> saveService_;

    // --- Auth stack (secret store -> byok -> auth) -------------------------
    std::unique_ptr<services::SecretStore>            ownedSecretStore_;
    services::SecretStore*                            secretStore_ = nullptr;
    std::unique_ptr<services::ByokProviderValidator>  ownedByokValidator_;
    services::ByokProviderValidator*                  byokValidator_ = nullptr;
    std::unique_ptr<services::ByokCredentialManager>  byokManager_;
    std::unique_ptr<services::AuthBackend>            ownedAuthBackend_;
    services::AuthBackend*                            authBackend_ = nullptr;
    std::unique_ptr<services::AuthenticationService>  auth_;

    // --- Generative pipeline ----------------------------------------------
    std::unique_ptr<services::IGenerativeBackend>          ownedGenerativeBackend_;
    services::IGenerativeBackend*                          generativeBackend_ = nullptr;
    std::unique_ptr<services::GenerativeClient>            genClient_;
    std::unique_ptr<services::IGenerationGate>             genGate_;
    std::unique_ptr<services::IGenerationRunner>           genRunner_;
    std::unique_ptr<services::ITimelinePlacement>          placer_;
    std::unique_ptr<services::GenerativeMediaCoordinator>  genCoordinator_;

    // --- Shared tool surface + MCP + agent --------------------------------
    std::unique_ptr<services::ToolRegistry>     toolRegistry_;
    std::unique_ptr<services::McpToolExecutor>  executor_;
    std::unique_ptr<services::RejectionLog>     rejectionLog_;
    std::unique_ptr<services::RemoteAccessGate> remoteAccessGate_;
    std::string                                 remoteAccessStartupError_;
    std::string                                 remoteAccessWarning_;
    std::unique_ptr<services::McpServer>        mcpServer_;
    std::unique_ptr<services::IAgentAuthGate>   agentGate_;
    std::unique_ptr<services::AgentOrchestrator> agent_;

    // --- Localization ------------------------------------------------------
    std::unique_ptr<services::LocalizationManager> localization_;
};

}  // namespace palmier::app

#endif  // PALMIER_APP_APPLICATIONCOMPOSITION_HPP
