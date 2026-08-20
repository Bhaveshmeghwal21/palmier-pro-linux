// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/ui/agent_chat_viewmodel_test.cpp — unit tests for the Qt-free Agent Chat
// presentation model (task 19.6; Requirements 8.1, 8.2, 8.3, 8.4, 8.5, 10.4,
// 13.4).
//
// These exercise AgentChatViewModel directly, without any Qt runtime (the
// sandbox has no Qt 6). They cover the four concerns the panel adds on top of
// the AgentOrchestrator:
//
//   1. Send pipeline projection — a message sent through the model drives the
//      SAME shared tool executor as the MCP server and is reflected in the
//      transcript (8.1); an auth rejection is surfaced as
//      AuthenticationRequired with the unsent text preserved (8.5).
//   2. @-mention affordances — previewMentions() reports how a message's
//      mentions resolve (8.2), and a submitted unmatched (8.3) / ambiguous (8.4)
//      mention is surfaced with the structured candidate list the picker renders.
//   3. GPU-unavailable notice — a non-blocking notification derived from a
//      software-fallback GPU context (10.4).
//   4. Generative-service-unavailable error — an error state that leaves the
//      open-source editor fully functional (13.4).
//
// The model, the orchestrator, and the shared tool path are driven exactly as
// the MCP server drives them (real TimelineEngine + default ToolRegistry + real
// McpToolExecutor), with a scripted IntentInterpreter standing in for the LLM.

#include "ui/AgentChatViewModel.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/Duration.hpp"
#include "core/Error.hpp"
#include "core/FrameRate.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/Project.hpp"
#include "core/Resolution.hpp"
#include "core/Result.hpp"
#include "core/TimelineEngine.hpp"
#include "core/Track.hpp"
#include "core/Uuid.hpp"
#include "services/AgentOrchestrator.hpp"
#include "services/Json.hpp"
#include "services/McpToolExecutor.hpp"
#include "services/ProjectSession.hpp"
#include "services/ToolRegistry.hpp"

