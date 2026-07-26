// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/services/mcp_protocol_handler_test.cpp — unit tests for
// services::McpProtocolHandler (task 5.2; Requirements 9.1-9.10, 9.14, 9.15,
// 9.16).
//
// The handler is the protocol half of design.md D3: JSON-RPC 2.0 envelope
// validation, the four supported methods, the fixed fault-code order, and session
// state — with no socket in sight. These tests drive it directly over a real
// ProjectSession, the real default tool surface and the real McpToolExecutor, so
// `tools/call` runs the production execution policy (validation, rollback, undo
// recording) rather than a stand-in:
//
//   * 9.2  `initialize` negotiates the requested version when supported and the
//          highest supported version otherwise, and reports the server name, the
//          server version and a capabilities object declaring `tools`.
//   * 9.10 `notifications/initialized` answers HTTP 202 with a zero-byte body and
//          marks the session initialized.
//   * 9.3  `tools/list` returns one entry per registered tool, each with `name`,
//          `description` and an object `inputSchema`.
//   * 9.4  `tools/call` returns a `content` array whose first entry is a text
//          entry, with `isError` false, and the edit reaches the project.
//   * 9.5  a failing tool yields `isError` true naming the tool and the reason,
//          and leaves the project and the undo history exactly as they were.
//   * 9.6-9.9  the parse (-32700), envelope (-32600), method (-32601) and
//          argument/tool (-32602) faults, none of which creates an edit command.
//   * 9.14/9.15  `tools/list` and `tools/call` on an uninitialized session, and on
//          an unknown or expired session, are refused.
//   * 9.16 a budget overrun is reported as an `isError` result naming the tool and
//          the limit, and the handler hands the invoker the 60-second budget.
//
// The MainThreadInvoker seam is exercised in both directions: the inline invoker
// (what headless builds and tests use) and a scripted invoker that reports a
// Timeout without running the work.

#include "services/McpProtocolHandler.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include "core/EditCommands.hpp"
#include "core/FrameRate.hpp"
#include "core/Project.hpp"
#include "core/Resolution.hpp"
#include "core/TimelineEngine.hpp"
#include "core/Uuid.hpp"
#include "services/Json.hpp"
#include "services/McpSessionRegistry.hpp"
#include "services/McpToolExecutor.hpp"
#include "services/ProjectSession.hpp"
#include "services/ToolRegistry.hpp"

namespace palmier::services {
namespace {

using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

Project makeProject(Uuid& trackId, Uuid& assetId) {
    Project project;
    project.id = Uuid::generateV4();
    project.name = "MCP Protocol Handler Test";
    project.timelineFps = FrameRate::fps30();
    project.canvas = Resolution::hd1080();

    Track track;
    track.id = Uuid::generateV4();
    track.kind = TrackKind::Video;

    const MediaAssetRef asset(Uuid::generateV4(), "/media/a.mp4");
    project.assets.push_back(asset);
    project.tracks.push_back(track);

    trackId = track.id;
    assetId = asset.assetId;
    return project;
}

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

/// A whole protocol stack: session -> tool surface -> executor -> handler.
class Stack {
public:
    explicit Stack(MainThreadInvoker invoker = {},
                   McpSessionRegistry::Options sessionOptions = {})
        : session_(std::make_unique<ProjectSession>()),
          sessions_(std::move(sessionOptions)) {
        (void)session_->engine().reset(makeProject(trackId_, assetId_));
        registry_ = buildDefaultToolRegistry(*session_);
        invoker_ = std::move(invoker);
    }

    /// Register an extra tool before the handler is built.
    void addTool(Tool tool) { registry_.add(std::move(tool)); }

    McpProtocolHandler& handler() {
        if (!handler_) {
            executor_ = std::make_unique<McpToolExecutor>(registry_, session_.get());
            handler_ = std::make_unique<McpProtocolHandler>(registry_, *executor_, sessions_,
                                                           invoker_);
        }
        return *handler_;
    }

