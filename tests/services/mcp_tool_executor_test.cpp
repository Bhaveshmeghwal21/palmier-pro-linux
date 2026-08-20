// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/services/mcp_tool_executor_test.cpp — unit tests for the MCP tool
// execution policy (task 15.3; Requirements 7.4, 7.5, 7.6, 7.7, 7.10).
//
// These exercise McpToolExecutor over a real services::ProjectSession (task 3.4:
// the executor and the tool surface now act on the session that owns the current
// project, not on a bare TimelineEngine) and the shared ToolRegistry, covering
// every branch of the required policy:
//   * recognized tool executes on the current project and returns within budget
//     (7.4);
//   * an unknown tool name yields an unknown-tool error and leaves the project
//     unchanged (7.5);
//   * a tool that applies a command and then fails is rolled back to the
//     pre-invocation state (7.6);
//   * a tool that overruns the time budget is aborted and rolled back (7.7),
//     driven deterministically through an injected clock;
//   * a request with no project open returns the no-project error (7.10);
//   * tool inputs are validated against their JSON schema before any command is
//     created, so an invalid request never mutates the project (7.10).
//
// Task 3.4 adds the session-switch expectations: the default budget is the 60 s of
// Requirement 9.16, argument validation delegates to the tool's one ToolSchema
// declaration, a project that becomes current after the registry and executor
// were built is still visible to the tools (design.md D1), and the
// InvocationSource tag is recorded without changing any outcome (Requirements 1.7,
// 9.4, 11.5).
//
// Custom tools are registered alongside the default surface to simulate the
// failure/timeout paths deterministically without real waiting or flakiness.

#include "services/McpToolExecutor.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/EditCommands.hpp"
#include "core/FrameRate.hpp"
#include "core/Project.hpp"
#include "core/Resolution.hpp"
#include "core/TimelineEngine.hpp"
#include "core/Uuid.hpp"
#include "services/Json.hpp"
#include "services/ProjectSession.hpp"
#include "services/ToolRegistry.hpp"
#include "services/ToolSchema.hpp"

