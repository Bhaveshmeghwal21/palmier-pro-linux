// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/services/agent_orchestrator_test.cpp — unit tests for the in-app agent
// chat orchestrator (task 16.1; Requirements 8.1, 8.5, 8.6, 8.7).
//
// The agent must operate on the current project using the SAME tools as the MCP
// server, so these tests drive AgentOrchestrator over a real TimelineEngine, the
// shared ToolRegistry, and the real McpToolExecutor — the exact path the MCP
// server uses (Property P4). They cover every branch of the send policy:
//
//   * 8.1/8.6 — a message translated into a tool call mutates the current
//               project, and the change is reflected synchronously (observable
//               the instant sendMessage returns).
//   * 8.7     — a failed edit surfaces an error and leaves the project state
//               unchanged (the shared executor rolls back).
//   * 8.5     — a message sent without an active subscription or BYOK is
//               rejected (Unauthenticated) with a prompt to authenticate, and
//               the unsent message content is preserved for retry. Covered with
//               a mock gate and with the real AuthServiceAgentGate over the
//               AuthenticationService (subscription and BYOK branches).
//
// A scripted IntentInterpreter maps fixed message strings to tool calls so the
// natural-language → tool mapping (an LLM in production) is stubbed out and the
// orchestrator's routing/policy is exercised deterministically.

#include "services/AgentOrchestrator.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include "core/Duration.hpp"
#include "core/EditCommands.hpp"
#include "core/FrameRate.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/Project.hpp"
#include "core/Resolution.hpp"
#include "core/TimelineEngine.hpp"
#include "core/Track.hpp"
#include "core/Uuid.hpp"
#include "services/AuthenticationService.hpp"
#include "services/ByokCredentialManager.hpp"
#include "services/ByokCredentials.hpp"
#include "services/Json.hpp"
#include "services/McpToolExecutor.hpp"
#include "services/ProjectSession.hpp"
#include "services/SecretStore.hpp"
#include "services/ToolRegistry.hpp"