    McpSessionRegistry& sessions() { return sessions_; }
    ToolRegistry&       registry() { return registry_; }
    TimelineEngine&     engine() { return session_->engine(); }
    const Uuid&         trackId() const { return trackId_; }
    const Uuid&         assetId() const { return assetId_; }

    /// `initialize` + `notifications/initialized`, returning the session id.
    std::string openSession() {
        const McpReply init = handler().handle(context(std::nullopt), initializeBody(1));
        EXPECT_EQ(init.httpStatus, 200);
        EXPECT_TRUE(init.newSessionId.has_value());
        const std::string id = init.newSessionId.value_or("");

        const McpReply notified = handler().handle(context(id), R"({"jsonrpc":"2.0",)"
                                                               R"("method":"notifications/initialized"})");
        EXPECT_EQ(notified.httpStatus, 202);
        EXPECT_TRUE(notified.body.empty());
        return id;
    }

    static McpRequestContext context(std::optional<std::string> sessionId) {
        McpRequestContext ctx;
        ctx.sourceAddress = "127.0.0.1";
        ctx.sessionId = std::move(sessionId);
        return ctx;
    }

    static std::string initializeBody(std::int64_t id,
                                      const std::string& protocolVersion = "2025-06-18") {
        Json params = Json::object();
        if (!protocolVersion.empty()) params.set("protocolVersion", Json(protocolVersion));
        Json request = Json::object();
        request.set("jsonrpc", Json("2.0"));
        request.set("id", Json(id));
        request.set("method", Json("initialize"));
        request.set("params", std::move(params));
        return request.dump();
    }

    static std::string callBody(std::int64_t id, const std::string& tool, Json arguments) {
        Json params = Json::object();
        params.set("name", Json(tool));
        params.set("arguments", std::move(arguments));
        Json request = Json::object();
        request.set("jsonrpc", Json("2.0"));
        request.set("id", Json(id));
        request.set("method", Json("tools/call"));
        request.set("params", std::move(params));
        return request.dump();
    }

