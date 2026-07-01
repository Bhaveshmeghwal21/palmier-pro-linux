// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/app/application_composition_test.cpp — unit tests for the application
// composition root (task 21.1; Requirements 1.6, 7.1, 7.2, 7.9, 13.3).
//
// These tests exercise the Qt-free composition root headlessly: that every
// component is composed and cross-wired, that the MCP server starts on launch
// and stops on close within its lifecycle, that a request served through the
// composed server reaches the SAME TimelineEngine the tool executor drives, and
// that the offline defaults keep the editor + MCP endpoint fully functional with
// no network connection (13.3). They bind the MCP endpoint on an ephemeral port
// (mcpPort == 0) so they never contend for the well-known 19789.

#include <gtest/gtest.h>

#include <string>

#include "app/ApplicationComposition.hpp"
#include "core/Error.hpp"
#include "core/Result.hpp"
#include "gpu/GpuContext.hpp"
#include "services/AgentOrchestrator.hpp"
#include "services/Json.hpp"
#include "services/LocalizationManager.hpp"
#include "services/McpServer.hpp"
#include "services/McpToolExecutor.hpp"
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

    // The executor is bound to a project (the composed TimelineEngine), so MCP
    // tool calls operate on the current project rather than reporting "no
    // project open" (Requirement 7.10 is about the null case).
    EXPECT_TRUE(composition.executor().hasProject());

    // The MCP endpoint is not accepting connections until start() is called.
    EXPECT_FALSE(composition.running());
    EXPECT_EQ(composition.mcpBoundPort(), 0);

    // The default tool surface is present and shared (Property P4 relies on this
    // single registry being what both the MCP server and the agent drive).
    EXPECT_GT(composition.toolRegistry().size(), 0u);
    EXPECT_TRUE(composition.toolRegistry().has("timeline.read"));
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
    // read tool. This exercises server -> executor -> ToolRegistry -> engine.
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