namespace palmier::services {
namespace {

// The tool surface and the executor act on a ProjectSession (task 3.4; design.md
// D1): the session owns one TimelineEngine for its lifetime and the handlers
// resolve it at invocation time. Tests seed their fixture project into that one
// engine the way `project.open` will.
void seedSession(ProjectSession& session, Project project) {
    (void)session.engine().reset(std::move(project));
}

// ---------------------------------------------------------------------------
// Fixtures / helpers
// ---------------------------------------------------------------------------

Project makeProject(Uuid& trackId, Uuid& assetId) {
    Project project;
    project.id = Uuid::generateV4();
    project.name = "Agent Orchestrator Test";
    project.timelineFps = FrameRate::fps30();
    project.canvas = Resolution::hd1080();

    Track track;
    track.id = Uuid::generateV4();
    track.kind = TrackKind::Video;

    MediaAssetRef asset(Uuid::generateV4(), "/media/a.mp4");
    project.assets.push_back(asset);
    project.tracks.push_back(track);

    trackId = track.id;
    assetId = asset.assetId;
    return project;
}

std::size_t clipCount(const TimelineEngine& engine) {
    std::size_t total = 0;
    for (const Track& t : engine.snapshot().tracks) total += t.clips.size();
    return total;
}

// A gate whose verdict is set by the test.
class MockGate : public IAgentAuthGate {
public:
    Result<void> verdict = ok();
    [[nodiscard]] Result<void> authorize() const override { return verdict; }
};

// An interpreter that maps a couple of fixed phrases to concrete tool calls.
IntentInterpreter makeInterpreter(const Uuid& trackId, const Uuid& assetId) {
    return [trackId, assetId](std::string_view message) -> Result<AgentIntent> {
        if (message == "add a clip") {
            Json args = Json::object();
            args.set("trackId", trackId.toString());
            args.set("assetId", assetId.toString());
            args.set("sourceOutNs", static_cast<std::int64_t>(1'000'000'000));
            return AgentIntent{"timeline.add_clip", std::move(args)};
        }
        if (message == "read the timeline") {
            return AgentIntent{"timeline.read", Json::object()};
        }
        if (message == "delete the ghost") {
            Json args = Json::object();
            args.set("clipId", Uuid::generateV4().toString());  // not in the project
            return AgentIntent{"timeline.delete_clip", std::move(args)};
        }
        return err<AgentIntent>(makeError(ErrorCode::InvalidArgument,
                                          "could not understand the request"));
    };
}

// Count messages of a given role in the transcript.
std::size_t countRole(const ChatSession& session, ChatRole role) {
    std::size_t n = 0;
    for (const ChatMessage& m : session.messages()) {
        if (m.role == role) ++n;
    }
    return n;
}

// ---------------------------------------------------------------------------
// 8.1 / 8.6 — a message drives the shared tools and mutates the current project
// ---------------------------------------------------------------------------

TEST(AgentOrchestrator, MessageAppliesEditThroughSharedExecutor) {
    Uuid trackId, assetId;
    ProjectSession session;
    seedSession(session, makeProject(trackId, assetId));
    TimelineEngine& engine = session.engine();
    ToolRegistry registry = buildDefaultToolRegistry(session);
    McpToolExecutor executor(registry, &session);
    MockGate gate;  // authorized by default

    AgentOrchestrator agent(executor, gate, makeInterpreter(trackId, assetId));

    ASSERT_EQ(clipCount(engine), 0u);
    Result<AgentTurn> turn = agent.sendMessage("add a clip");

    ASSERT_TRUE(turn.isOk());
    EXPECT_EQ(turn.value().toolName, "timeline.add_clip");
    // 8.6 — the edit is reflected in the project state synchronously.
    EXPECT_EQ(clipCount(engine), 1u);
    // The transcript records the user turn and the agent response.
    EXPECT_EQ(countRole(agent.session(), ChatRole::User), 1u);
    EXPECT_EQ(countRole(agent.session(), ChatRole::Agent), 1u);
}

TEST(AgentOrchestrator, ReadToolReturnsProjectState) {
    Uuid trackId, assetId;
    ProjectSession session;
    seedSession(session, makeProject(trackId, assetId));
    ToolRegistry registry = buildDefaultToolRegistry(session);
    McpToolExecutor executor(registry, &session);
    MockGate gate;

    AgentOrchestrator agent(executor, gate, makeInterpreter(trackId, assetId));

    Result<AgentTurn> turn = agent.sendMessage("read the timeline");
    ASSERT_TRUE(turn.isOk());
    EXPECT_TRUE(turn.value().result.isObject());
    EXPECT_TRUE(turn.value().result.contains("tracks"));
}

// ---------------------------------------------------------------------------
// 8.7 — a failed edit surfaces an error and leaves the project unchanged
// ---------------------------------------------------------------------------

TEST(AgentOrchestrator, FailedEditLeavesProjectUnchangedAndReportsError) {
    Uuid trackId, assetId;
    ProjectSession session;
    seedSession(session, makeProject(trackId, assetId));
    TimelineEngine& engine = session.engine();
    ToolRegistry registry = buildDefaultToolRegistry(session);
    McpToolExecutor executor(registry, &session);
    MockGate gate;

    AgentOrchestrator agent(executor, gate, makeInterpreter(trackId, assetId));

    // Seed a legitimate clip first so we can confirm the failed op changes nothing.
    ASSERT_TRUE(agent.sendMessage("add a clip").isOk());
    const std::size_t before = clipCount(engine);
    ASSERT_EQ(before, 1u);

    // Deleting a clip that does not exist fails; the executor rolls back.
    Result<AgentTurn> turn = agent.sendMessage("delete the ghost");
    ASSERT_TRUE(turn.isError());
    EXPECT_EQ(turn.error().code(), ErrorCode::NotFound);

    // 8.7 — project state is unchanged from before the failed operation.
    EXPECT_EQ(clipCount(engine), before);
    // A system error entry describes the failure, and it is flagged as an error.
    const ChatMessage* last = agent.session().last();
    ASSERT_NE(last, nullptr);
    EXPECT_EQ(last->role, ChatRole::System);
    EXPECT_TRUE(last->isError);
}

TEST(AgentOrchestrator, UninterpretableMessageDoesNotMutateProject) {
    Uuid trackId, assetId;
    ProjectSession session;
    seedSession(session, makeProject(trackId, assetId));
    TimelineEngine& engine = session.engine();
    ToolRegistry registry = buildDefaultToolRegistry(session);
    McpToolExecutor executor(registry, &session);
    MockGate gate;

    AgentOrchestrator agent(executor, gate, makeInterpreter(trackId, assetId));

    Result<AgentTurn> turn = agent.sendMessage("please make me a sandwich");
    ASSERT_TRUE(turn.isError());
    EXPECT_EQ(turn.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(clipCount(engine), 0u);
}

// ---------------------------------------------------------------------------
// 8.5 — unauthorized send is rejected and the unsent content is preserved
// ---------------------------------------------------------------------------

TEST(AgentOrchestrator, UnauthorizedSendRejectedAndPreservesMessage) {
    Uuid trackId, assetId;
    ProjectSession session;
    seedSession(session, makeProject(trackId, assetId));
    TimelineEngine& engine = session.engine();
    ToolRegistry registry = buildDefaultToolRegistry(session);
    McpToolExecutor executor(registry, &session);

    MockGate gate;
    gate.verdict = err(makeError(ErrorCode::Unauthenticated, "auth required"));

    AgentOrchestrator agent(executor, gate, makeInterpreter(trackId, assetId));

    Result<AgentTurn> turn = agent.sendMessage("add a clip");
    ASSERT_TRUE(turn.isError());
    EXPECT_EQ(turn.error().code(), ErrorCode::Unauthenticated);

    // Nothing was executed: the project is untouched.
    EXPECT_EQ(clipCount(engine), 0u);
    // The unsent content is preserved so the user does not lose their text.
    EXPECT_TRUE(agent.hasPendingMessage());
    EXPECT_EQ(agent.pendingMessage(), "add a clip");
    // The rejected message is NOT recorded as a sent user turn.
    EXPECT_EQ(countRole(agent.session(), ChatRole::User), 0u);
}

TEST(AgentOrchestrator, PreservedMessageClearedAfterSuccessfulSend) {
    Uuid trackId, assetId;
    ProjectSession session;
    seedSession(session, makeProject(trackId, assetId));
    TimelineEngine& engine = session.engine();
    ToolRegistry registry = buildDefaultToolRegistry(session);
    McpToolExecutor executor(registry, &session);

    MockGate gate;
    gate.verdict = err(makeError(ErrorCode::Unauthenticated, "auth required"));

    AgentOrchestrator agent(executor, gate, makeInterpreter(trackId, assetId));

    ASSERT_TRUE(agent.sendMessage("add a clip").isError());
    ASSERT_TRUE(agent.hasPendingMessage());

    // The user authenticates; the next send succeeds and clears the pending text.
    gate.verdict = ok();
    ASSERT_TRUE(agent.sendMessage("add a clip").isOk());
    EXPECT_FALSE(agent.hasPendingMessage());
    EXPECT_TRUE(agent.pendingMessage().empty());
    EXPECT_EQ(clipCount(engine), 1u);
}

// ---------------------------------------------------------------------------
// 8.5 — the real AuthServiceAgentGate over AuthenticationService
// ---------------------------------------------------------------------------

class ScriptedAuthBackend : public AuthBackend {
public:
    explicit ScriptedAuthBackend(EntitlementStatus entitlement) : entitlement_(entitlement) {}
    Result<BackendSession> authenticate(const LoginCredentials&) override {
        return BackendSession{"token", entitlement_};
    }

private:
    EntitlementStatus entitlement_;
};

// A BYOK provider validator that accepts any well-formed credential.
class AcceptingByokValidator : public ByokProviderValidator {
public:
    Result<void> validate(const ByokCredential&) override { return ok(); }
};

TEST(AgentOrchestrator, RealGateRejectsWithoutSubscriptionOrByok) {
    ScriptedAuthBackend backend(EntitlementStatus::None);
    AuthenticationService auth(backend);
    AuthServiceAgentGate gate(auth, {"openai"});

    // No session and no BYOK -> unauthorized.
    EXPECT_TRUE(gate.authorize().isError());
    EXPECT_EQ(gate.authorize().error().code(), ErrorCode::Unauthenticated);
}

TEST(AgentOrchestrator, RealGateAuthorizesWithActiveSubscription) {
    ScriptedAuthBackend backend(EntitlementStatus::Active);
    AuthenticationService auth(backend);
    ASSERT_TRUE(auth.login(LoginCredentials{"ada@example.com", "pw"}).isOk());

    AuthServiceAgentGate gate(auth, {"openai"});
    EXPECT_TRUE(gate.authorize().isOk());
}

TEST(AgentOrchestrator, RealGateAuthorizesWithByokCredentials) {
    // No active subscription, but a validated BYOK credential authorizes the agent.
    ScriptedAuthBackend backend(EntitlementStatus::None);
    AuthenticationService auth(backend);

    AcceptingByokValidator validator;
    InMemorySecretStore store;
    ByokCredentialManager byok(validator, store, "ada");
    auth.setByokManager(byok);

    ASSERT_TRUE(auth.saveByokCredentials(ByokCredential{"openai", "sk-test"}).isOk());
    ASSERT_TRUE(auth.isByokAuthorized("openai"));

    AuthServiceAgentGate gate(auth, {"openai"});
    EXPECT_TRUE(gate.authorize().isOk());
}

TEST(AgentOrchestrator, RealGateDrivesEndToEndSend) {
    // Wire the real gate into a full orchestrator and confirm the gated send path
    // works end to end: rejected before login, accepted after an active login.
    Uuid trackId, assetId;
    ProjectSession session;
    seedSession(session, makeProject(trackId, assetId));
    TimelineEngine& engine = session.engine();
    ToolRegistry registry = buildDefaultToolRegistry(session);
    McpToolExecutor executor(registry, &session);

    ScriptedAuthBackend backend(EntitlementStatus::Active);
    AuthenticationService auth(backend);
    AuthServiceAgentGate gate(auth, {"openai"});

    AgentOrchestrator agent(executor, gate, makeInterpreter(trackId, assetId));

    // Before login: rejected, message preserved, project untouched.
    ASSERT_TRUE(agent.sendMessage("add a clip").isError());
    EXPECT_TRUE(agent.hasPendingMessage());
    EXPECT_EQ(clipCount(engine), 0u);

    // After an active login: the same message now applies the edit.
    ASSERT_TRUE(auth.login(LoginCredentials{"ada@example.com", "pw"}).isOk());
    ASSERT_TRUE(agent.sendMessage("add a clip").isOk());
    EXPECT_FALSE(agent.hasPendingMessage());
    EXPECT_EQ(clipCount(engine), 1u);
}

// ---------------------------------------------------------------------------
// The mention-resolution seam (task 16.2) can reject a message before execution
// ---------------------------------------------------------------------------

TEST(AgentOrchestrator, PreprocessorRejectionBlocksSubmission) {
    Uuid trackId, assetId;
    ProjectSession session;
    seedSession(session, makeProject(trackId, assetId));
    TimelineEngine& engine = session.engine();
    ToolRegistry registry = buildDefaultToolRegistry(session);
    McpToolExecutor executor(registry, &session);
    MockGate gate;

    AgentOrchestrator agent(executor, gate, makeInterpreter(trackId, assetId));
    // Simulate an unmatched @-mention (task 16.2 wires the real resolver here).
    agent.setPreprocessor([](std::string) -> Result<std::string> {
        return err<std::string>(notFound("referenced media item was not found"));
    });

    Result<AgentTurn> turn = agent.sendMessage("add a clip @missing");
    ASSERT_TRUE(turn.isError());
    EXPECT_EQ(turn.error().code(), ErrorCode::NotFound);
    // Nothing was submitted for processing; the project is untouched.
    EXPECT_EQ(clipCount(engine), 0u);
    EXPECT_EQ(countRole(agent.session(), ChatRole::User), 0u);
}

}  // namespace
}  // namespace palmier::services