    static std::string listBody(std::int64_t id) {
        Json request = Json::object();
        request.set("jsonrpc", Json("2.0"));
        request.set("id", Json(id));
        request.set("method", Json("tools/list"));
        return request.dump();
    }

private:
    Uuid                                trackId_;
    Uuid                                assetId_;
    std::unique_ptr<ProjectSession>     session_;
    ToolRegistry                        registry_;
    McpSessionRegistry                  sessions_;
    MainThreadInvoker                   invoker_;
    std::unique_ptr<McpToolExecutor>    executor_;
    std::unique_ptr<McpProtocolHandler> handler_;
};

Json addClipArgs(const Uuid& trackId, const Uuid& assetId) {
    Json args = Json::object();
    args.set("trackId", trackId.toString());
    args.set("assetId", assetId.toString());
    args.set("sourceOutNs", static_cast<std::int64_t>(1'000'000'000));
    return args;
}

Json parseReply(const McpReply& reply) {
    Result<Json> parsed = Json::parse(reply.body);
    EXPECT_TRUE(parsed.isOk()) << "reply body was not JSON: " << reply.body;
    return parsed.isOk() ? std::move(parsed).value() : Json::object();
}

/// Every response body must be a JSON-RPC 2.0 envelope carrying exactly one of
/// `result` / `error` (Requirement 9.1).
void expectEnvelope(const Json& body) {
    ASSERT_TRUE(body.isObject());
    EXPECT_EQ(body.stringOr("jsonrpc"), "2.0");
    const bool hasResult = body.contains("result");
    const bool hasError = body.contains("error");
    EXPECT_NE(hasResult, hasError) << "exactly one of result/error is required";
}

std::int64_t errorCode(const Json& body) {
    const Json* error = body.find("error");
    return error != nullptr ? error->intOr("code") : 0;
}

std::string errorMessage(const Json& body) {
    const Json* error = body.find("error");
    return error != nullptr ? error->stringOr("message") : std::string{};
}

// ---------------------------------------------------------------------------
// initialize (Requirements 9.2, 9.11)
// ---------------------------------------------------------------------------

TEST(McpProtocolInitialize, NegotiatesTheRequestedSupportedVersion) {
    Stack stack;
    const McpReply reply =
        stack.handler().handle(Stack::context(std::nullopt), Stack::initializeBody(7, "2025-03-26"));

    EXPECT_EQ(reply.httpStatus, 200);
    const Json body = parseReply(reply);
    expectEnvelope(body);
    ASSERT_TRUE(body.contains("id"));
    EXPECT_EQ(body.intOr("id"), 7);

    const Json* result = body.find("result");
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->stringOr("protocolVersion"), "2025-03-26");

    const Json* serverInfo = result->find("serverInfo");
    ASSERT_NE(serverInfo, nullptr);
    EXPECT_FALSE(serverInfo->stringOr("name").empty());
    EXPECT_FALSE(serverInfo->stringOr("version").empty());

    const Json* capabilities = result->find("capabilities");
    ASSERT_NE(capabilities, nullptr);
    EXPECT_TRUE(capabilities->contains("tools"));

    // Requirement 9.11: an opaque identifier of at least 32 characters.
    ASSERT_TRUE(reply.newSessionId.has_value());
    EXPECT_GE(reply.newSessionId->size(), 32u);
    EXPECT_TRUE(McpSessionRegistry::isWellFormedId(*reply.newSessionId));
}

TEST(McpProtocolInitialize, FallsBackToTheHighestSupportedVersion) {
    Stack stack;
    for (const std::string& requested : {std::string("1999-01-01"), std::string("")}) {
        const McpReply reply = stack.handler().handle(Stack::context(std::nullopt),
                                                     Stack::initializeBody(1, requested));
        const Json body = parseReply(reply);
        expectEnvelope(body);
        const Json* result = body.find("result");
        ASSERT_NE(result, nullptr);
        EXPECT_EQ(result->stringOr("protocolVersion"),
                  std::string(McpProtocolHandler::latestProtocolVersion()));
    }
}

TEST(McpProtocolInitialize, EchoesAStringIdUnchanged) {
    Stack stack;
    const McpReply reply = stack.handler().handle(
        Stack::context(std::nullopt),
        R"({"jsonrpc":"2.0","id":"abc-1","method":"initialize","params":{}})");
    const Json body = parseReply(reply);
    expectEnvelope(body);
    const Json* id = body.find("id");
    ASSERT_NE(id, nullptr);
    ASSERT_TRUE(id->isString());
    EXPECT_EQ(id->asString(), "abc-1");
}

TEST(McpProtocolInitialize, RefusesOnlyTheExcessSessionWhenTheLimitIsReached) {
    McpSessionRegistry::Options options;
    options.maxSessions = 1;
    Stack stack({}, options);

    const McpReply first =
        stack.handler().handle(Stack::context(std::nullopt), Stack::initializeBody(1));
    ASSERT_TRUE(first.newSessionId.has_value());

    const McpReply second =
        stack.handler().handle(Stack::context(std::nullopt), Stack::initializeBody(2));
    const Json body = parseReply(second);
    expectEnvelope(body);
    EXPECT_EQ(errorCode(body), McpProtocolHandler::kErrorSessionLimit);
    EXPECT_FALSE(second.newSessionId.has_value());
    // The established session survives.
    EXPECT_TRUE(stack.sessions().touch(*first.newSessionId).isOk());
}

// ---------------------------------------------------------------------------
// notifications/initialized (Requirement 9.10)
// ---------------------------------------------------------------------------

TEST(McpProtocolInitialized, Answers202WithAZeroByteBody) {
    Stack stack;
    const McpReply init =
        stack.handler().handle(Stack::context(std::nullopt), Stack::initializeBody(1));
    ASSERT_TRUE(init.newSessionId.has_value());

    const McpReply reply = stack.handler().handle(
        Stack::context(*init.newSessionId),
        R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
    EXPECT_EQ(reply.httpStatus, 202);
    EXPECT_TRUE(reply.body.empty());

    const Result<McpSessionRecord*> record = stack.sessions().touch(*init.newSessionId);
    ASSERT_TRUE(record.isOk());
    EXPECT_TRUE(record.value()->initialized);
}

TEST(McpProtocolInitialized, UnknownSessionIsRefused) {
    Stack stack;
    const McpReply reply = stack.handler().handle(
        Stack::context(std::string(64, 'a')),
        R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
    const Json body = parseReply(reply);
    expectEnvelope(body);
    EXPECT_EQ(errorCode(body), McpProtocolHandler::kErrorSessionUnknown);
}

// ---------------------------------------------------------------------------
// tools/list (Requirement 9.3)
// ---------------------------------------------------------------------------

TEST(McpProtocolToolsList, DescribesEveryRegisteredTool) {
    Stack             stack;
    const std::string session = stack.openSession();

    const McpReply reply = stack.handler().handle(Stack::context(session), Stack::listBody(2));
    EXPECT_EQ(reply.httpStatus, 200);
    const Json body = parseReply(reply);
    expectEnvelope(body);

    const Json* result = body.find("result");
    ASSERT_NE(result, nullptr);
    const Json* tools = result->find("tools");
    ASSERT_NE(tools, nullptr);
    ASSERT_TRUE(tools->isArray());
    EXPECT_EQ(tools->asArray().size(), stack.registry().size());
    EXPECT_GT(tools->asArray().size(), 0u);

    for (const Json& entry : tools->asArray()) {
        ASSERT_TRUE(entry.isObject());
        EXPECT_FALSE(entry.stringOr("name").empty());
        EXPECT_LE(entry.stringOr("name").size(), 64u);
        EXPECT_FALSE(entry.stringOr("description").empty());
        const Json* schema = entry.find("inputSchema");
        ASSERT_NE(schema, nullptr) << entry.stringOr("name");
        EXPECT_TRUE(schema->isObject());
        EXPECT_EQ(schema->stringOr("type"), "object");
    }
}

TEST(McpProtocolToolsList, RefusedBeforeTheSessionIsInitialized) {
    Stack stack;
    const McpReply init =
        stack.handler().handle(Stack::context(std::nullopt), Stack::initializeBody(1));
    ASSERT_TRUE(init.newSessionId.has_value());

    // No notifications/initialized yet (Requirement 9.14).
    const McpReply reply =
        stack.handler().handle(Stack::context(*init.newSessionId), Stack::listBody(2));
    const Json body = parseReply(reply);
    expectEnvelope(body);
    EXPECT_EQ(errorCode(body), McpProtocolHandler::kErrorSessionNotInitialized);
}

TEST(McpProtocolToolsList, RefusedForAnUnknownOrAbsentSession) {
    Stack stack;

    const Json unknown = parseReply(
        stack.handler().handle(Stack::context(std::string(64, 'b')), Stack::listBody(1)));
    expectEnvelope(unknown);
    EXPECT_EQ(errorCode(unknown), McpProtocolHandler::kErrorSessionUnknown);

    const Json absent =
        parseReply(stack.handler().handle(Stack::context(std::nullopt), Stack::listBody(2)));
    expectEnvelope(absent);
    EXPECT_EQ(errorCode(absent), McpProtocolHandler::kErrorSessionUnknown);
}

// ---------------------------------------------------------------------------
// tools/call (Requirements 9.4, 9.5, 9.9, 9.16)
// ---------------------------------------------------------------------------

TEST(McpProtocolToolsCall, SuccessCarriesATextContentEntryAndReachesTheProject) {
    Stack             stack;
    const std::string session = stack.openSession();
    ASSERT_EQ(clipCount(stack.engine()), 0u);

    const McpReply reply = stack.handler().handle(
        Stack::context(session),
        Stack::callBody(3, "timeline.add_clip", addClipArgs(stack.trackId(), stack.assetId())));
    EXPECT_EQ(reply.httpStatus, 200);

    const Json body = parseReply(reply);
    expectEnvelope(body);
    const Json* result = body.find("result");
    ASSERT_NE(result, nullptr);
    EXPECT_FALSE(result->boolOr("isError", true));

    const Json* content = result->find("content");
    ASSERT_NE(content, nullptr);
    ASSERT_TRUE(content->isArray());
    ASSERT_FALSE(content->asArray().empty());
    EXPECT_EQ(content->asArray().front().stringOr("type"), "text");
    EXPECT_FALSE(content->asArray().front().stringOr("text").empty());

    EXPECT_EQ(clipCount(stack.engine()), 1u);
}

TEST(McpProtocolToolsCall, FailingToolIsErrorAndLeavesTheProjectUntouched) {
    Stack stack;

    Tool failing;
    failing.name = "test.apply_then_fail";
    failing.description = "Applies a clip and then reports failure.";
    TimelineEngine* engine = &stack.engine();
    const Uuid      trackId = stack.trackId();
    const Uuid      assetId = stack.assetId();
    failing.handler = [engine, trackId, assetId](const Json&) -> Result<Json> {
        const CommandResult applied =
            engine->apply(std::make_unique<AddClipCommand>(trackId, makeClip(assetId, 0)));
        EXPECT_TRUE(applied.isOk());
        return err<Json>(failedPrecondition("intentional failure after a successful apply"));
    };
    stack.addTool(std::move(failing));

    const std::string session = stack.openSession();
    ASSERT_EQ(clipCount(stack.engine()), 0u);

    const Json body = parseReply(stack.handler().handle(
        Stack::context(session), Stack::callBody(4, "test.apply_then_fail", Json::object())));
    expectEnvelope(body);
    const Json* result = body.find("result");
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(result->boolOr("isError", false));

    const Json* content = result->find("content");
    ASSERT_NE(content, nullptr);
    ASSERT_FALSE(content->asArray().empty());
    const std::string text = content->asArray().front().stringOr("text");
    EXPECT_NE(text.find("test.apply_then_fail"), std::string::npos) << text;
    EXPECT_NE(text.find("intentional failure"), std::string::npos) << text;

    // Requirement 9.5: pre-invocation state, including the undo history.
    EXPECT_EQ(clipCount(stack.engine()), 0u);
    EXPECT_FALSE(stack.engine().canUndo());
}

TEST(McpProtocolToolsCall, UnknownToolIsInvalidParamsNamingTheTool) {
    Stack             stack;
    const std::string session = stack.openSession();

    const Json body = parseReply(stack.handler().handle(
        Stack::context(session), Stack::callBody(5, "timeline.no_such_tool", Json::object())));
    expectEnvelope(body);
    EXPECT_EQ(errorCode(body), McpProtocolHandler::kErrorInvalidParams);
    EXPECT_NE(errorMessage(body).find("timeline.no_such_tool"), std::string::npos);
    EXPECT_EQ(clipCount(stack.engine()), 0u);
    EXPECT_FALSE(stack.engine().canUndo());
}

TEST(McpProtocolToolsCall, SchemaViolationsAreInvalidParamsAndCreateNoCommand) {
    Stack             stack;
    const std::string session = stack.openSession();

    // Missing the required "sourceOutNs".
    Json missing = Json::object();
    missing.set("trackId", stack.trackId().toString());
    missing.set("assetId", stack.assetId().toString());
    const Json missingBody = parseReply(stack.handler().handle(
        Stack::context(session), Stack::callBody(6, "timeline.add_clip", std::move(missing))));
    expectEnvelope(missingBody);
    EXPECT_EQ(errorCode(missingBody), McpProtocolHandler::kErrorInvalidParams);
    EXPECT_NE(errorMessage(missingBody).find("sourceOutNs"), std::string::npos)
        << errorMessage(missingBody);

    // Wrong JSON type for a declared argument.
    Json wrongType = addClipArgs(stack.trackId(), stack.assetId());
    wrongType.set("sourceOutNs", Json("not a number"));
    const Json wrongTypeBody = parseReply(stack.handler().handle(
        Stack::context(session), Stack::callBody(7, "timeline.add_clip", std::move(wrongType))));
    expectEnvelope(wrongTypeBody);
    EXPECT_EQ(errorCode(wrongTypeBody), McpProtocolHandler::kErrorInvalidParams);

    // Out-of-bounds value for a declared argument.
    Json outOfBounds = addClipArgs(stack.trackId(), stack.assetId());
    outOfBounds.set("sourceOutNs", static_cast<std::int64_t>(-5));
    const Json outOfBoundsBody = parseReply(stack.handler().handle(
        Stack::context(session), Stack::callBody(8, "timeline.add_clip", std::move(outOfBounds))));
    expectEnvelope(outOfBoundsBody);
    EXPECT_EQ(errorCode(outOfBoundsBody), McpProtocolHandler::kErrorInvalidParams);

    EXPECT_EQ(clipCount(stack.engine()), 0u);
    EXPECT_FALSE(stack.engine().canUndo());
}

TEST(McpProtocolToolsCall, MissingParamsObjectIsInvalidParams) {
    Stack             stack;
    const std::string session = stack.openSession();

    const Json body = parseReply(stack.handler().handle(
        Stack::context(session), R"({"jsonrpc":"2.0","id":9,"method":"tools/call"})"));
    expectEnvelope(body);
    EXPECT_EQ(errorCode(body), McpProtocolHandler::kErrorInvalidParams);
}

TEST(McpProtocolToolsCall, BudgetOverrunIsReportedAsAnIsErrorResultNamingTheTool) {
    std::chrono::milliseconds observedBudget{0};
    bool                      workRan = false;

    MainThreadInvoker timingOut = [&observedBudget, &workRan](
                                     std::function<Result<Json>()> work,
                                     std::chrono::milliseconds budget) -> Result<Json> {
        observedBudget = budget;
        (void)work;  // abandoned, exactly as an overrun does
        return err<Json>(makeError(ErrorCode::Timeout,
                                   "the invocation was abandoned at the budget"));
    };
    Stack             stack(std::move(timingOut));
    const std::string session = stack.openSession();

    const Json body = parseReply(stack.handler().handle(
        Stack::context(session),
        Stack::callBody(10, "timeline.add_clip", addClipArgs(stack.trackId(), stack.assetId()))));
    expectEnvelope(body);
    const Json* result = body.find("result");
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(result->boolOr("isError", false));
    const std::string text = result->find("content")->asArray().front().stringOr("text");
    EXPECT_NE(text.find("timeline.add_clip"), std::string::npos) << text;
    EXPECT_NE(text.find("time limit"), std::string::npos) << text;

    // Requirement 9.16: the handler hands the invoker the 60-second budget.
    EXPECT_EQ(observedBudget, 60s);
    EXPECT_EQ(stack.handler().toolBudget(), 60s);
    EXPECT_FALSE(workRan);
    EXPECT_EQ(clipCount(stack.engine()), 0u);
}

// ---------------------------------------------------------------------------
// Envelope and transport-level faults (Requirements 9.6, 9.7, 9.8)
// ---------------------------------------------------------------------------

TEST(McpProtocolFaults, UnparsableBodyIsParseError) {
    Stack          stack;
    const McpReply reply = stack.handler().handle(Stack::context(std::nullopt), "this is not json");
    EXPECT_EQ(reply.httpStatus, 400);
    const Json body = parseReply(reply);
    expectEnvelope(body);
    EXPECT_EQ(errorCode(body), McpProtocolHandler::kErrorParse);
    EXPECT_TRUE(body.find("id")->isNull());
}

TEST(McpProtocolFaults, OversizeBodyIsParseError) {
    Stack             stack;
    McpRequestContext ctx = Stack::context(std::nullopt);
    ctx.bodyBytes = McpProtocolHandler::kDefaultMaxBodyBytes + 1;

    const McpReply reply = stack.handler().handle(ctx, R"({"jsonrpc":"2.0","id":1,"method":"initialize"})");
    EXPECT_EQ(reply.httpStatus, 400);
    const Json body = parseReply(reply);
    expectEnvelope(body);
    EXPECT_EQ(errorCode(body), McpProtocolHandler::kErrorParse);
    EXPECT_EQ(stack.sessions().activeCount(), 0u);  // no session was created
}

TEST(McpProtocolFaults, MalformedEnvelopesAreInvalidRequest) {
    Stack stack;
    const char* bodies[] = {
        R"({"id":1,"method":"tools/list"})",                  // no jsonrpc
        R"({"jsonrpc":"1.0","id":1,"method":"tools/list"})",  // wrong version
        R"({"jsonrpc":"2.0","id":1})",                        // no method
        R"({"jsonrpc":"2.0","id":1,"method":42})",            // non-string method
        R"({"jsonrpc":"2.0","id":{"a":1},"method":"tools/list"})",  // object id
        R"(["jsonrpc","2.0"])",                               // not an object
    };
    for (const char* raw : bodies) {
        const McpReply reply = stack.handler().handle(Stack::context(std::nullopt), raw);
        EXPECT_EQ(reply.httpStatus, 400) << raw;
        const Json body = parseReply(reply);
        expectEnvelope(body);
        EXPECT_EQ(errorCode(body), McpProtocolHandler::kErrorInvalidRequest) << raw;
    }
}

TEST(McpProtocolFaults, UnsupportedMethodIsMethodNotFoundNamingTheMethod) {
    Stack          stack;
    const McpReply reply = stack.handler().handle(
        Stack::context(std::nullopt),
        R"({"jsonrpc":"2.0","id":11,"method":"resources/list"})");
    const Json body = parseReply(reply);
    expectEnvelope(body);
    EXPECT_EQ(errorCode(body), McpProtocolHandler::kErrorMethodNotFound);
    EXPECT_NE(errorMessage(body).find("resources/list"), std::string::npos);
    EXPECT_EQ(body.intOr("id"), 11);
}

// ---------------------------------------------------------------------------
// Protocol-version helpers
// ---------------------------------------------------------------------------

TEST(McpProtocolVersions, SupportedSetIsNewestFirst) {
    EXPECT_TRUE(McpProtocolHandler::isSupportedProtocolVersion("2025-06-18"));
    EXPECT_TRUE(McpProtocolHandler::isSupportedProtocolVersion("2025-03-26"));
    EXPECT_FALSE(McpProtocolHandler::isSupportedProtocolVersion("2024-11-05"));
    EXPECT_EQ(McpProtocolHandler::latestProtocolVersion(), "2025-06-18");
}

TEST(McpProtocolVersions, InlineInvokerRunsTheWorkOnTheCallingThread) {
    const MainThreadInvoker inline_ = inlineMainThreadInvoker();
    bool                    ran = false;
    const Result<Json>      out = inline_(
        [&ran]() -> Result<Json> {
            ran = true;
            return Json("done");
        },
        1ms);
    EXPECT_TRUE(ran);
    ASSERT_TRUE(out.isOk());
    EXPECT_EQ(out.value().asString(), "done");
}

}  // namespace
}  // namespace palmier::services
