// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/services/mention_auth_gating_test.cpp — the dedicated combined unit-test
// suite for @-mention resolution AND agent-chat auth gating (task 16.3;
// Requirements 8.2, 8.3, 8.4, 8.5).
//
// Tasks 16.1 and 16.2 each proved one half of the agent send policy in
// isolation:
//   * tests/services/agent_orchestrator_test.cpp (16.1) drives the orchestrator
//     over the shared executor and covers the auth gate (8.5), edit success
//     (8.6), and failed-edit rollback (8.7) — with the mention seam stubbed.
//   * tests/services/mention_resolver_test.cpp (16.2) drives the MentionResolver
//     directly and through its MessagePreprocessor adapters (8.2/8.3/8.4) — with
//     an always-authorizing gate.
//
// THIS suite is the 16.3 checklist executed END-TO-END through the
// AgentOrchestrator with the real @-mention preprocessor and the auth gate wired
// together, so the two policies are exercised as one pipeline rather than each in
// isolation. Its complementary value over the two files above is the INTERACTION
// between the gate and the mention resolver:
//
//   * single-match resolve (8.2) — a matched @mention is rewritten to its
//     canonical @<assetId> and reaches the interpreter through an authorized send.
//   * no-match rejection (8.3) — an unmatched @mention rejects the message before
//     submission (interpreter never invoked; project untouched).
//   * multi-match prompt (8.4) — an ambiguous @mention rejects with the candidate
//     set to choose from, before submission.
//   * unauthenticated gating (8.5) — an unauthorized send is rejected with an
//     authenticate prompt and the unsent text (INCLUDING the raw @mention) is
//     preserved for retry.
//   * ORDERING — the auth gate runs BEFORE mention resolution: an unauthorized
//     send of a message whose @mention is unmatched/ambiguous is rejected as
//     Unauthenticated (never NotFound / FailedPrecondition), the mention
//     preprocessor never runs, and the raw text is preserved; after
//     authenticating, the same preserved message then resolves and is submitted.
//
// Following the mention-resolver suite's isolated-source compile pattern, the
// resolver, orchestrator, executor, tool surface, JSON value, auth, and BYOK
// sources are compiled directly into this binary alongside Palmier::core (which
// supplies the real TimelineEngine, EditCommands, and MediaManager), without the
// media/FFmpeg, libsecret, or lcms2 dependencies the full Palmier::services
// library links.

#include "services/MentionResolver.hpp"

#include <string>
#include <string_view>
#include <vector>
#include <utility>

#include <gtest/gtest.h>

#include "core/FrameRate.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/Project.hpp"
#include "core/Resolution.hpp"
#include "core/TimelineEngine.hpp"
#include "core/Track.hpp"
#include "core/Uuid.hpp"
#include "services/AgentOrchestrator.hpp"
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

MediaAssetRef makeAsset(std::string path) {
    return MediaAssetRef(Uuid::generateV4(), std::move(path));
}

// A minimal single-track project with one imported asset so the shared executor
// has a real project to read/mutate.
Project makeProject(const MediaAssetRef& intro) {
    Project project;
    project.id = Uuid::generateV4();
    project.name = "Mention + Auth Gating Test";
    project.timelineFps = FrameRate::fps30();
    project.canvas = Resolution::hd1080();

    Track track;
    track.id = Uuid::generateV4();
    track.kind = TrackKind::Video;

    project.assets.push_back(intro);
    project.tracks.push_back(track);
    return project;
}

std::size_t clipCount(const TimelineEngine& engine) {
    std::size_t total = 0;
    for (const Track& t : engine.snapshot().tracks) total += t.clips.size();
    return total;
}

std::size_t countRole(const ChatSession& session, ChatRole role) {
    std::size_t n = 0;
    for (const ChatMessage& m : session.messages()) {
        if (m.role == role) ++n;
    }
    return n;
}

// A gate whose verdict is chosen by the test.
class MockGate : public IAgentAuthGate {
public:
    Result<void> verdict = ok();
    [[nodiscard]] Result<void> authorize() const override { return verdict; }
};

// Records the message it received and how many times it was invoked, then maps
// to a harmless read tool. "seen" lets us assert the interpreter saw the
// canonical rewrite; "calls" lets us assert it was NOT invoked when a message is
// rejected before submission.
struct RecordingInterpreter {
    std::string* seen;
    int*         calls;
    Result<AgentIntent> operator()(std::string_view message) const {
        *seen = std::string(message);
        ++*calls;
        return AgentIntent{"timeline.read", Json::object()};
    }
};