namespace palmier::services {
namespace {

using namespace std::chrono_literals;

// The tool surface and the executor now act on a ProjectSession rather than a
// bare TimelineEngine (task 3.4; design.md D1): the session owns one engine for
// its whole lifetime and swaps the project value inside it. Tests therefore build
// a session and seed it with their fixture project the way `project.open` will —
// through the engine's in-place reset — then keep using `session.engine()`.
void seedSession(ProjectSession& session, Project project) {
    (void)session.engine().reset(std::move(project));
}

// A project with one empty video track and one referenced asset.
Project makeProject(Uuid& trackId, Uuid& assetId) {
    Project project;
    project.id = Uuid::generateV4();
    project.name = "MCP Executor Test";
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

// A one-second clip referencing `assetId`, placed at `startNs` on the timeline.
Clip makeClip(const Uuid& assetId, std::int64_t startNs) {
    Clip clip;
    clip.id = Uuid::generateV4();
    clip.assetRef = MediaAssetRef(assetId, "/media/a.mp4");
    clip.timelineStart = Duration::fromNanoseconds(startNs);
    clip.sourceIn = Duration::fromNanoseconds(0);
    clip.sourceOut = Duration::fromNanoseconds(1'000'000'000);
    return clip;
}

std::size_t clipCount(const TimelineEngine& engine) {
    std::size_t total = 0;
    for (const Track& t : engine.snapshot().tracks) total += t.clips.size();
    return total;
}

Json addClipArgs(const Uuid& trackId, const Uuid& assetId) {
    Json args = Json::object();
    args.set("trackId", trackId.toString());
    args.set("assetId", assetId.toString());
    args.set("sourceOutNs", static_cast<std::int64_t>(1'000'000'000));
    return args;
}

// ---------------------------------------------------------------------------
// 7.4 — recognized tool executes on the current project and returns within budget
// ---------------------------------------------------------------------------

TEST(McpToolExecutor, RecognizedReadToolSucceeds) {
    Uuid trackId, assetId;
    ProjectSession session;
    seedSession(session, makeProject(trackId, assetId));
    ToolRegistry registry = buildDefaultToolRegistry(session);
    McpToolExecutor executor(registry, &session);

    Result<Json> result = executor.executeTool("timeline.read", Json::object());
    ASSERT_TRUE(result.isOk());
    EXPECT_TRUE(result.value().isObject());
    EXPECT_TRUE(result.value().contains("tracks"));
}

TEST(McpToolExecutor, RecognizedEditToolAppliesToProject) {
    Uuid trackId, assetId;
    ProjectSession session;
    seedSession(session, makeProject(trackId, assetId));
    TimelineEngine& engine = session.engine();
    ToolRegistry registry = buildDefaultToolRegistry(session);
    McpToolExecutor executor(registry, &session);

    ASSERT_EQ(clipCount(engine), 0u);
    Result<Json> result = executor.executeTool("timeline.add_clip", addClipArgs(trackId, assetId));
    ASSERT_TRUE(result.isOk());
    EXPECT_EQ(clipCount(engine), 1u);
}

// ---------------------------------------------------------------------------
// 7.5 — unknown tool name: error + project unchanged
// ---------------------------------------------------------------------------

TEST(McpToolExecutor, UnknownToolReturnsNotFoundAndLeavesProjectUnchanged) {
    Uuid trackId, assetId;
    ProjectSession session;
    seedSession(session, makeProject(trackId, assetId));
    TimelineEngine& engine = session.engine();
    // Seed one clip so we can confirm the project is untouched.
    ASSERT_TRUE(engine.apply(std::make_unique<AddClipCommand>(trackId, makeClip(assetId, 0))).isOk());
    const std::size_t before = clipCount(engine);

    ToolRegistry registry = buildDefaultToolRegistry(session);
    McpToolExecutor executor(registry, &session);

    Result<Json> result = executor.executeTool("timeline.no_such_tool", Json::object());
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::NotFound);
    EXPECT_EQ(clipCount(engine), before);
}

// ---------------------------------------------------------------------------
// 7.6 — execution failure: roll back to the pre-invocation state
// ---------------------------------------------------------------------------

TEST(McpToolExecutor, ExecutionFailureRollsBackAppliedCommand) {
    Uuid trackId, assetId;
    ProjectSession session;
    seedSession(session, makeProject(trackId, assetId));
    TimelineEngine& engine = session.engine();

    ToolRegistry registry = buildDefaultToolRegistry(session);
    // A tool that applies a real command successfully, then fails a later step.
    Tool failing;
    failing.name = "test.apply_then_fail";
    failing.description = "Applies a clip and then reports failure.";
    failing.handler = [&engine, trackId, assetId](const Json&) -> Result<Json> {
        const CommandResult applied =
            engine.apply(std::make_unique<AddClipCommand>(trackId, makeClip(assetId, 0)));
        EXPECT_TRUE(applied.isOk());
        return err<Json>(failedPrecondition("intentional failure after a successful apply"));
    };
    registry.add(std::move(failing));

    McpToolExecutor executor(registry, &session);
    ASSERT_EQ(clipCount(engine), 0u);

    Result<Json> result = executor.executeTool("test.apply_then_fail", Json::object());
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::FailedPrecondition);

    // The clip the tool applied must have been rolled back: no partial mutation,
    // and no undo residue left behind by the aborted invocation.
    EXPECT_EQ(clipCount(engine), 0u);
    EXPECT_FALSE(engine.canUndo());
}

TEST(McpToolExecutor, RollbackPreservesUndoHistoryFromBeforeInvocation) {
    Uuid trackId, assetId;
    ProjectSession session;
    seedSession(session, makeProject(trackId, assetId));
    TimelineEngine& engine = session.engine();

    ToolRegistry registry = buildDefaultToolRegistry(session);
    Tool failing;
    failing.name = "test.apply_then_fail";
    failing.handler = [&engine, trackId, assetId](const Json&) -> Result<Json> {
        (void)engine.apply(std::make_unique<AddClipCommand>(trackId, makeClip(assetId, 2'000'000'000)));
        return err<Json>(failedPrecondition("boom"));
    };
    registry.add(std::move(failing));

    McpToolExecutor executor(registry, &session);

    // A legitimate edit performed before the aborted invocation.
    ASSERT_TRUE(executor.executeTool("timeline.add_clip", addClipArgs(trackId, assetId)).isOk());
    ASSERT_EQ(clipCount(engine), 1u);
    ASSERT_TRUE(engine.canUndo());

    // The aborted tool rolls back its own clip but must not disturb the prior one.
    ASSERT_TRUE(executor.executeTool("test.apply_then_fail", Json::object()).isError());
    EXPECT_EQ(clipCount(engine), 1u);
    EXPECT_TRUE(engine.canUndo());  // the pre-invocation edit is still undoable
}

// ---------------------------------------------------------------------------
// 7.7 — timeout: abort + roll back to the pre-invocation state
// ---------------------------------------------------------------------------

TEST(McpToolExecutor, TimeoutAbortsAndRollsBack) {
    Uuid trackId, assetId;
    ProjectSession session;
    seedSession(session, makeProject(trackId, assetId));
    TimelineEngine& engine = session.engine();

    auto now = std::make_shared<std::chrono::steady_clock::time_point>(
        std::chrono::steady_clock::now());

    ToolRegistry registry = buildDefaultToolRegistry(session);
    // A tool that "takes" 40s (advancing the injected clock) and applies a clip.
    Tool slow;
    slow.name = "test.slow_ok";
    slow.handler = [&engine, trackId, assetId, now](const Json&) -> Result<Json> {
        *now += 40s;  // simulate a long-running operation
        (void)engine.apply(std::make_unique<AddClipCommand>(trackId, makeClip(assetId, 0)));
        return Json::object();  // the handler itself "succeeds"
    };
    registry.add(std::move(slow));

    McpToolExecutor::Options options;
    options.timeBudget = 30s;
    options.clock = [now] { return *now; };
    McpToolExecutor executor(registry, &session, options);

    Result<Json> result = executor.executeTool("test.slow_ok", Json::object());
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::Timeout);

    // Even though the handler applied a clip and returned success, overrunning the
    // budget aborts the operation and rolls the project back.
    EXPECT_EQ(clipCount(engine), 0u);
    EXPECT_FALSE(engine.canUndo());
}

TEST(McpToolExecutor, WithinBudgetSucceeds) {
    Uuid trackId, assetId;
    ProjectSession session;
    seedSession(session, makeProject(trackId, assetId));
    TimelineEngine& engine = session.engine();

    auto now = std::make_shared<std::chrono::steady_clock::time_point>(
        std::chrono::steady_clock::now());

    ToolRegistry registry = buildDefaultToolRegistry(session);
    Tool quick;
    quick.name = "test.quick_ok";
    quick.handler = [&engine, trackId, assetId, now](const Json&) -> Result<Json> {
        *now += 5s;  // well under the 30s budget
        (void)engine.apply(std::make_unique<AddClipCommand>(trackId, makeClip(assetId, 0)));
        return Json::object();
    };
    registry.add(std::move(quick));

    McpToolExecutor::Options options;
    options.timeBudget = 30s;
    options.clock = [now] { return *now; };
    McpToolExecutor executor(registry, &session, options);

    Result<Json> result = executor.executeTool("test.quick_ok", Json::object());
    ASSERT_TRUE(result.isOk());
    EXPECT_EQ(clipCount(engine), 1u);
}

// ---------------------------------------------------------------------------
// 7.10 — no project open: error indicating no project is open
// ---------------------------------------------------------------------------

TEST(McpToolExecutor, NoProjectOpenReturnsError) {
    // The registry must be built over some session; the executor is given a null
    // session to model "no project open", so the registry is never invoked.
    Uuid trackId, assetId;
    ProjectSession registrySession;
    seedSession(registrySession, makeProject(trackId, assetId));
    ToolRegistry registry = buildDefaultToolRegistry(registrySession);

    McpToolExecutor executor(registry, /*session=*/nullptr);
    EXPECT_FALSE(executor.hasProject());

    Result<Json> result = executor.executeTool("timeline.read", Json::object());
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::FailedPrecondition);
}

TEST(McpToolExecutor, UnknownToolTakesPrecedenceOverNoProject) {
    Uuid trackId, assetId;
    ProjectSession registrySession;
    seedSession(registrySession, makeProject(trackId, assetId));
    ToolRegistry registry = buildDefaultToolRegistry(registrySession);

    McpToolExecutor executor(registry, /*session=*/nullptr);
    Result<Json> result = executor.executeTool("totally.unknown", Json::object());
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::NotFound);
}

// ---------------------------------------------------------------------------
// 7.10 — validate inputs against the schema BEFORE creating commands
// ---------------------------------------------------------------------------

TEST(McpToolExecutor, MissingRequiredFieldRejectedBeforeCommandCreation) {
    Uuid trackId, assetId;
    ProjectSession session;
    seedSession(session, makeProject(trackId, assetId));
    TimelineEngine& engine = session.engine();

    ToolRegistry registry = buildDefaultToolRegistry(session);
    // A tool whose handler WOULD mutate the project; validation must run first.
    Tool needsX;
    needsX.name = "test.requires_x";
    // Declared once, as every tool does: one required string argument.
    needsX.schema.arg(
        ArgSpec{.name = "x", .kind = JsonKind::String, .required = true});
    needsX.handler = [&engine, trackId, assetId](const Json&) -> Result<Json> {
        (void)engine.apply(std::make_unique<AddClipCommand>(trackId, makeClip(assetId, 0)));
        return Json::object();
    };
    registry.add(std::move(needsX));

    McpToolExecutor executor(registry, &session);

    // Missing required "x" -> rejected, handler never runs, project untouched.
    Result<Json> missing = executor.executeTool("test.requires_x", Json::object());
    ASSERT_TRUE(missing.isError());
    EXPECT_EQ(missing.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(clipCount(engine), 0u);

    // Wrong type for "x" -> rejected before command creation.
    Json wrongType = Json::object();
    wrongType.set("x", static_cast<std::int64_t>(7));
    Result<Json> mismatch = executor.executeTool("test.requires_x", wrongType);
    ASSERT_TRUE(mismatch.isError());
    EXPECT_EQ(mismatch.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(clipCount(engine), 0u);

    // Valid "x" -> the handler runs and mutates the project.
    Json valid = Json::object();
    valid.set("x", "hello");
    Result<Json> good = executor.executeTool("test.requires_x", valid);
    ASSERT_TRUE(good.isOk());
    EXPECT_EQ(clipCount(engine), 1u);
}

// The executor's pre-execution validation is no longer a private JSON-Schema
// subset: it delegates to ToolSchema::validate, the same declaration each tool
// publishes through inputSchema() (task 3.4; design.md D3, Requirement 9.12).
TEST(McpToolExecutor, SchemaValidationHelperDelegatesToToolSchema) {
    ToolSchema schema;
    schema.arg(ArgSpec{.name = "name", .kind = JsonKind::String, .required = true})
        .arg(ArgSpec{.name = "age", .kind = JsonKind::Integer});

    // Non-object input for an object schema.
    EXPECT_TRUE(McpToolExecutor::validateAgainstSchema(Json(42), schema).isError());

    // Missing required "name".
    EXPECT_TRUE(McpToolExecutor::validateAgainstSchema(Json::object(), schema).isError());

    // Present but wrong type for "age".
    Json wrongAge = Json::object();
    wrongAge.set("name", "kiro");
    wrongAge.set("age", "not-a-number");
    EXPECT_TRUE(McpToolExecutor::validateAgainstSchema(wrongAge, schema).isError());

    // An integer argument now wants an exact integer payload: a double is
    // rejected, which is the rule ToolSchema publishes and enforces (task 3.1),
    // rather than the looser "integral double counts" rule the executor's private
    // validator used before it delegated.
    Json doublePayload = Json::object();
    doublePayload.set("name", "kiro");
    doublePayload.set("age", 5.0);
    EXPECT_TRUE(McpToolExecutor::validateAgainstSchema(doublePayload, schema).isError());

    // An undeclared member is rejected, because the published schema says
    // "additionalProperties": false (Requirement 9.9).
    Json extra = Json::object();
    extra.set("name", "kiro");
    extra.set("nickname", "k");
    EXPECT_TRUE(McpToolExecutor::validateAgainstSchema(extra, schema).isError());

    // Fully valid.
    Json ok2 = Json::object();
    ok2.set("name", "kiro");
    ok2.set("age", static_cast<std::int64_t>(5));
    EXPECT_TRUE(McpToolExecutor::validateAgainstSchema(ok2, schema).isOk());

    // Whatever the declaration says, both renderings say it: the helper agrees
    // with the schema the tool advertises, because they are one declaration.
    EXPECT_EQ(McpToolExecutor::validateAgainstSchema(ok2, schema).isOk(),
              schema.validate(ok2).isOk());
}

// ---------------------------------------------------------------------------
// execute(request) envelope for the HTTP transport (task 15.2)
// ---------------------------------------------------------------------------

TEST(McpToolExecutor, ExecuteEnvelopeWrapsSuccessAndError) {
    Uuid trackId, assetId;
    ProjectSession session;
    seedSession(session, makeProject(trackId, assetId));
    TimelineEngine& engine = session.engine();
    ToolRegistry registry = buildDefaultToolRegistry(session);
    McpToolExecutor executor(registry, &session);

    // Success envelope (JSON-RPC-style nesting under "params").
    Json request = Json::object();
    Json params = Json::object();
    params.set("name", "timeline.read");
    params.set("arguments", Json::object());
    request.set("params", std::move(params));

    Json ok = executor.execute(request);
    ASSERT_TRUE(ok.contains("ok"));
    EXPECT_TRUE(ok.find("ok")->asBool());
    ASSERT_TRUE(ok.contains("result"));
    EXPECT_TRUE(ok.find("result")->isObject());

    // Error envelope (top-level name, unknown tool).
    Json bad = Json::object();
    bad.set("name", "timeline.does_not_exist");
    Json badResult = executor.execute(bad);
    ASSERT_TRUE(badResult.contains("ok"));
    EXPECT_FALSE(badResult.find("ok")->asBool());
    ASSERT_TRUE(badResult.contains("error"));
    EXPECT_EQ(badResult.find("error")->find("code")->asString(),
              std::string(toStringView(ErrorCode::NotFound)));

    // Missing name -> InvalidArgument envelope.
    Json empty = executor.execute(Json::object());
    EXPECT_FALSE(empty.find("ok")->asBool());
    EXPECT_EQ(empty.find("error")->find("code")->asString(),
              std::string(toStringView(ErrorCode::InvalidArgument)));
}

// ---------------------------------------------------------------------------
// Task 3.4 — the session switch: budget, late project loads, source tagging
// ---------------------------------------------------------------------------

// Requirement 9.16: a tool that does not complete within 60 seconds is abandoned,
// so 60 seconds is the default budget.
TEST(McpToolExecutor, DefaultTimeBudgetIsSixtySeconds) {
    Uuid trackId, assetId;
    ProjectSession session;
    seedSession(session, makeProject(trackId, assetId));
    ToolRegistry registry = buildDefaultToolRegistry(session);
    McpToolExecutor executor(registry, &session);

    EXPECT_EQ(executor.timeBudget(), std::chrono::milliseconds(60s));
}

// design.md D1: because every handler and the executor resolve the engine through
// the session at invocation time, a project that becomes current AFTER the
// registry and the executor were built is visible to the tools — no rebinding.
TEST(McpToolExecutor, ProjectBecomingCurrentAfterRegistrationIsVisibleToTools) {
    Uuid firstTrack, firstAsset;
    ProjectSession session;
    seedSession(session, makeProject(firstTrack, firstAsset));

    ToolRegistry registry = buildDefaultToolRegistry(session);
    McpToolExecutor executor(registry, &session);

    // A different project becomes current the way project.open will make it so.
    Uuid secondTrack, secondAsset;
    seedSession(session, makeProject(secondTrack, secondAsset));

    // The read tool reports the NEW project's track...
    Result<Json> read = executor.executeTool("timeline.read", Json::object());
    ASSERT_TRUE(read.isOk());
    const Json* tracks = read.value().find("tracks");
    ASSERT_NE(tracks, nullptr);
    ASSERT_EQ(tracks->asArray().size(), 1u);
    EXPECT_EQ(tracks->asArray()[0].find("id")->asString(), secondTrack.toString());

    // ... and an edit tool applies to it, while the stale track is unknown.
    ASSERT_TRUE(
        executor.executeTool("timeline.add_clip", addClipArgs(secondTrack, secondAsset)).isOk());
    EXPECT_EQ(clipCount(session.engine()), 1u);
    EXPECT_TRUE(
        executor.executeTool("timeline.add_clip", addClipArgs(firstTrack, firstAsset)).isError());
    EXPECT_EQ(clipCount(session.engine()), 1u);
}

// The InvocationSource argument names the issuing surface for the invocation log
// and has no behavioural effect: the same call from the GUI, the MCP endpoint and
// the agent runs the identical policy (Requirements 1.7, 9.4, 11.5).
TEST(McpToolExecutor, InvocationSourceIsRecordedButChangesNothing) {
    Uuid trackId, assetId;
    ProjectSession session;
    seedSession(session, makeProject(trackId, assetId));
    ToolRegistry registry = buildDefaultToolRegistry(session);

    std::vector<std::string> log;
    McpToolExecutor::Options options;
    options.invocationLog = [&log](InvocationSource source, std::string_view tool,
                                   bool succeeded, std::chrono::milliseconds) {
        log.push_back(std::string(invocationSourceName(source)) + " " + std::string(tool) +
                      (succeeded ? " ok" : " err"));
    };
    McpToolExecutor executor(registry, &session, options);

    // Three non-overlapping clips, one per surface.
    const auto argsAt = [&](std::int64_t startNs) {
        Json args = addClipArgs(trackId, assetId);
        args.set("timelineStartNs", startNs);
        return args;
    };
    ASSERT_TRUE(executor.executeTool("timeline.add_clip", argsAt(0), InvocationSource::Gui)
                    .isOk());
    ASSERT_TRUE(executor
                    .executeTool("timeline.add_clip", argsAt(2'000'000'000),
                                 InvocationSource::Mcp)
                    .isOk());
    ASSERT_TRUE(executor
                    .executeTool("timeline.add_clip", argsAt(4'000'000'000),
                                 InvocationSource::Agent)
                    .isOk());
    // A rejection is recorded too, with the same source vocabulary.
    ASSERT_TRUE(
        executor.executeTool("timeline.no_such_tool", Json::object(), InvocationSource::Agent)
            .isError());

    ASSERT_EQ(log.size(), 4u);
    EXPECT_EQ(log[0], "gui timeline.add_clip ok");
    EXPECT_EQ(log[1], "mcp timeline.add_clip ok");
    EXPECT_EQ(log[2], "agent timeline.add_clip ok");
    EXPECT_EQ(log[3], "agent timeline.no_such_tool err");

    // Three identical invocations, three identical applied edits — the source did
    // not change validation, execution or undo recording.
    EXPECT_EQ(clipCount(session.engine()), 3u);
}

}  // namespace
}  // namespace palmier::services