namespace palmier::ui {
namespace {

using palmier::services::AgentIntent;
using palmier::services::AgentOrchestrator;
using palmier::services::ChatRole;
using palmier::services::IAgentAuthGate;
using palmier::services::IntentInterpreter;
using palmier::services::Json;
using palmier::services::McpToolExecutor;
using palmier::services::ProjectSession;
using palmier::services::ToolRegistry;
using palmier::services::buildDefaultToolRegistry;

// ---------------------------------------------------------------------------
// Helpers / fixtures
// ---------------------------------------------------------------------------

Project makeProject(Uuid& trackId, Uuid& assetId) {
    Project project;
    project.id = Uuid::generateV4();
    project.name = "Agent Chat ViewModel Test";
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

// A gate whose verdict the test controls (subscription/BYOK stand-in, Req 8.5).
class MockGate : public IAgentAuthGate {
public:
    Result<void> verdict = ok();
    [[nodiscard]] Result<void> authorize() const override { return verdict; }
};

// Maps fixed phrases (after mention rewrite) to tool calls. "read ..." always
// maps to the read-only tool so mention-rewritten messages still interpret.
IntentInterpreter makeInterpreter(const Uuid& trackId, const Uuid& assetId) {
    return [trackId, assetId](std::string_view message) -> Result<AgentIntent> {
        if (message.rfind("read", 0) == 0) {
            return AgentIntent{"timeline.read", Json::object()};
        }
        if (message == "add a clip") {
            Json args = Json::object();
            args.set("trackId", trackId.toString());
            args.set("assetId", assetId.toString());
            args.set("sourceOutNs", static_cast<std::int64_t>(1'000'000'000));
            return AgentIntent{"timeline.add_clip", std::move(args)};
        }
        return err<AgentIntent>(
            makeError(ErrorCode::InvalidArgument, "could not understand the request"));
    };
}

// A GpuContext-like stand-in for makeGpuAccelerationStatus (Req 10.4). Mirrors
// the two accessors the real gpu::GpuContext exposes, so the model's status
// derivation is exercised without linking the GPU library.
struct FakeGpuContext {
    bool                       software = false;
    std::optional<std::string> notice;
    [[nodiscard]] bool isSoftwareFallback() const { return software; }
    [[nodiscard]] const std::optional<std::string>& unavailableNotice() const { return notice; }
};

// Seed `session` with `project` the way `project.open` will — through the one
// engine the session owns for its lifetime — and hand that engine back.
TimelineEngine& seedSession(ProjectSession& session, Project project) {
    (void)session.engine().reset(std::move(project));
    return session.engine();
}

// Owns the whole real send stack (project session -> registry -> executor -> gate
// -> orchestrator) so references stay alive for the model under test. The tool
// surface and the executor act on the session since task 3.4 (design.md D1);
// `engine` is just a view onto the one engine that session owns.
struct Harness {
    Uuid trackId;
    Uuid assetId;
    ProjectSession session;
    TimelineEngine& engine;
    ToolRegistry registry;
    McpToolExecutor executor;
    MockGate gate;
    AgentOrchestrator orchestrator;

    Harness()
        : engine(seedSession(session, makeProject(trackId, assetId))),
          registry(buildDefaultToolRegistry(session)),
          executor(registry, &session),
          orchestrator(executor, gate, makeInterpreter(trackId, assetId)) {}
};

std::size_t clipCount(const TimelineEngine& engine) {
    std::size_t total = 0;
    for (const Track& t : engine.snapshot().tracks) total += t.clips.size();
    return total;
}

std::vector<MediaAssetRef> makeMentionLibrary() {
    // Two assets whose base name collides ("clip") -> "@clip" is ambiguous (8.4);
    // "solo" is unique (8.2); anything else is unmatched (8.3).
    return {
        MediaAssetRef(Uuid::generateV4(), "/media/a/clip.mp4"),
        MediaAssetRef(Uuid::generateV4(), "/media/b/clip.mp4"),
        MediaAssetRef(Uuid::generateV4(), "/media/solo.mp4"),
    };
}

// ---------------------------------------------------------------------------
// 8.1 — a sent message drives the shared tools and lands in the transcript
// ---------------------------------------------------------------------------

TEST(AgentChatViewModel, SendReflectsInTranscript) {
    Harness h;
    AgentChatViewModel model(h.orchestrator);

    ASSERT_TRUE(model.transcript().empty());
    const ChatSendResult result = model.sendMessage("add a clip");

    EXPECT_EQ(result.status, ChatSendStatus::Applied);
    ASSERT_TRUE(result.toolName.has_value());
    EXPECT_EQ(*result.toolName, "timeline.add_clip");
    // 8.6 — the edit is reflected in the project synchronously.
    EXPECT_EQ(clipCount(h.engine), 1u);

    // The transcript now carries the user turn and the agent response.
    bool sawUser = false;
    bool sawAgent = false;
    for (const auto& m : model.transcript()) {
        sawUser |= (m.role == ChatRole::User);
        sawAgent |= (m.role == ChatRole::Agent);
    }
    EXPECT_TRUE(sawUser);
    EXPECT_TRUE(sawAgent);
}

TEST(AgentChatViewModel, SendDraftClearsDraftOnSuccess) {
    Harness h;
    AgentChatViewModel model(h.orchestrator);

    model.setDraft("add a clip");
    const ChatSendResult result = model.sendDraft();

    EXPECT_EQ(result.status, ChatSendStatus::Applied);
    EXPECT_TRUE(model.draft().empty());
}

// ---------------------------------------------------------------------------
// 8.5 — an unauthorized send is surfaced and the unsent text is preserved
// ---------------------------------------------------------------------------

TEST(AgentChatViewModel, AuthRejectionPreservesPendingText) {
    Harness h;
    h.gate.verdict = err(makeError(ErrorCode::Unauthenticated, "please authenticate"));
    AgentChatViewModel model(h.orchestrator);

    const ChatSendResult result = model.sendMessage("add a clip");

    EXPECT_EQ(result.status, ChatSendStatus::AuthenticationRequired);
    EXPECT_EQ(result.notice, "please authenticate");
    // The unsent content is preserved for retry; nothing was executed.
    EXPECT_TRUE(model.hasPendingMessage());
    EXPECT_EQ(model.pendingMessage(), "add a clip");
    EXPECT_EQ(clipCount(h.engine), 0u);
}

TEST(AgentChatViewModel, SendDraftKeepsDraftOnAuthRejection) {
    Harness h;
    h.gate.verdict = err(makeError(ErrorCode::Unauthenticated, "please authenticate"));
    AgentChatViewModel model(h.orchestrator);

    model.setDraft("add a clip");
    const ChatSendResult result = model.sendDraft();

    EXPECT_EQ(result.status, ChatSendStatus::AuthenticationRequired);
    // 8.5 — the draft is left intact so the user can retry after authenticating.
    EXPECT_EQ(model.draft(), "add a clip");

    // After authenticating, the preserved draft sends successfully.
    h.gate.verdict = ok();
    const ChatSendResult retry = model.sendDraft();
    EXPECT_EQ(retry.status, ChatSendStatus::Applied);
    EXPECT_FALSE(model.hasPendingMessage());
    EXPECT_EQ(clipCount(h.engine), 1u);
}

// ---------------------------------------------------------------------------
// 8.2 / 8.3 / 8.4 — @-mention affordances
// ---------------------------------------------------------------------------

TEST(AgentChatViewModel, PreviewMentionsResolvesUniqueMention) {
    Harness h;
    AgentChatViewModel model(h.orchestrator);
    model.setMentionSource(makeMentionLibrary);

    const MentionAffordance a = model.previewMentions("read @solo");
    EXPECT_TRUE(a.isResolved());
    ASSERT_EQ(a.resolved.size(), 1u);
    EXPECT_EQ(a.resolved.front().mention, "solo");
}

TEST(AgentChatViewModel, PreviewMentionsReportsAmbiguousCandidates) {
    Harness h;
    AgentChatViewModel model(h.orchestrator);
    model.setMentionSource(makeMentionLibrary);

    const MentionAffordance a = model.previewMentions("read @clip");
    EXPECT_EQ(a.status, services::MentionStatus::Ambiguous);
    EXPECT_EQ(a.problemMention, "clip");
    EXPECT_EQ(a.candidates.size(), 2u);  // 8.4 — two candidates to choose from
}

TEST(AgentChatViewModel, PreviewMentionsReportsUnmatched) {
    Harness h;
    AgentChatViewModel model(h.orchestrator);
    model.setMentionSource(makeMentionLibrary);

    const MentionAffordance a = model.previewMentions("read @ghost");
    EXPECT_EQ(a.status, services::MentionStatus::Unmatched);
    EXPECT_EQ(a.problemMention, "ghost");
}

TEST(AgentChatViewModel, SendAmbiguousMentionReturnsCandidatesAndDoesNotSubmit) {
    Harness h;
    AgentChatViewModel model(h.orchestrator);
    model.setMentionSource(makeMentionLibrary);

    const ChatSendResult result = model.sendMessage("read @clip");

    // 8.4 — surfaced as ambiguous with the candidate list; nothing executed.
    EXPECT_EQ(result.status, ChatSendStatus::MentionAmbiguous);
    EXPECT_EQ(result.problemMention, "clip");
    EXPECT_EQ(result.candidates.size(), 2u);
    EXPECT_EQ(clipCount(h.engine), 0u);
}

TEST(AgentChatViewModel, SendUnmatchedMentionRejectsWithoutSubmitting) {
    Harness h;
    AgentChatViewModel model(h.orchestrator);
    model.setMentionSource(makeMentionLibrary);

    const ChatSendResult result = model.sendMessage("read @ghost");

    // 8.3 — rejected as not-found; the message is not submitted for processing.
    EXPECT_EQ(result.status, ChatSendStatus::MentionNotFound);
    EXPECT_EQ(result.problemMention, "ghost");
    EXPECT_EQ(clipCount(h.engine), 0u);
}

TEST(AgentChatViewModel, SendResolvedMentionAppliesThroughSharedTools) {
    Harness h;
    AgentChatViewModel model(h.orchestrator);
    model.setMentionSource(makeMentionLibrary);

    // 8.2 — a uniquely-resolved mention is rewritten and the message is submitted
    // through the shared tool path (the read tool here).
    const ChatSendResult result = model.sendMessage("read @solo");
    EXPECT_EQ(result.status, ChatSendStatus::Applied);
    ASSERT_TRUE(result.toolName.has_value());
    EXPECT_EQ(*result.toolName, "timeline.read");
}

// ---------------------------------------------------------------------------
// 10.4 — non-blocking GPU-unavailable notification
// ---------------------------------------------------------------------------

TEST(AgentChatViewModel, GpuNoticeAbsentWhenAccelerated) {
    Harness h;
    AgentChatViewModel model(h.orchestrator);

    model.setGpuAccelerationStatus(makeGpuAccelerationStatus(FakeGpuContext{false, std::nullopt}));
    EXPECT_TRUE(model.gpuAccelerationAvailable());
    EXPECT_FALSE(model.gpuUnavailableNotice().has_value());
}

TEST(AgentChatViewModel, GpuNoticePresentOnSoftwareFallback) {
    Harness h;
    AgentChatViewModel model(h.orchestrator);

    model.setGpuAccelerationStatus(
        makeGpuAccelerationStatus(FakeGpuContext{true, std::string("no compatible GPU found")}));

    EXPECT_FALSE(model.gpuAccelerationAvailable());
    ASSERT_TRUE(model.gpuUnavailableNotice().has_value());
    EXPECT_EQ(*model.gpuUnavailableNotice(), "no compatible GPU found");

    // The notice is non-blocking: chat still works with acceleration unavailable.
    EXPECT_EQ(model.sendMessage("add a clip").status, ChatSendStatus::Applied);
    EXPECT_EQ(clipCount(h.engine), 1u);
}

TEST(AgentChatViewModel, GpuNoticeUsesDefaultMessageWhenContextHasNone) {
    Harness h;
    AgentChatViewModel model(h.orchestrator);

    model.setGpuAccelerationStatus(makeGpuAccelerationStatus(FakeGpuContext{true, std::nullopt}));
    ASSERT_TRUE(model.gpuUnavailableNotice().has_value());
    EXPECT_FALSE(model.gpuUnavailableNotice()->empty());
}

// ---------------------------------------------------------------------------
// 13.4 — generative-service-unavailable error leaves the editor functional
// ---------------------------------------------------------------------------

TEST(AgentChatViewModel, GenerativeServiceUnavailableKeepsEditorFunctional) {
    Harness h;
    AgentChatViewModel model(h.orchestrator);

    EXPECT_TRUE(model.generativeServiceAvailable());
    EXPECT_TRUE(model.editorRemainsFunctional());

    model.reportGenerativeServiceUnavailable(
        "The Generative AI service is currently unreachable.");

    // The error is surfaced...
    EXPECT_FALSE(model.generativeServiceAvailable());
    ASSERT_TRUE(model.generativeServiceError().has_value());
    EXPECT_EQ(*model.generativeServiceError(),
              "The Generative AI service is currently unreachable.");

    // ...but the open-source editor stays fully functional: an edit still applies
    // through the shared tools and the in-progress project state is preserved.
    EXPECT_TRUE(model.editorRemainsFunctional());
    EXPECT_EQ(model.sendMessage("add a clip").status, ChatSendStatus::Applied);
    EXPECT_EQ(clipCount(h.engine), 1u);
}

TEST(AgentChatViewModel, GenerativeServiceErrorCanBeCleared) {
    Harness h;
    AgentChatViewModel model(h.orchestrator);

    model.reportGenerativeServiceUnavailable("unreachable");
    ASSERT_FALSE(model.generativeServiceAvailable());

    model.clearGenerativeServiceError();
    EXPECT_TRUE(model.generativeServiceAvailable());
    EXPECT_FALSE(model.generativeServiceError().has_value());
}

}  // namespace
}  // namespace palmier::ui