// A preprocessor wrapper that records whether the wrapped mention resolver ran.
// Used to prove the ORDERING guarantee (auth gate before mention resolution).
struct TracingPreprocessor {
    MessagePreprocessor inner;
    bool*               ran;
    Result<std::string> operator()(std::string message) const {
        *ran = true;
        return inner(std::move(message));
    }
};

// ---------------------------------------------------------------------------
// 8.2 — a single matching @mention resolves and reaches the interpreter
// ---------------------------------------------------------------------------

TEST(MentionAuthGating, SingleMatchResolvesAndReachesInterpreterAuthorized) {
    MediaAssetRef intro = makeAsset("/media/intro.mp4");
    ProjectSession session;
    seedSession(session, makeProject(intro));
    ToolRegistry registry = buildDefaultToolRegistry(session);
    McpToolExecutor executor(registry, &session);
    MockGate gate;  // authorized by default

    std::string seen;
    int calls = 0;
    AgentOrchestrator agent(executor, gate, RecordingInterpreter{&seen, &calls});
    agent.setPreprocessor(makeMentionPreprocessor(std::vector<MediaAssetRef>{intro}));

    const Result<AgentTurn> turn = agent.sendMessage("read @intro please");
    ASSERT_TRUE(turn.isOk());
    // The interpreter was invoked exactly once, with the mention rewritten to the
    // canonical @<assetId> reference (not the raw @intro).
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(seen, "read @" + intro.assetId.toString() + " please");
    // The send was accepted and recorded as a user turn.
    EXPECT_EQ(countRole(agent.session(), ChatRole::User), 1u);
}

// ---------------------------------------------------------------------------
// 8.3 — an unmatched @mention rejects the message BEFORE submission
// ---------------------------------------------------------------------------

TEST(MentionAuthGating, NoMatchRejectedBeforeSubmissionAuthorized) {
    MediaAssetRef intro = makeAsset("/media/intro.mp4");
    ProjectSession session;
    seedSession(session, makeProject(intro));
    TimelineEngine& engine = session.engine();
    ToolRegistry registry = buildDefaultToolRegistry(session);
    McpToolExecutor executor(registry, &session);
    MockGate gate;

    std::string seen;
    int calls = 0;
    AgentOrchestrator agent(executor, gate, RecordingInterpreter{&seen, &calls});
    agent.setPreprocessor(makeMentionPreprocessor(std::vector<MediaAssetRef>{intro}));

    const Result<AgentTurn> turn = agent.sendMessage("read @ghost");
    ASSERT_TRUE(turn.isError());
    // 8.3 — the item was not found; the message was never submitted.
    EXPECT_EQ(turn.error().code(), ErrorCode::NotFound);
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(clipCount(engine), 0u);
    // Not recorded as a submitted user turn; a system error notice is recorded.
    EXPECT_EQ(countRole(agent.session(), ChatRole::User), 0u);
    const ChatMessage* last = agent.session().last();
    ASSERT_NE(last, nullptr);
    EXPECT_EQ(last->role, ChatRole::System);
    EXPECT_TRUE(last->isError);
}

// ---------------------------------------------------------------------------
// 8.4 — an ambiguous @mention prompts for selection BEFORE submission
// ---------------------------------------------------------------------------

TEST(MentionAuthGating, MultiMatchPromptsBeforeSubmissionAuthorized) {
    MediaAssetRef clipA = makeAsset("/a/clip.mp4");
    MediaAssetRef clipB = makeAsset("/b/clip.mp4");
    ProjectSession session;
    seedSession(session, makeProject(clipA));
    ToolRegistry registry = buildDefaultToolRegistry(session);
    McpToolExecutor executor(registry, &session);
    MockGate gate;

    std::string seen;
    int calls = 0;
    AgentOrchestrator agent(executor, gate, RecordingInterpreter{&seen, &calls});
    agent.setPreprocessor(
        makeMentionPreprocessor(std::vector<MediaAssetRef>{clipA, clipB}));

    const Result<AgentTurn> turn = agent.sendMessage("trim @clip");
    ASSERT_TRUE(turn.isError());
    // 8.4 — a selection is required; the message was not submitted.
    EXPECT_EQ(turn.error().code(), ErrorCode::FailedPrecondition);
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(countRole(agent.session(), ChatRole::User), 0u);

    // The user is prompted to select — the resolver surfaces the candidate set the
    // UI renders for that selection.
    const MentionResolution resolution =
        MentionResolver({clipA, clipB}).resolve("trim @clip");
    EXPECT_EQ(resolution.status, MentionStatus::Ambiguous);
    EXPECT_EQ(resolution.problemMention, "clip");
    ASSERT_EQ(resolution.candidates.size(), 2u);
    EXPECT_NE(resolution.candidates[0].assetId, resolution.candidates[1].assetId);
}

