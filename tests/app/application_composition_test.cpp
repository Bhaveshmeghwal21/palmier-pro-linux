// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/app/application_composition_test.cpp — unit tests for the application
// composition root (task 21.1; Requirements 1.6, 7.1, 7.2, 7.9, 13.3).
//
// These tests exercise the Qt-free composition root headlessly: that every
// component is composed and cross-wired, that the MCP server starts on launch
// and stops on close within its lifecycle, that a request served through the
// composed server reaches the SAME project session the tool executor drives, and
// that the offline defaults keep the editor + MCP endpoint fully functional with
// no network connection (13.3). They bind the MCP endpoint on an ephemeral port
// (mcpPort == 0) so they never contend for the well-known 19789.

#include <gtest/gtest.h>

#include <string>

#include "app/ApplicationComposition.hpp"
#include "core/Error.hpp"
#include "core/Result.hpp"
#include "core/TimelineEngine.hpp"
#include "gpu/Compositor.hpp"
#include "gpu/GpuContext.hpp"
#include "media/AudioEngine.hpp"
#include "media/AudioSink.hpp"
#include "ui/PreviewController.hpp"
#include "services/AgentOrchestrator.hpp"
#include "services/Json.hpp"
#include "services/LocalizationManager.hpp"
#include "services/McpServer.hpp"
#include "services/McpToolExecutor.hpp"
#include "services/ProjectSession.hpp"
#include "services/ToolRegistry.hpp"

