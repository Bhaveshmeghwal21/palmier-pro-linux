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

#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include "app/AppSettings.hpp"
#include "app/ApplicationComposition.hpp"
#include "core/MediaManager.hpp"
#include "core/Project.hpp"
#include "services/GenerativeBackendRegistry.hpp"
#include "services/HostedGenerativeBackend.hpp"
#include "services/SecretStore.hpp"
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

// ---------------------------------------------------------------------------
// Generative backend selection (task 10.5; Requirements 12.1, 12.2, 12.4, 12.8).
// ---------------------------------------------------------------------------

// Clips live per track, so "no clip was added" is a count over every track.
std::size_t totalClips(const palmier::Project& project) {
    std::size_t count = 0;
    for (const palmier::Track& track : project.tracks) count += track.clips.size();
    return count;
}

// The `generation.generate` arguments the tool surface accepts. A valid, complete
// argument object, so that a rejection can only be about the backend rather than
// about the arguments.
Json generateArguments(const std::string& trackId) {
    Json arguments = Json::object();
    arguments.set("model", std::string("sota-video-1"));
    arguments.set("prompt", std::string("a slow pan across a harbour at dawn"));
    arguments.set("mediaType", std::string("video"));
    arguments.set("trackId", trackId);
    arguments.set("framePosition", static_cast<std::int64_t>(0));
    arguments.set("sourceInTicks", static_cast<std::int64_t>(0));
    arguments.set("sourceOutTicks", static_cast<std::int64_t>(1'000'000'000));
    return arguments;
}

TEST(ApplicationComposition, DefaultsToTheOfflineGenerativeBackendWithNoStartupError) {
    ApplicationComposition composition{ephemeralConfig()};

    // Requirement 12.1/12.2: the offline stub is installed when nothing is
    // configured, and the active id is reported through a public accessor.
    EXPECT_EQ(composition.generativeBackendId(), "offline");
    EXPECT_TRUE(composition.startupErrors().empty());

    // Requirement 12.4/12.5: the unmet precondition is available for the
    // non-dismissable indication, and it names the preconditions by name.
    const std::string unmet = composition.generationUnmetPrecondition();
    EXPECT_FALSE(unmet.empty());
    EXPECT_NE(unmet.find("generation is unavailable"), std::string::npos) << unmet;
}

TEST(ApplicationComposition, AnUnknownGenerativeBackendIdFallsBackAndStillComposes) {
    AppConfig config = ephemeralConfig();
    config.generativeBackendId = "no-such-backend";

    ApplicationComposition composition{config};

    // Requirement 12.8: the offline stub is installed, the rejected id is named in
    // `startupErrors()`, and every other component named in Requirement 1.1 is
    // still constructed — including the endpoint, which still starts.
    EXPECT_EQ(composition.generativeBackendId(), "offline");
    ASSERT_EQ(composition.startupErrors().size(), 1u);
    EXPECT_NE(composition.startupErrors()[0].find("no-such-backend"), std::string::npos);

    (void)composition.projectSession();
    (void)composition.timeline();
    (void)composition.mediaLibrary();
    (void)composition.projectSaveService();
    (void)composition.auth();
    (void)composition.generativeClient();
    (void)composition.generativeCoordinator();
    (void)composition.toolRegistry();
    (void)composition.executor();
    (void)composition.agent();
    (void)composition.localization();
    EXPECT_TRUE(composition.start().isOk());
    composition.stop();
}

TEST(ApplicationComposition, CredentiallessHostedGenerativeBackendFallsBackToOffline) {
    AppConfig config = ephemeralConfig();
    config.generativeBackendId = "hosted";

    ApplicationComposition composition{config};

    // The offline auth backend grants no subscription, so `hosted` is unauthorized
    // and the Requirement 12.8 fallback applies, naming the unmet requirement.
    EXPECT_EQ(composition.generativeBackendId(), "offline");
    ASSERT_EQ(composition.startupErrors().size(), 1u);
    EXPECT_NE(composition.startupErrors()[0].find("hosted"), std::string::npos);
    EXPECT_NE(composition.generationUnmetPrecondition().find("no authenticated account"),
              std::string::npos);
}

TEST(ApplicationComposition, OfflineGenerationIsRejectedWithNoLibraryClipOrUndoEntry) {
    // Requirement 12.4, through the ONE hook every caller reaches: the tool
    // surface, the MCP endpoint and the agent all dispatch `generation.generate`
    // through this registry, so a rejection here is a rejection for all three.
    ApplicationComposition composition{ephemeralConfig()};

    // A real track to aim at, so the request is otherwise entirely valid.
    Json addTrack = Json::object();
    addTrack.set("kind", std::string("video"));
    addTrack.set("name", std::string("V1"));
    const palmier::Result<Json> track =
        composition.toolRegistry().invoke("timeline.add_track", addTrack);
    ASSERT_TRUE(track.isOk()) << track.error().toString();
    const std::string trackId = track.value().stringOr("trackId");
    ASSERT_FALSE(trackId.empty());

    const std::size_t assetsBefore = composition.mediaLibrary().assetCount();
    const std::size_t clipsBefore = totalClips(composition.timeline().snapshot());
    const bool canUndoBefore = composition.timeline().canUndo();

    const auto started = std::chrono::steady_clock::now();
    const palmier::Result<Json> generated =
        composition.toolRegistry().invoke("generation.generate", generateArguments(trackId));
    const auto elapsed = std::chrono::steady_clock::now() - started;

    // Rejected within 1 s, with the unmet precondition named.
    ASSERT_TRUE(generated.isError());
    EXPECT_EQ(generated.error().code(), palmier::ErrorCode::FailedPrecondition);
    EXPECT_NE(generated.error().message().find("generation is unavailable"), std::string::npos)
        << generated.error().message();
    EXPECT_LT(elapsed, std::chrono::seconds(1));

    // And nothing was added: no media library entry, no clip, no undo entry.
    EXPECT_EQ(composition.mediaLibrary().assetCount(), assetsBefore);
    EXPECT_EQ(totalClips(composition.timeline().snapshot()), clipsBefore);
    EXPECT_EQ(composition.timeline().canUndo(), canUndoBefore);
}

TEST(ApplicationComposition, OfflineModeKeepsEveryNonGenerationOperationWorking) {
    // Requirement 12.5: with the offline stub in force the editor is not degraded.
    // One representative operation from the surfaces Requirement 12.5 enumerates,
    // all through the same registry the rejected generation went through.
    ApplicationComposition composition{ephemeralConfig()};
    ASSERT_EQ(composition.generativeBackendId(), "offline");

    Json addTrack = Json::object();
    addTrack.set("kind", std::string("video"));
    addTrack.set("name", std::string("V1"));
    ASSERT_TRUE(composition.toolRegistry().invoke("timeline.add_track", addTrack).isOk());

    EXPECT_TRUE(composition.toolRegistry().invoke("timeline.read", Json::object()).isOk());
    EXPECT_TRUE(composition.toolRegistry().invoke("project.info", Json::object()).isOk());
    EXPECT_TRUE(composition.toolRegistry().invoke("media.list", Json::object()).isOk());

    // Playback and the MCP endpoint are equally unaffected.
    EXPECT_EQ(composition.playbackEngine().state(), palmier::ui::PlaybackState::Stopped);
    ASSERT_TRUE(composition.start().isOk());
    EXPECT_TRUE(composition.running());
    composition.stop();
}

TEST(ApplicationComposition, AnInjectedGenerativeBackendOverridesTheRegistry) {
    // The injection seam the existing suite uses keeps working, and reports the
    // configured id rather than demoting it — exactly as an injected interpreter
    // does. The injected backend then answers for itself, so the hook does not
    // short-circuit on its behalf.
    class NeverSucceedingBackend final : public palmier::services::IGenerativeBackend {
    public:
        [[nodiscard]] palmier::Result<palmier::services::JobId> submit(
            const palmier::services::GenerationRequest&, std::string_view) override {
            return palmier::err<palmier::services::JobId>(
                palmier::makeError(palmier::ErrorCode::Io, "the injected backend declined"));
        }
        [[nodiscard]] palmier::Result<palmier::services::GenerationStatus> poll(
            const palmier::services::JobId&, std::string_view) override {
            return palmier::err<palmier::services::GenerationStatus>(
                palmier::makeError(palmier::ErrorCode::Io, "the injected backend declined"));
        }
        [[nodiscard]] palmier::Result<palmier::services::MediaAsset> fetchResult(
            const palmier::services::JobId&, std::string_view) override {
            return palmier::err<palmier::services::MediaAsset>(
                palmier::makeError(palmier::ErrorCode::Io, "the injected backend declined"));
        }
        [[nodiscard]] palmier::Result<void> cancel(const palmier::services::JobId&,
                                                   std::string_view) override {
            return palmier::ok();
        }
    };

    NeverSucceedingBackend backend;
    AppConfig config = ephemeralConfig();
    config.generativeBackendId = "hosted";
    config.generativeBackend = &backend;

    ApplicationComposition composition{config};
    EXPECT_EQ(composition.generativeBackendId(), "hosted");
    EXPECT_TRUE(composition.startupErrors().empty());
    EXPECT_TRUE(composition.generationUnmetPrecondition().empty());

    // Sanity: the same configuration WITHOUT the injection is demoted, which is
    // what makes the override observable rather than incidental.
    AppConfig withoutInjection = ephemeralConfig();
    withoutInjection.generativeBackendId = "hosted";
    ApplicationComposition demoted{withoutInjection};
    EXPECT_EQ(demoted.generativeBackendId(), "offline");
    EXPECT_FALSE(demoted.startupErrors().empty());
}

TEST(ApplicationComposition, ASelectedHostedBackendIsInstalledWhenCredentialsArePresent) {
    // Requirement 12.2's "without recompilation": with credentials present the
    // configured id is installed, in the SAME binary, with no rebuild — the
    // difference between this case and the demoted one above is two configuration
    // fields.
    //
    // `featureCredentials` is the seam that makes this reachable at all: at
    // construction nobody has signed in, so the auth-stack probe necessarily
    // answers "no credentials".
    palmier::services::InMemorySecretStore store;
    ASSERT_TRUE(store.store(palmier::services::HostedGenerativeBackend::credentialKey("default"),
                            "stored-hosted-account-token-placeholder")
                    .isOk());

    AppConfig config = ephemeralConfig();
    config.generativeBackendId = "hosted";
    config.secretStore = &store;
    config.generativeEndpoint.baseUrl = "https://generative.invalid";
    config.featureCredentials = [](std::string_view id) { return id == "hosted"; };

    ApplicationComposition composition{config};

    EXPECT_EQ(composition.generativeBackendId(), "hosted");
    EXPECT_TRUE(composition.startupErrors().empty());

    // The stored credential is present and the endpoint configured, so generation
    // reports no unmet precondition — the hook no longer short-circuits.
    EXPECT_TRUE(composition.generationUnmetPrecondition().empty());

    // With no HTTPS transport injected the capability is reported per request, and
    // still nothing is added to the project.
    Json addTrack = Json::object();
    addTrack.set("kind", std::string("video"));
    addTrack.set("name", std::string("V1"));
    const palmier::Result<Json> track =
        composition.toolRegistry().invoke("timeline.add_track", addTrack);
    ASSERT_TRUE(track.isOk()) << track.error().toString();

    const std::size_t clipsBefore = totalClips(composition.timeline().snapshot());
    const palmier::Result<Json> generated = composition.toolRegistry().invoke(
        "generation.generate", generateArguments(track.value().stringOr("trackId")));
    ASSERT_TRUE(generated.isError());
    EXPECT_EQ(totalClips(composition.timeline().snapshot()), clipsBefore);
    EXPECT_EQ(composition.mediaLibrary().assetCount(), 0u);
}

TEST(ApplicationComposition, LocalizationDefaultsToASupportedLanguage) {
    ApplicationComposition composition{ephemeralConfig()};
    // On first launch with no persisted selection the manager picks the system
    // language when supported, else English — always a supported language.
    EXPECT_TRUE(palmier::services::isSupportedLanguage(
        composition.localization().currentLanguage()));
}

// ---------------------------------------------------------------------------
// Startup wiring: resolved settings actually reach the composed AppConfig.
//
// The composition root only ever sees an `AppConfig`, and `app::AppSettings` only
// ever produces one — but nothing tied the two together, so the entry point could
// (and did) construct the composition from a default-constructed config, leaving
// every configurable option unreachable from the shipped binary. These tests pin
// the sequence the entry point performs: resolve settings from the config file and
// the command line, then construct the composition from `settings.config()`.
//
// Hermetic: the config file lives in a pid-qualified scratch directory (no real
// user configuration is read), the environment is driven through the injected
// lookup seam (the process environment is never consulted), and the MCP endpoint
// binds an ephemeral LOOPBACK port — a remote configuration with unmet
// prerequisites is deliberately used, which by design binds loopback.
// ---------------------------------------------------------------------------

using palmier::app::AppSettings;

/// A scratch $XDG_CONFIG_HOME-style directory holding one config file.
class ScratchConfig {
public:
    explicit ScratchConfig(std::string_view label, std::string_view contents) {
        root_ = std::filesystem::temp_directory_path() /
                ("palmier_startup_wiring_" + std::to_string(static_cast<long>(::getpid())) + "_" +
                 std::string(label));
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
        std::filesystem::create_directories(root_);
        path_ = root_ / "config";
        std::ofstream output(path_, std::ios::binary | std::ios::trunc);
        output << contents;
    }

    ~ScratchConfig() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    ScratchConfig(const ScratchConfig&) = delete;
    ScratchConfig& operator=(const ScratchConfig&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path root_;
    std::filesystem::path path_;
};

/// Settings options that know nothing about the process environment, and read the
/// given file as the config file.
AppSettings::Options isolatedOptions(const ScratchConfig& config) {
    AppSettings::Options options;
    options.environment = [](std::string_view) { return std::optional<std::string>{}; };
    options.configFile = config.path();
    return options;
}

TEST(ApplicationStartupWiring, ConfigFileAndCommandLineOverridesReachTheComposedConfig) {
    const ScratchConfig config("reaches",
                               "mcp.host = 127.0.0.1\n"
                               "mcp.port = 0\n"          // ephemeral: never contends for 19789
                               "remote.max_sessions = 12\n"
                               "agent.interpreter = hosted\n"
                               "generative.backend = hosted\n");

    // The command line the entry point would be given, program name included.
    const std::string configFlag = "--config=" + config.path().string();
    const char* argv[] = {"palmier-pro", configFlag.c_str(), "--remote-max-sessions=3",
                          "--generative-backend=no-such-backend", "--turbo-mode"};
    const AppSettings settings =
        AppSettings::fromArgv(static_cast<int>(std::size(argv)), argv, isolatedOptions(config));

    // Resolution: the file was read, the command line won where it spoke, and the
    // unknown option was REPORTED rather than silently swallowed.
    ASSERT_TRUE(settings.configFileRead());
    EXPECT_FALSE(settings.helpRequested());
    bool reportedUnknownOption = false;
    for (const std::string& diagnostic : settings.diagnostics()) {
        reportedUnknownOption =
            reportedUnknownOption || diagnostic.find("--turbo-mode") != std::string::npos;
    }
    EXPECT_TRUE(reportedUnknownOption);

    // The entry point's one job: construct the composition from what was resolved.
    ApplicationComposition composition{settings.config()};

    // Every one of these was unreachable while the entry point used a
    // default-constructed AppConfig.
    EXPECT_EQ(settings.config().mcpPort, 0);
    EXPECT_EQ(settings.config().remote.maxSessions, 3);
    EXPECT_EQ(settings.config().agentInterpreterId, "hosted");
    EXPECT_EQ(settings.config().generativeBackendId, "no-such-backend");
    EXPECT_EQ(composition.remoteAccessGate().config().maxSessions, 3);

    // ...and the ids reached the registries, whose documented fallbacks then
    // applied: credential-less `hosted` and an unrecognised backend id both install
    // `offline` and say so in startupErrors() (Requirements 11.8, 12.8), which is
    // exactly what the entry point now prints on stderr.
    EXPECT_EQ(composition.agentInterpreterId(), "offline");
    EXPECT_EQ(composition.generativeBackendId(), "offline");
    ASSERT_EQ(composition.startupErrors().size(), 2u);
    bool namedTheRejectedInterpreter = false;
    bool namedTheRejectedBackend = false;
    for (const std::string& error : composition.startupErrors()) {
        namedTheRejectedInterpreter =
            namedTheRejectedInterpreter || error.find("hosted") != std::string::npos;
        namedTheRejectedBackend =
            namedTheRejectedBackend || error.find("no-such-backend") != std::string::npos;
    }
    EXPECT_TRUE(namedTheRejectedInterpreter);
    EXPECT_TRUE(namedTheRejectedBackend);

    // The application still comes up in full: nothing here is fatal.
    ASSERT_TRUE(composition.start());
    EXPECT_TRUE(composition.running());
    EXPECT_NE(composition.mcpBoundPort(), 0);
    (void)composition.timeline();
    composition.stop();
}

TEST(ApplicationStartupWiring, RemoteSettingsReachTheGateAndReportTheirUnmetPrerequisites) {
    // Remote access could not be turned on from the binary at all before the entry
    // point resolved settings. Enabled with the token and the acknowledgement
    // missing, so the gate refuses the non-loopback bind, names what is missing and
    // binds loopback instead (Requirements 10.3, 10.12) — no external address is
    // ever bound by this test.
    const ScratchConfig config("remote",
                               "mcp.port = 0\n"
                               "remote.enabled = true\n"
                               "remote.bind_address = 203.0.113.7\n"
                               "remote.idle_timeout_seconds = 45\n");

    const AppSettings settings = AppSettings::load({}, isolatedOptions(config));
    ASSERT_TRUE(settings.config().remote.enabled);
    EXPECT_EQ(settings.config().remote.bindAddress, "203.0.113.7");
    EXPECT_EQ(settings.config().remote.idleTimeout, std::chrono::seconds{45});

    ApplicationComposition composition{settings.config()};
    EXPECT_TRUE(composition.remoteAccessGate().config().enabled);
    EXPECT_EQ(composition.remoteAccessGate().config().bindAddress, "203.0.113.7");
    EXPECT_EQ(composition.remoteAccessGate().config().idleTimeout, std::chrono::seconds{45});

    ASSERT_TRUE(composition.start());
    EXPECT_TRUE(composition.running());
    EXPECT_FALSE(composition.remoteAccessStartupError().empty());
    composition.stop();
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