// ---------------------------------------------------------------------------
// 8.5 — an unauthenticated send is rejected and the unsent content preserved
// ---------------------------------------------------------------------------

TEST(MentionAuthGating, UnauthenticatedSendRejectedAndMessagePreserved) {
    MediaAssetRef intro = makeAsset("/media/intro.mp4");
    ProjectSession session;
    seedSession(session, makeProject(intro));
    TimelineEngine& engine = session.engine();
    ToolRegistry registry = buildDefaultToolRegistry(session);
    McpToolExecutor executor(registry, &session);

    MockGate gate;
    gate.verdict = err(makeError(ErrorCode::Unauthenticated,
                                 "please authenticate to use the agent chat"));

    std::string seen;
    int calls = 0;
    AgentOrchestrator agent(executor, gate, RecordingInterpreter{&seen, &calls});
    agent.setPreprocessor(makeMentionPreprocessor(std::vector<MediaAssetRef>{intro}));

    const Result<AgentTurn> turn = agent.sendMessage("read the timeline");
    ASSERT_TRUE(turn.isError());
    EXPECT_EQ(turn.error().code(), ErrorCode::Unauthenticated);
    // The interpreter never ran, the project is untouched.
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(clipCount(engine), 0u);
    // The unsent content is preserved so the user does not lose their text.
    EXPECT_TRUE(agent.hasPendingMessage());
    EXPECT_EQ(agent.pendingMessage(), "read the timeline");
    EXPECT_EQ(countRole(agent.session(), ChatRole::User), 0u);
}

// ---------------------------------------------------------------------------
// ORDERING — the auth gate runs BEFORE mention resolution
// ---------------------------------------------------------------------------

TEST(MentionAuthGating, AuthGateRunsBeforeMentionResolution) {
    MediaAssetRef intro = makeAsset("/media/intro.mp4");
    ProjectSession session;
    seedSession(session, makeProject(intro));
    ToolRegistry registry = buildDefaultToolRegistry(session);
    McpToolExecutor executor(registry, &session);

    MockGate gate;
    gate.verdict = err(makeError(ErrorCode::Unauthenticated, "authenticate first"));

    std::string seen;
    int calls = 0;
    bool mentionRan = false;
    AgentOrchestrator agent(executor, gate, RecordingInterpreter{&seen, &calls});
    agent.setPreprocessor(TracingPreprocessor{
        makeMentionPreprocessor(std::vector<MediaAssetRef>{intro}), &mentionRan});

    // The @mention here is UNMATCHED — resolution alone would fail with NotFound.
    // But the gate rejects first, so the error is Unauthenticated, the mention
    // preprocessor never runs, and the RAW text (mention intact) is preserved.
    const Result<AgentTurn> turn = agent.sendMessage("cut @ghost from the clip");
    ASSERT_TRUE(turn.isError());
    EXPECT_EQ(turn.error().code(), ErrorCode::Unauthenticated);
    EXPECT_FALSE(mentionRan);
    EXPECT_EQ(calls, 0);
    EXPECT_TRUE(agent.hasPendingMessage());
    EXPECT_EQ(agent.pendingMessage(), "cut @ghost from the clip");
}

// ---------------------------------------------------------------------------
// Combined — a preserved @mention message resolves after authenticating
// ---------------------------------------------------------------------------

TEST(MentionAuthGating, PreservedMentionMessageResolvesAfterAuthenticating) {
    MediaAssetRef intro = makeAsset("/media/intro.mp4");
    ProjectSession session;
    seedSession(session, makeProject(intro));
    ToolRegistry registry = buildDefaultToolRegistry(session);
    McpToolExecutor executor(registry, &session);

    MockGate gate;
    gate.verdict = err(makeError(ErrorCode::Unauthenticated, "authenticate first"));

    std::string seen;
    int calls = 0;
    AgentOrchestrator agent(executor, gate, RecordingInterpreter{&seen, &calls});
    agent.setPreprocessor(makeMentionPreprocessor(std::vector<MediaAssetRef>{intro}));

    // Unauthorized: the message with a VALID @mention is rejected and preserved
    // with the mention still in its raw form (resolution has not run yet).
    ASSERT_TRUE(agent.sendMessage("read @intro now").isError());
    ASSERT_TRUE(agent.hasPendingMessage());
    EXPECT_EQ(agent.pendingMessage(), "read @intro now");
    EXPECT_EQ(calls, 0);

    // The user authenticates and retries the preserved text. Now the gate passes,
    // the mention resolves to its canonical id, and the send is accepted.
    gate.verdict = ok();
    const Result<AgentTurn> turn = agent.sendMessage(agent.pendingMessage());
    ASSERT_TRUE(turn.isOk());
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(seen, "read @" + intro.assetId.toString() + " now");
    // The preserved pending message is cleared on the successful send.
    EXPECT_FALSE(agent.hasPendingMessage());
    EXPECT_TRUE(agent.pendingMessage().empty());
}