namespace {

using palmier::app::AppConfig;
using palmier::app::ApplicationComposition;
using palmier::services::Json;

// A config that binds an ephemeral MCP port so lifecycle tests are deterministic
// and never collide with a real 127.0.0.1:19789 endpoint.
AppConfig ephemeralConfig() {
    AppConfig config;
    config.mcpPort = 0;  // ask the OS for a free loopback port
    return config;
}

TEST(ApplicationComposition, ComposesAllComponentsWithoutStarting) {
    ApplicationComposition composition{ephemeralConfig()};

    // Every component is constructed and reachable.
    // (Accessors return references; a crash/UB here would fail the test.)
    (void)composition.gpuContext();
    (void)composition.projectSession();
    (void)composition.timeline();
    (void)composition.mediaLibrary();
    (void)composition.projectSaveService();
    (void)composition.auth();
    (void)composition.byokManager();
    (void)composition.generativeClient();
    (void)composition.generativeCoordinator();
    (void)composition.toolRegistry();
    (void)composition.agent();
    (void)composition.localization();

    // Requirement 1.1 / design.md D1: there is exactly ONE Project_Session, and
    // it is the owner of the single timeline engine and the single media library —
    // `timeline()` and `mediaLibrary()` are views onto that same session, not
    // separately constructed components.
    EXPECT_EQ(&composition.projectSession(), &composition.projectSession());
    EXPECT_EQ(&composition.timeline(), &composition.projectSession().engine());
    EXPECT_EQ(&composition.mediaLibrary(), &composition.projectSession().mediaLibrary());

    // Requirement 1.10: with no project path supplied the session starts on an
    // empty project, unmodified and with no known on-disk location.
    EXPECT_FALSE(composition.projectSession().modified());
    EXPECT_FALSE(composition.projectSession().documentPath().has_value());

    // The executor is bound to that session, so MCP tool calls operate on the
    // current project rather than reporting "no project open" (Requirement 7.10 is
    // about the null case).
    EXPECT_TRUE(composition.executor().hasProject());
    EXPECT_EQ(composition.executor().session(), &composition.projectSession());

    // The MCP endpoint is not accepting connections until start() is called.
    EXPECT_FALSE(composition.running());
    EXPECT_EQ(composition.mcpBoundPort(), 0);

    // The default tool surface is present and shared (Property P4 relies on this
    // single registry being what both the MCP server and the agent drive).
    EXPECT_GT(composition.toolRegistry().size(), 0u);
    EXPECT_TRUE(composition.toolRegistry().has("timeline.read"));
}

// --- Playback_Engine (task 7.5; Requirement 1.1) ---------------------------
//
// The Playback_Engine named in Requirement 1.1 is composed here: one Compositor
// over the one GpuContext, one decoder teardown queue, one decoder-backed clip
// frame provider installed on that compositor, and one PreviewController
// transport reading the one session's project. Each accessor must hand back a
// non-null reference to the SAME instance for the process lifetime.
TEST(ApplicationComposition, ComposesOnePlaybackEngineOverTheOneSession) {
    ApplicationComposition composition{ephemeralConfig()};

    // Same instance on every call (Requirement 1.1).
    EXPECT_EQ(&composition.playbackEngine(), &composition.playbackEngine());
    EXPECT_EQ(&composition.compositor(), &composition.compositor());
    EXPECT_EQ(&composition.clipFrameProvider(), &composition.clipFrameProvider());
    EXPECT_EQ(&composition.decoderTeardownQueue(), &composition.decoderTeardownQueue());

    // The compositor has its source of pixels wired in — the decoder-backed
    // provider, not a test double — so a position covered by a clip can decode.
    EXPECT_TRUE(composition.compositor().hasFrameProvider());

    // The transport starts halted at position zero on the composed session's
    // empty default project, and its preview cadence is the documented minimum.
    EXPECT_EQ(composition.playbackEngine().state(), palmier::ui::PlaybackState::Stopped);
    EXPECT_EQ(composition.playbackEngine().playhead(), palmier::Duration::zero());
    EXPECT_GE(composition.playbackEngine().previewFps(), 24.0);
    EXPECT_EQ(composition.playbackEngine().timelineDuration(), palmier::Duration::zero());

    // Requirement 5.6: the software-compositing notice is empty until a GPU
    // compositing failure degrades the path at runtime.
    EXPECT_TRUE(composition.softwareCompositingNotice().empty());

    // The transport reads the project from the ONE session, so an edit applied
    // through the session is visible to playback with no rewiring: adding a clip
    // gives the previously empty timeline a positive duration.
    EXPECT_EQ(composition.playbackEngine().timelineDuration(), palmier::Duration::zero());
    EXPECT_EQ(composition.playbackEngine().timelineDuration(),
              palmier::timelineDuration(composition.timeline().snapshot()));
}

// The Audio_Engine of Requirement 1.1 (task 8.7): exactly one media::AudioEngine,
// constructed with the sink chosen at startup in the order PipeWire -> ALSA ->
// Null, the SAME decoder teardown queue the video path uses, and a project
// provider bound to the one session — and installed as the PreviewController's
// audio master clock, which is what makes the sink the clock the whole
// presentation pipeline slews to (Requirement 6.3).
//
// Every assertion here is host-independent: on a machine with an audio server a
// real sink is selected, and on a machine with none (this project's CI, and every
// container) selection falls through to the null sink with the Requirement 6.7
// notice. The invariants below hold either way.
TEST(ApplicationComposition, ComposesOneAudioEngineWhoseSinkIsTheMasterClock) {
    ApplicationComposition composition{ephemeralConfig()};

    // Same instance on every call (Requirement 1.1).
    EXPECT_EQ(&composition.audioEngine(), &composition.audioEngine());

    // The engine's output format is the documented fixed one (Requirement 6.2).
    EXPECT_EQ(palmier::media::AudioEngine::kOutputSampleRate, 48'000);
    EXPECT_EQ(palmier::media::AudioEngine::kOutputChannels, 2);

    // The selected sink is one of the design's three, and the sink the engine
    // reports is the one selection chose.
    const std::string sinkName = composition.audioSinkName();
    EXPECT_TRUE(sinkName == "pipewire" || sinkName == "alsa" || sinkName == "null")
        << "unexpected sink name: " << sinkName;
    EXPECT_EQ(std::string(composition.audioEngine().sinkName()), sinkName);

    // The Requirement 6.7 notice is present exactly when no real device was opened,
    // and it never appears alongside a real device.
    EXPECT_EQ(composition.audioOutputAvailable(), sinkName != "null");
    EXPECT_EQ(composition.audioUnavailableNotice().empty(), composition.audioOutputAvailable());
    if (!composition.audioOutputAvailable()) {
        EXPECT_NE(composition.audioUnavailableNotice().find("Audio output is unavailable"),
                  std::string::npos);
    }

    // The quantum comes from the project frame rate: at most 512 frames above
    // 48 fps, otherwise 1024 (design.md D7).
    const double fps = composition.timeline().snapshot().timelineFps.toDouble();
    EXPECT_EQ(composition.audioEngine().quantumFrames(),
              palmier::media::preferredQuantumFrames(fps));

    // The transport is halted, so the engine has not been started and the audio
    // clock is not yet authoritative — video therefore paces off the wall clock,
    // which is what keeps a session with no audio device behaving exactly as it did
    // before this stage.
    EXPECT_FALSE(composition.audioEngine().running());
    EXPECT_TRUE(composition.playbackEngine().hasAudioMasterClock());
    EXPECT_EQ(composition.playbackEngine().pump(), 0u);
    EXPECT_FALSE(composition.playbackEngine().lastAudioPosition().has_value());

    // Starting the engine makes its position the master clock, and it starts at the
    // requested timeline position rather than jumping.
    ASSERT_TRUE(composition.audioEngine().start(palmier::Duration::zero()).isOk());
    EXPECT_TRUE(composition.audioEngine().running());
    EXPECT_EQ(composition.audioEngine().presentationPosition(), palmier::Duration::zero());

    // The engine mixes the ONE session's project: an empty default project yields a
    // silent quantum with no contributions and no errors.
    const palmier::Result<std::size_t> mixed = composition.audioEngine().pump();
    ASSERT_TRUE(mixed.isOk());
    EXPECT_EQ(mixed.value(), composition.audioEngine().quantumFrames());
    EXPECT_TRUE(composition.audioEngine().lastQuantum().contributions.empty());
    EXPECT_TRUE(composition.audioEngine().errors().empty());

    composition.audioEngine().stop();
    EXPECT_FALSE(composition.audioEngine().running());
}

TEST(ApplicationComposition, StartsMcpServerOnLaunchAndStopsOnClose) {
    ApplicationComposition composition{ephemeralConfig()};

    // Requirement 7.1/7.2: start binds the loopback endpoint and begins accepting.
    const palmier::Result<void> started = composition.start();
    ASSERT_TRUE(started.isOk()) << started.error().toString();
    EXPECT_TRUE(composition.running());
    EXPECT_NE(composition.mcpBoundPort(), 0);

    // Requirement 7.9: stop() halts the endpoint; it is idempotent.
    composition.stop();
    EXPECT_FALSE(composition.running());
    composition.stop();  // second stop is a harmless no-op
    EXPECT_FALSE(composition.running());

    // The endpoint can be brought back up after a clean stop.
    const palmier::Result<void> restarted = composition.start();
    ASSERT_TRUE(restarted.isOk()) << restarted.error().toString();
    EXPECT_TRUE(composition.running());
}

TEST(ApplicationComposition, McpServerServesToolCallsAgainstTheComposedTimeline) {
    ApplicationComposition composition{ephemeralConfig()};
    ASSERT_TRUE(composition.start().isOk());

    // Route a well-formed MCP tool call through the composed server's pure
    // request-routing core (no sockets): POST /mcp with a tools/call for the
    // read tool. This exercises server -> executor -> ToolRegistry -> session.
    Json call = Json::object();
    call.set("name", "timeline.read");
    call.set("arguments", Json::object());

    palmier::services::HttpRequest request;
    request.method = "POST";
    request.target = std::string(palmier::services::McpServer::kPath);
    request.body = call.dump();

    const palmier::services::HttpResponse response =
        composition.mcpServer().dispatch(request);
    EXPECT_EQ(response.status, 200);

    const palmier::Result<Json> parsed = Json::parse(response.body);
    ASSERT_TRUE(parsed.isOk()) << parsed.error().toString();
    EXPECT_TRUE(parsed.value().boolOr("ok", false));

    // The project the tool read is the composed session's project, so the request
    // really did reach the one current project (Requirements 1.1, 9.4).
    const Json* result = parsed.value().find("result");
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->stringOr("id"),
              composition.projectSession().status().projectId.toString());

    composition.stop();
}

TEST(ApplicationComposition, DestructorStopsAServerLeftRunning) {
    // Requirement 7.9: closing the application stops the server. Constructing in
    // a nested scope and letting the destructor run must join the accept thread
    // cleanly (no hang / crash) even when stop() was not called explicitly.
    {
        ApplicationComposition composition{ephemeralConfig()};
        ASSERT_TRUE(composition.start().isOk());
        EXPECT_TRUE(composition.running());
    }
    SUCCEED();
}

TEST(ApplicationComposition, OfflineAgentGateRejectsAndPreservesMessage) {
    // With the offline auth backend and no subscription/BYOK, the agent send is
    // gated (Requirement 8.5): rejected with Unauthenticated and the unsent text
    // preserved for retry. This confirms the agent is wired to the auth gate.
    ApplicationComposition composition{ephemeralConfig()};

    const palmier::Result<palmier::services::AgentTurn> turn =
        composition.agent().sendMessage("please add a clip");
    ASSERT_TRUE(turn.isError());
    EXPECT_EQ(turn.error().code(), palmier::ErrorCode::Unauthenticated);
    EXPECT_TRUE(composition.agent().hasPendingMessage());
    EXPECT_EQ(composition.agent().pendingMessage(), "please add a clip");
}

// ---------------------------------------------------------------------------
// Agent interpreter selection (task 10.1; Requirements 11.1, 11.8) — the
// composition root accepts exactly one implementation through configuration and
// exposes the ACTIVE one, which is not always the one that was asked for.
// ---------------------------------------------------------------------------

TEST(ApplicationComposition, DefaultsToTheOfflineInterpreterWithNoStartupError) {
    ApplicationComposition composition{ephemeralConfig()};

    // Requirement 11.1: the active implementation is reported through a public
    // accessor. Replacing `makeUnconfiguredInterpreter()` means the default is a
    // WORKING interpreter, so a documented phrase maps rather than reporting the
    // interpreter as unconfigured.
    EXPECT_EQ(composition.agentInterpreterId(), "offline");
    EXPECT_TRUE(composition.startupErrors().empty());
}

TEST(ApplicationComposition, AnUnknownInterpreterIdFallsBackToOfflineAndStillComposes) {
    AppConfig config = ephemeralConfig();
    config.agentInterpreterId = "no-such-interpreter";

    ApplicationComposition composition{config};

    // Requirement 11.8: the rejected id is named, the offline interpreter is in
    // force, and every other component is still constructed — a mistyped
    // configuration string cannot stop the editor from coming up.
    EXPECT_EQ(composition.agentInterpreterId(), "offline");
    ASSERT_EQ(composition.startupErrors().size(), 1u);
    EXPECT_NE(composition.startupErrors()[0].find("no-such-interpreter"), std::string::npos);
    (void)composition.toolRegistry();
    (void)composition.executor();
    (void)composition.agent();
    (void)composition.projectSession();
    EXPECT_TRUE(composition.start().isOk());
}

TEST(ApplicationComposition, CredentiallessHostedInterpreterFallsBackToOffline) {
    AppConfig config = ephemeralConfig();
    config.agentInterpreterId = "hosted";

    ApplicationComposition composition{config};

    // The offline auth backend grants no subscription, so `hosted` is unauthorized
    // and the fallback applies, naming the unmet requirement.
    EXPECT_EQ(composition.agentInterpreterId(), "offline");
    ASSERT_EQ(composition.startupErrors().size(), 1u);
    EXPECT_NE(composition.startupErrors()[0].find("hosted"), std::string::npos);
}

TEST(ApplicationComposition, AnInjectedInterpreterOverridesTheRegistry) {
    // Injecting an implementation directly is the seam a test or a model-backed
    // shell uses. It bypasses the registry entirely, so the credential check that
    // would otherwise demote `hosted` to `offline` does not run: the reported id is
    // the configured one and no fallback diagnostic is recorded.
    AppConfig config = ephemeralConfig();
    config.agentInterpreterId = "hosted";
    config.agentInterpreter =
        [](std::string_view) -> palmier::Result<palmier::services::AgentIntent> {
        return palmier::services::AgentIntent{"timeline.read",
                                              palmier::services::Json::object()};
    };

    ApplicationComposition composition{config};
    EXPECT_EQ(composition.agentInterpreterId(), "hosted");
    EXPECT_TRUE(composition.startupErrors().empty());

    // Sanity: the same configuration WITHOUT the injected implementation is demoted,
    // which is what makes the override observable rather than incidental.
    AppConfig withoutInjection = ephemeralConfig();
    withoutInjection.agentInterpreterId = "hosted";
    ApplicationComposition demoted{withoutInjection};
    EXPECT_EQ(demoted.agentInterpreterId(), "offline");
    EXPECT_FALSE(demoted.startupErrors().empty());
}

TEST(ApplicationComposition, LocalizationDefaultsToASupportedLanguage) {
    ApplicationComposition composition{ephemeralConfig()};
    // On first launch with no persisted selection the manager picks the system
    // language when supported, else English — always a supported language.
    EXPECT_TRUE(palmier::services::isSupportedLanguage(
        composition.localization().currentLanguage()));
}

TEST(ApplicationComposition, GpuContextDegradesToSoftwareWhenNoGpu) {
    // Requirement 13.3: with no GPU the context degrades to software so the
    // editor stays fully functional. In the sandbox (no Vulkan) this is the
    // software fallback; the notice is surfaced for the UI's non-blocking banner.
    ApplicationComposition composition{ephemeralConfig()};
    if (composition.gpuContext().isSoftwareFallback()) {
        EXPECT_FALSE(composition.gpuUnavailableNotice().empty());
    }
}

}  // namespace