// ---------------------------------------------------------------------------
// Combined — the REAL AuthServiceAgentGate + mention resolution end to end
// ---------------------------------------------------------------------------

class ScriptedAuthBackend : public AuthBackend {
public:
    explicit ScriptedAuthBackend(EntitlementStatus entitlement)
        : entitlement_(entitlement) {}
    Result<BackendSession> authenticate(const LoginCredentials&) override {
        return BackendSession{"token", entitlement_};
    }

private:
    EntitlementStatus entitlement_;
};

TEST(MentionAuthGating, RealGateGatesThenMentionResolvesEndToEnd) {
    MediaAssetRef intro = makeAsset("/media/intro.mp4");
    ProjectSession session;
    seedSession(session, makeProject(intro));
    ToolRegistry registry = buildDefaultToolRegistry(session);
    McpToolExecutor executor(registry, &session);

    ScriptedAuthBackend backend(EntitlementStatus::Active);
    AuthenticationService auth(backend);
    AuthServiceAgentGate gate(auth, {"openai"});

    std::string seen;
    int calls = 0;
    AgentOrchestrator agent(executor, gate, RecordingInterpreter{&seen, &calls});
    agent.setPreprocessor(makeMentionPreprocessor(std::vector<MediaAssetRef>{intro}));

    // Before login: the real gate rejects (no subscription, no BYOK); the @mention
    // message is preserved and resolution never runs.
    const Result<AgentTurn> blocked = agent.sendMessage("read @intro");
    ASSERT_TRUE(blocked.isError());
    EXPECT_EQ(blocked.error().code(), ErrorCode::Unauthenticated);
    EXPECT_EQ(calls, 0);
    EXPECT_TRUE(agent.hasPendingMessage());
    EXPECT_EQ(agent.pendingMessage(), "read @intro");

    // After an active-entitlement login: the same message is authorized, the
    // mention resolves to its canonical id, and it reaches the interpreter.
    ASSERT_TRUE(auth.login(LoginCredentials{"ada@example.com", "pw"}).isOk());
    const Result<AgentTurn> allowed = agent.sendMessage(agent.pendingMessage());
    ASSERT_TRUE(allowed.isOk());
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(seen, "read @" + intro.assetId.toString());
    EXPECT_FALSE(agent.hasPendingMessage());
}

class AcceptingByokValidator : public ByokProviderValidator {
public:
    Result<void> validate(const ByokCredential&) override { return ok(); }
};

TEST(MentionAuthGating, RealGateAuthorizesViaByokThenMentionResolves) {
    MediaAssetRef intro = makeAsset("/media/intro.mp4");
    ProjectSession session;
    seedSession(session, makeProject(intro));
    ToolRegistry registry = buildDefaultToolRegistry(session);
    McpToolExecutor executor(registry, &session);

    // No subscription, but a validated BYOK credential authorizes the agent.
    ScriptedAuthBackend backend(EntitlementStatus::None);
    AuthenticationService auth(backend);
    AcceptingByokValidator validator;
    InMemorySecretStore store;
    ByokCredentialManager byok(validator, store, "ada");
    auth.setByokManager(byok);

    AuthServiceAgentGate gate(auth, {"openai"});

    std::string seen;
    int calls = 0;
    AgentOrchestrator agent(executor, gate, RecordingInterpreter{&seen, &calls});
    agent.setPreprocessor(makeMentionPreprocessor(std::vector<MediaAssetRef>{intro}));

    // Before saving BYOK: rejected, mention message preserved.
    ASSERT_TRUE(agent.sendMessage("read @intro").isError());
    EXPECT_TRUE(agent.hasPendingMessage());

    // After saving a valid BYOK credential: authorized, mention resolves through.
    ASSERT_TRUE(auth.saveByokCredentials(ByokCredential{"openai", "sk-test"}).isOk());
    const Result<AgentTurn> turn = agent.sendMessage(agent.pendingMessage());
    ASSERT_TRUE(turn.isOk());
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(seen, "read @" + intro.assetId.toString());
    EXPECT_FALSE(agent.hasPendingMessage());
}

}  // namespace
}  // namespace palmier::services
