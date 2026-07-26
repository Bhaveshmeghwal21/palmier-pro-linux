// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/services/mcp_http_integration_test.cpp — end-to-end integration tests for
// MCP HTTP protocol conformance (task 15.5; Requirements 7.4, 7.5, 7.6, 7.10).
//
// Unlike the transport unit tests (task 15.2, mcp_server_test.cpp) and the
// execution-policy unit tests (task 15.3, mcp_tool_executor_test.cpp), which each
// exercise one half of Component 2 in isolation, these tests wire the two halves
// TOGETHER exactly as the composition root does and drive them over a real TCP
// loopback socket:
//
//     real TimelineEngine  ->  buildDefaultToolRegistry  ->  McpToolExecutor
//                                                                   |
//                                          McpServer(handler = executor.execute)
//                                                                   |
//                                     bind 127.0.0.1:<ephemeral>/mcp, accept()
//                                                                   |
//                                        real HTTP POST from a client socket
//
// so a genuine HTTP request travels the whole path: socket -> HTTP parse ->
// dispatch() routing -> JSON parse -> executor envelope -> ToolRegistry handler
// -> EditCommand on the TimelineEngine -> JSON response -> HTTP response ->
// socket. The project state is then observed directly on the engine.
//
// Coverage (driving edits over HTTP and asserting the resulting project state,
// rollback on failure, and error responses — per the task):
//   * 7.4  a recognized edit tool executes and the resulting project state
//          reflects the edit; a recognized read tool returns the project.
//   * 7.5  an unknown tool name returns an error envelope and leaves the current
//          project unchanged.
//   * 7.6  a tool whose execution fails rolls the project back to its
//          pre-invocation state and returns a failure error envelope.
//   * 7.10 a request while no project is open returns a "no project open" error;
//          a schema-invalid request returns a validation error and never mutates
//          the project.
//   * HTTP-level conformance: an unknown path is 404, a non-POST method on /mcp
//          is 405, and a malformed JSON body is a 400 JSON-RPC parse error.
//
// The endpoint is bound on an ephemeral loopback port (bind port 0) so the tests
// are deterministic and need no fixed free port. The whole stack uses only POSIX
// sockets + the standard library, so it builds without Qt/FFmpeg/Vulkan/libsecret
// and links only Palmier::core.

#include "services/McpProtocolHandler.hpp"
#include "services/McpServer.hpp"
#include "services/McpSessionRegistry.hpp"
#include "services/McpToolExecutor.hpp"
#include "services/ProjectSession.hpp"
#include "services/ToolRegistry.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cstdint>
#include <memory>
#include <optional>
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

namespace palmier::services {
namespace {

// ---------------------------------------------------------------------------
// Project / clip fixtures (mirrors mcp_tool_executor_test.cpp so the observable
// project state lines up with the shared tool surface).
// ---------------------------------------------------------------------------

// A project with one empty video track and one referenced asset.
Project makeProject(Uuid& trackId, Uuid& assetId) {
    Project project;
    project.id = Uuid::generateV4();
    project.name = "MCP HTTP Integration Test";
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

// Build an MCP tool-call envelope body: {"name": <tool>, "arguments": <args>}.
std::string envelope(const std::string& tool, Json arguments) {
    Json req = Json::object();
    req.set("name", tool);
    req.set("arguments", std::move(arguments));
    return req.dump();
}

Json addClipArgs(const Uuid& trackId, const Uuid& assetId) {
    Json args = Json::object();
    args.set("trackId", trackId.toString());
    args.set("assetId", assetId.toString());
    args.set("sourceOutNs", static_cast<std::int64_t>(1'000'000'000));
    return args;
}

// ---------------------------------------------------------------------------
// Minimal blocking HTTP client for the loopback endpoint.
// ---------------------------------------------------------------------------

struct HttpReply {
    int         status = 0;   ///< Parsed HTTP status code (0 on transport failure).
    std::string body;         ///< Response body (after the header terminator).
    std::string raw;          ///< The full raw response, for coarse assertions.
    bool        ok = false;   ///< True iff a response was received.

    /// Response headers with lower-case names, in arrival order (task 5.3: the
    /// `Mcp-Session-Id` emission is observed here).
    std::vector<std::pair<std::string, std::string>> headers;

    /// The value of the response header `name` (matched case-insensitively), or
    /// nullptr when absent.
    [[nodiscard]] const std::string* header(std::string_view name) const {
        std::string wanted(name);
        for (char& c : wanted) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (const auto& [key, value] : headers) {
            if (key == wanted) return &value;
        }
        return nullptr;
    }
};

// Perform a single blocking HTTP request to 127.0.0.1:port and return the reply.
HttpReply httpRequest(std::uint16_t port, const std::string& method,
                      const std::string& path, const std::string& body,
                      const std::vector<std::pair<std::string, std::string>>& extraHeaders = {}) {
    HttpReply reply;
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return reply;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return reply;
    }

    std::string req = method + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n";
    req += "Content-Type: application/json\r\n";
    for (const auto& [name, value] : extraHeaders) {
        req += name + ": " + value + "\r\n";
    }
    req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "Connection: close\r\n\r\n";
    req += body;
    // Large bodies need a loop: a single send() may not accept everything.
    std::size_t sent = 0;
    while (sent < req.size()) {
        const ssize_t n = ::send(fd, req.data() + sent, req.size() - sent, 0);
        if (n <= 0) break;
        sent += static_cast<std::size_t>(n);
    }

    std::string resp;
    char buf[4096];
    while (true) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        resp.append(buf, static_cast<std::size_t>(n));
    }
    ::close(fd);

    if (resp.empty()) return reply;
    reply.ok = true;
    reply.raw = resp;

    // Parse the status code from the status line "HTTP/1.1 <code> <reason>".
    const std::size_t sp = resp.find(' ');
    if (sp != std::string::npos) {
        try {
            reply.status = std::stoi(resp.substr(sp + 1, 3));
        } catch (...) {
            reply.status = 0;
        }
    }

    // The body follows the blank line terminating the headers.
    const std::size_t sep = resp.find("\r\n\r\n");
    if (sep != std::string::npos) {
        reply.body = resp.substr(sep + 4);

        // Parse the header block (skipping the status line) into lower-cased names.
        const std::string headerBlock = resp.substr(0, sep);
        std::size_t       lineStart = headerBlock.find("\r\n");
        while (lineStart != std::string::npos) {
            lineStart += 2;
            const std::size_t lineEnd = headerBlock.find("\r\n", lineStart);
            const std::string line = headerBlock.substr(
                lineStart, lineEnd == std::string::npos ? std::string::npos : lineEnd - lineStart);
            const std::size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string name = line.substr(0, colon);
                for (char& c : name) {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                std::size_t valueStart = colon + 1;
                while (valueStart < line.size() && line[valueStart] == ' ') ++valueStart;
                reply.headers.emplace_back(name, line.substr(valueStart));
            }
            lineStart = lineEnd;
        }
    }
    return reply;
}

// ---------------------------------------------------------------------------
// A wired MCP stack: project session + default tool registry + executor + HTTP
// server,
// started on an ephemeral loopback port. Allows registering extra tools BEFORE
// building the executor (used for the failure-rollback case).
// ---------------------------------------------------------------------------

class McpStack {
public:
    // `withProject == false` models "no project open": the executor is given a
    // null session, while the registry is still built over the (untouched) session
    // so the tool surface is identical.
    explicit McpStack(bool withProject = true) {
        session_ = std::make_unique<ProjectSession>();
        (void)session_->engine().reset(makeProject(trackId_, assetId_));
        registry_ = buildDefaultToolRegistry(*session_);
    }

    // Register an extra tool into the surface before starting. Must be called
    // before start().
    void addTool(Tool tool) { registry_.add(std::move(tool)); }

    // Build the executor + server and bind an ephemeral loopback port.
    void start(bool withProject = true) {
        executor_ = std::make_unique<McpToolExecutor>(
            registry_, withProject ? session_.get() : nullptr);
        server_ = std::make_unique<McpServer>(
            [this](const Json& request) { return executor_->execute(request); });
        const Result<void> r = server_->start("127.0.0.1", 0);
        started_ = r.isOk();
        if (started_) port_ = server_->boundPort();
    }

    ~McpStack() {
        if (server_) server_->stop();
    }

    HttpReply post(const std::string& path, const std::string& body) {
        return httpRequest(port_, "POST", path, body);
    }
    HttpReply request(const std::string& method, const std::string& path,
                      const std::string& body) {
        return httpRequest(port_, method, path, body);
    }

    ProjectSession& session() { return *session_; }
    TimelineEngine& engine() { return session_->engine(); }
    const Uuid& trackId() const { return trackId_; }
    const Uuid& assetId() const { return assetId_; }
    bool started() const { return started_; }
    std::uint16_t port() const { return port_; }

private:
    Uuid                             trackId_;
    Uuid                             assetId_;
    std::unique_ptr<ProjectSession>  session_;
    ToolRegistry                     registry_;
    std::unique_ptr<McpToolExecutor> executor_;
    std::unique_ptr<McpServer>       server_;
    bool                             started_ = false;
    std::uint16_t                    port_ = 0;
};

// Parse a reply body as JSON, failing the current test if it is not valid JSON.
Json parseBody(const HttpReply& reply) {
    Result<Json> parsed = Json::parse(reply.body);
    EXPECT_TRUE(parsed.isOk()) << "response body was not valid JSON: " << reply.body;
    return parsed.isOk() ? parsed.value() : Json::object();
}

// ---------------------------------------------------------------------------
// 7.4 — a recognized edit tool executes over HTTP and the resulting project
// state reflects the edit.
// ---------------------------------------------------------------------------

TEST(McpHttpIntegration, RecognizedEditToolChangesProjectStateOverHttp) {
    McpStack stack;
    stack.start();
    ASSERT_TRUE(stack.started());
    ASSERT_EQ(clipCount(stack.engine()), 0u);

    const HttpReply reply =
        stack.post("/mcp", envelope("timeline.add_clip",
                                    addClipArgs(stack.trackId(), stack.assetId())));

    // HTTP transport succeeded and the tool-call envelope reports success.
    ASSERT_TRUE(reply.ok);
    EXPECT_EQ(reply.status, 200);
    const Json body = parseBody(reply);
    ASSERT_TRUE(body.contains("ok"));
    EXPECT_TRUE(body.find("ok")->asBool());
    ASSERT_TRUE(body.contains("result"));

    // The edit is reflected in the resulting project state.
    EXPECT_EQ(clipCount(stack.engine()), 1u);
}

TEST(McpHttpIntegration, RecognizedReadToolReturnsProjectOverHttp) {
    McpStack stack;
    stack.start();
    ASSERT_TRUE(stack.started());

    const HttpReply reply = stack.post("/mcp", envelope("timeline.read", Json::object()));
    ASSERT_TRUE(reply.ok);
    EXPECT_EQ(reply.status, 200);
    const Json body = parseBody(reply);
    EXPECT_TRUE(body.find("ok")->asBool());
    ASSERT_TRUE(body.contains("result"));
    // The read payload is the serialized project (carries a "tracks" array).
    EXPECT_TRUE(body.find("result")->contains("tracks"));
}

// ---------------------------------------------------------------------------
// 7.5 — an unknown tool returns an error response and leaves the project
// unchanged.
// ---------------------------------------------------------------------------

TEST(McpHttpIntegration, UnknownToolReturnsErrorAndLeavesProjectUnchanged) {
    McpStack stack;
    // Seed one clip so we can confirm the project is untouched by the unknown call.
    ASSERT_TRUE(stack.engine()
                    .apply(std::make_unique<AddClipCommand>(
                        stack.trackId(), makeClip(stack.assetId(), 0)))
                    .isOk());
    stack.start();
    ASSERT_TRUE(stack.started());
    const std::size_t before = clipCount(stack.engine());

    const HttpReply reply =
        stack.post("/mcp", envelope("timeline.no_such_tool", Json::object()));

    // The request is well-formed HTTP, so the transport answers 200 with an
    // error envelope (the tool-level error, not an HTTP-level error).
    ASSERT_TRUE(reply.ok);
    EXPECT_EQ(reply.status, 200);
    const Json body = parseBody(reply);
    ASSERT_TRUE(body.contains("ok"));
    EXPECT_FALSE(body.find("ok")->asBool());
    ASSERT_TRUE(body.contains("error"));
    EXPECT_EQ(body.find("error")->find("code")->asString(),
              std::string(toStringView(ErrorCode::NotFound)));

    // The project is unchanged.
    EXPECT_EQ(clipCount(stack.engine()), before);
}

// ---------------------------------------------------------------------------
// 7.6 — a failing tool rolls the project back to its pre-invocation state and
// returns an error response.
// ---------------------------------------------------------------------------

TEST(McpHttpIntegration, FailingToolRollsBackAndReturnsErrorOverHttp) {
    McpStack stack;
    // A tool that applies a real command successfully and then fails a later step.
    // The executor must roll the applied command back before returning the error.
    Tool failing;
    failing.name = "test.apply_then_fail";
    failing.description = "Applies a clip and then reports failure.";
    // No declared arguments: the default ToolSchema accepts an empty object.
    TimelineEngine* enginePtr = &stack.engine();
    const Uuid trackId = stack.trackId();
    const Uuid assetId = stack.assetId();
    failing.handler = [enginePtr, trackId, assetId](const Json&) -> Result<Json> {
        const CommandResult applied =
            enginePtr->apply(std::make_unique<AddClipCommand>(trackId, makeClip(assetId, 0)));
        EXPECT_TRUE(applied.isOk());
        return err<Json>(failedPrecondition("intentional failure after a successful apply"));
    };
    stack.addTool(std::move(failing));

    stack.start();
    ASSERT_TRUE(stack.started());
    ASSERT_EQ(clipCount(stack.engine()), 0u);

    const HttpReply reply =
        stack.post("/mcp", envelope("test.apply_then_fail", Json::object()));

    ASSERT_TRUE(reply.ok);
    EXPECT_EQ(reply.status, 200);
    const Json body = parseBody(reply);
    ASSERT_TRUE(body.contains("ok"));
    EXPECT_FALSE(body.find("ok")->asBool());
    ASSERT_TRUE(body.contains("error"));
    EXPECT_EQ(body.find("error")->find("code")->asString(),
              std::string(toStringView(ErrorCode::FailedPrecondition)));

    // The clip the tool applied is rolled back — no partial mutation and no undo
    // residue left behind by the aborted invocation.
    EXPECT_EQ(clipCount(stack.engine()), 0u);
    EXPECT_FALSE(stack.engine().canUndo());
}

// ---------------------------------------------------------------------------
// 7.10 — no project open, and schema-invalid requests, return the appropriate
// error responses without mutating the project.
// ---------------------------------------------------------------------------

TEST(McpHttpIntegration, NoProjectOpenReturnsErrorOverHttp) {
    McpStack stack;
    stack.start(/*withProject=*/false);  // executor bound to a null engine
    ASSERT_TRUE(stack.started());

    const HttpReply reply = stack.post("/mcp", envelope("timeline.read", Json::object()));
    ASSERT_TRUE(reply.ok);
    EXPECT_EQ(reply.status, 200);
    const Json body = parseBody(reply);
    ASSERT_TRUE(body.contains("ok"));
    EXPECT_FALSE(body.find("ok")->asBool());
    ASSERT_TRUE(body.contains("error"));
    EXPECT_EQ(body.find("error")->find("code")->asString(),
              std::string(toStringView(ErrorCode::FailedPrecondition)));
}

TEST(McpHttpIntegration, SchemaInvalidRequestReturnsErrorAndLeavesProjectUnchanged) {
    McpStack stack;
    stack.start();
    ASSERT_TRUE(stack.started());
    ASSERT_EQ(clipCount(stack.engine()), 0u);

    // add_clip requires "trackId", "assetId", and "sourceOutNs"; omit the last so
    // schema validation rejects the request BEFORE any command is created.
    Json args = Json::object();
    args.set("trackId", stack.trackId().toString());
    args.set("assetId", stack.assetId().toString());

    const HttpReply reply = stack.post("/mcp", envelope("timeline.add_clip", std::move(args)));
    ASSERT_TRUE(reply.ok);
    EXPECT_EQ(reply.status, 200);
    const Json body = parseBody(reply);
    ASSERT_TRUE(body.contains("ok"));
    EXPECT_FALSE(body.find("ok")->asBool());
    ASSERT_TRUE(body.contains("error"));
    EXPECT_EQ(body.find("error")->find("code")->asString(),
              std::string(toStringView(ErrorCode::InvalidArgument)));

    // No command was created; the project is untouched.
    EXPECT_EQ(clipCount(stack.engine()), 0u);
}

// ---------------------------------------------------------------------------
// HTTP-level protocol conformance over the live socket.
// ---------------------------------------------------------------------------

TEST(McpHttpIntegration, UnknownPathReturns404) {
    McpStack stack;
    stack.start();
    ASSERT_TRUE(stack.started());

    const HttpReply reply = stack.post("/not-mcp", "{}");
    ASSERT_TRUE(reply.ok);
    EXPECT_EQ(reply.status, 404);
}

TEST(McpHttpIntegration, NonPostMethodReturns405) {
    McpStack stack;
    stack.start();
    ASSERT_TRUE(stack.started());

    const HttpReply reply = stack.request("GET", "/mcp", "");
    ASSERT_TRUE(reply.ok);
    EXPECT_EQ(reply.status, 405);
}

TEST(McpHttpIntegration, MalformedJsonBodyReturns400ParseError) {
    McpStack stack;
    stack.start();
    ASSERT_TRUE(stack.started());

    const HttpReply reply = stack.post("/mcp", "this is not json");
    ASSERT_TRUE(reply.ok);
    EXPECT_EQ(reply.status, 400);
    const Json body = parseBody(reply);
    ASSERT_TRUE(body.contains("error"));
    // JSON-RPC parse-error code is surfaced.
    EXPECT_EQ(body.find("error")->intOr("code"), -32700);
}

// ===========================================================================
// Task 5.3 — JSON-RPC 2.0 over real loopback HTTP (Requirements 9.1, 9.6, 9.10,
// 9.11, 10.1, 15.3).
//
// The section above pins the pre-JSON-RPC bespoke envelope, which the transport
// still answers when no protocol layer is wired. This section wires the full
// stage-5 stack the composition root builds —
//
//   ProjectSession -> buildDefaultToolRegistry -> McpToolExecutor
//                          + McpSessionRegistry -> McpProtocolHandler
//                                                        |
//                                    McpServer(protocol delegate), 127.0.0.1:0
//
// — and drives a genuine client handshake over a loopback socket:
// `initialize` -> `notifications/initialized` -> `tools/list` -> `tools/call`.
// Requirement 15.3 is asserted on EVERY response: the body parses as JSON,
// carries `"jsonrpc":"2.0"`, echoes the request identifier unchanged in type and
// value, and carries exactly one of `result` / `error`; and every `tools/list`
// entry carries `name`, `description` and `inputSchema`.
// ===========================================================================

/// A JSON-RPC MCP stack behind a real loopback listener on an ephemeral port
/// (Requirement 10.1: loopback only; never the well-known 19789, so parallel
/// CTest processes cannot collide).
class JsonRpcStack {
public:
    JsonRpcStack() {
        session_ = std::make_unique<ProjectSession>();
        (void)session_->engine().reset(makeProject(trackId_, assetId_));
        registry_ = buildDefaultToolRegistry(*session_);
        executor_ = std::make_unique<McpToolExecutor>(registry_, session_.get());
        handler_ = std::make_unique<McpProtocolHandler>(registry_, *executor_, sessions_,
                                                       inlineMainThreadInvoker());
        server_ = std::make_unique<McpServer>();
        server_->setProtocolDelegate(protocolDelegateFor(*handler_));

        const Result<void> started = server_->start(BindDecision::loopback(/*port=*/0));
        started_ = started.isOk();
        if (started_) port_ = server_->boundPort();
    }

    ~JsonRpcStack() {
        if (server_) server_->stop();
    }

    [[nodiscard]] HttpReply post(const std::string& body,
                                 std::optional<std::string> sessionId = std::nullopt) const {
        std::vector<std::pair<std::string, std::string>> headers;
        if (sessionId.has_value()) headers.emplace_back("Mcp-Session-Id", *sessionId);
        return httpRequest(port_, "POST", "/mcp", body, headers);
    }

    [[nodiscard]] bool            started() const { return started_; }
    [[nodiscard]] std::uint16_t   port() const { return port_; }
    [[nodiscard]] TimelineEngine& engine() { return session_->engine(); }
    [[nodiscard]] ToolRegistry&   registry() { return registry_; }
    [[nodiscard]] const Uuid&     trackId() const { return trackId_; }
    [[nodiscard]] const Uuid&     assetId() const { return assetId_; }

private:
    Uuid                                trackId_;
    Uuid                                assetId_;
    std::unique_ptr<ProjectSession>     session_;
    ToolRegistry                        registry_;
    McpSessionRegistry                  sessions_;
    std::unique_ptr<McpToolExecutor>    executor_;
    std::unique_ptr<McpProtocolHandler> handler_;
    std::unique_ptr<McpServer>          server_;
    bool                                started_ = false;
    std::uint16_t                       port_ = 0;
};

std::string jsonRpcRequest(const Json& id, const std::string& method, Json params) {
    Json request = Json::object();
    request.set("jsonrpc", Json("2.0"));
    if (!id.isNull()) request.set("id", id);
    request.set("method", Json(method));
    if (!params.isNull()) request.set("params", std::move(params));
    return request.dump();
}

/// Requirement 15.3, asserted on every response: valid JSON, `"jsonrpc":"2.0"`,
/// the request id echoed unchanged in type and value, exactly one of
/// `result`/`error`.
Json expectJsonRpcEnvelope(const HttpReply& reply, const Json& expectedId) {
    EXPECT_TRUE(reply.ok) << "no HTTP response was received";
    const Json body = parseBody(reply);
    EXPECT_TRUE(body.isObject()) << reply.body;
    EXPECT_EQ(body.stringOr("jsonrpc"), "2.0") << reply.body;

    const Json* id = body.find("id");
    EXPECT_NE(id, nullptr) << reply.body;
    if (id != nullptr) {
        EXPECT_EQ(static_cast<int>(id->type()), static_cast<int>(expectedId.type()))
            << "the response changed the id's JSON type: " << reply.body;
        EXPECT_TRUE(*id == expectedId) << "the response did not echo the id: " << reply.body;
    }

    const bool hasResult = body.contains("result");
    const bool hasError = body.contains("error");
    EXPECT_NE(hasResult, hasError) << "exactly one of result/error is required: " << reply.body;
    return body;
}

TEST(McpJsonRpcHttpIntegration, FullHandshakeOverLoopbackHttp) {
    JsonRpcStack stack;
    ASSERT_TRUE(stack.started());
    ASSERT_NE(stack.port(), 19789);  // ephemeral, never the well-known port
    ASSERT_EQ(clipCount(stack.engine()), 0u);

    // --- initialize --------------------------------------------------------
    Json initializeParams = Json::object();
    initializeParams.set("protocolVersion", Json("2025-06-18"));
    initializeParams.set("clientInfo",
                         Json::object({{"name", Json("integration-test-client")},
                                       {"version", Json("1.0")}}));
    const Json     initializeId(std::int64_t{1});
    const HttpReply initialize =
        stack.post(jsonRpcRequest(initializeId, "initialize", std::move(initializeParams)));
    EXPECT_EQ(initialize.status, 200);
    const Json initializeBody = expectJsonRpcEnvelope(initialize, initializeId);
    const Json* initializeResult = initializeBody.find("result");
    ASSERT_NE(initializeResult, nullptr);
    EXPECT_EQ(initializeResult->stringOr("protocolVersion"), "2025-06-18");
    const Json* serverInfo = initializeResult->find("serverInfo");
    ASSERT_NE(serverInfo, nullptr);
    EXPECT_FALSE(serverInfo->stringOr("name").empty());
    EXPECT_FALSE(serverInfo->stringOr("version").empty());
    const Json* capabilities = initializeResult->find("capabilities");
    ASSERT_NE(capabilities, nullptr);
    EXPECT_TRUE(capabilities->contains("tools"));

    // Requirement 9.11: the session identifier arrives as a response header and is
    // opaque and at least 32 characters long.
    const std::string* sessionHeader = initialize.header("Mcp-Session-Id");
    ASSERT_NE(sessionHeader, nullptr) << initialize.raw;
    const std::string session = *sessionHeader;
    EXPECT_GE(session.size(), 32u);
    EXPECT_TRUE(McpSessionRegistry::isWellFormedId(session)) << session;

    // --- notifications/initialized (Requirement 9.10) ----------------------
    const HttpReply notified =
        stack.post(jsonRpcRequest(Json(nullptr), "notifications/initialized", Json()), session);
    ASSERT_TRUE(notified.ok);
    EXPECT_EQ(notified.status, 202);
    EXPECT_TRUE(notified.body.empty()) << notified.body;

    // --- tools/list (Requirements 9.3, 15.3) -------------------------------
    const Json      listId("list-1");
    const HttpReply list = stack.post(jsonRpcRequest(listId, "tools/list", Json()), session);
    EXPECT_EQ(list.status, 200);
    const Json  listBody = expectJsonRpcEnvelope(list, listId);
    const Json* listResult = listBody.find("result");
    ASSERT_NE(listResult, nullptr);
    const Json* tools = listResult->find("tools");
    ASSERT_NE(tools, nullptr);
    ASSERT_TRUE(tools->isArray());
    EXPECT_EQ(tools->asArray().size(), stack.registry().size());
    EXPECT_GT(tools->asArray().size(), 0u);
    for (const Json& entry : tools->asArray()) {
        ASSERT_TRUE(entry.isObject());
        EXPECT_FALSE(entry.stringOr("name").empty());
        EXPECT_FALSE(entry.stringOr("description").empty());
        const Json* schema = entry.find("inputSchema");
        ASSERT_NE(schema, nullptr) << entry.stringOr("name");
        EXPECT_TRUE(schema->isObject()) << entry.stringOr("name");
        EXPECT_EQ(schema->stringOr("type"), "object") << entry.stringOr("name");
    }

    // --- tools/call (Requirement 9.4) --------------------------------------
    Json callParams = Json::object();
    callParams.set("name", Json("timeline.add_clip"));
    callParams.set("arguments", addClipArgs(stack.trackId(), stack.assetId()));
    const Json      callId(std::int64_t{2});
    const HttpReply call = stack.post(jsonRpcRequest(callId, "tools/call", std::move(callParams)),
                                      session);
    EXPECT_EQ(call.status, 200);
    const Json  callBody = expectJsonRpcEnvelope(call, callId);
    const Json* callResult = callBody.find("result");
    ASSERT_NE(callResult, nullptr);
    EXPECT_FALSE(callResult->boolOr("isError", true));
    const Json* content = callResult->find("content");
    ASSERT_NE(content, nullptr);
    ASSERT_TRUE(content->isArray());
    ASSERT_FALSE(content->asArray().empty());
    EXPECT_EQ(content->asArray().front().stringOr("type"), "text");

    // The edit reached the real project through the real execution policy.
    EXPECT_EQ(clipCount(stack.engine()), 1u);
}

TEST(McpJsonRpcHttpIntegration, ToolsListWithoutASessionIsRefusedOverHttp) {
    JsonRpcStack stack;
    ASSERT_TRUE(stack.started());

    // No session header at all (Requirement 9.15).
    const Json      id(std::int64_t{5});
    const HttpReply anonymous = stack.post(jsonRpcRequest(id, "tools/list", Json()));
    const Json      anonymousBody = expectJsonRpcEnvelope(anonymous, id);
    ASSERT_TRUE(anonymousBody.contains("error"));
    EXPECT_EQ(anonymousBody.find("error")->intOr("code"),
              McpProtocolHandler::kErrorSessionUnknown);

    // A session identifier the endpoint never issued.
    const HttpReply unknown =
        stack.post(jsonRpcRequest(id, "tools/list", Json()), std::string(64, 'a'));
    const Json unknownBody = expectJsonRpcEnvelope(unknown, id);
    ASSERT_TRUE(unknownBody.contains("error"));
    EXPECT_EQ(unknownBody.find("error")->intOr("code"), McpProtocolHandler::kErrorSessionUnknown);
}

TEST(McpJsonRpcHttpIntegration, UnsupportedMethodIsMethodNotFoundOverHttp) {
    JsonRpcStack stack;
    ASSERT_TRUE(stack.started());

    const Json      id(std::int64_t{6});
    const HttpReply reply = stack.post(jsonRpcRequest(id, "resources/list", Json()));
    const Json      body = expectJsonRpcEnvelope(reply, id);
    ASSERT_TRUE(body.contains("error"));
    EXPECT_EQ(body.find("error")->intOr("code"), McpProtocolHandler::kErrorMethodNotFound);
    EXPECT_NE(body.find("error")->stringOr("message").find("resources/list"), std::string::npos);
}

TEST(McpJsonRpcHttpIntegration, MalformedJsonAndOversizeBodiesAreParseErrors) {
    JsonRpcStack stack;
    ASSERT_TRUE(stack.started());

    // Unparsable body (Requirement 9.6).
    const HttpReply malformed = stack.post("{not json");
    ASSERT_TRUE(malformed.ok);
    EXPECT_EQ(malformed.status, 400);
    const Json malformedBody = expectJsonRpcEnvelope(malformed, Json(nullptr));
    ASSERT_TRUE(malformedBody.contains("error"));
    EXPECT_EQ(malformedBody.find("error")->intOr("code"), McpProtocolHandler::kErrorParse);

    // A body above the 1 MiB cap is refused with -32700 and never dispatched.
    std::string oversize = R"({"jsonrpc":"2.0","id":1,"method":"tools/list","pad":")";
    oversize += std::string(McpServer::kMaxRequestBodyBytes, 'x');
    oversize += "\"}";
    ASSERT_GT(oversize.size(), McpServer::kMaxRequestBodyBytes);
    const HttpReply big = stack.post(oversize);
    ASSERT_TRUE(big.ok);
    EXPECT_EQ(big.status, 400);
    const Json bigBody = expectJsonRpcEnvelope(big, Json(nullptr));
    ASSERT_TRUE(bigBody.contains("error"));
    EXPECT_EQ(bigBody.find("error")->intOr("code"), McpProtocolHandler::kErrorParse);
    EXPECT_EQ(clipCount(stack.engine()), 0u);
}

TEST(McpJsonRpcHttpIntegration, HeaderCaptureAndBindDecisionAreLoopbackOnly) {
    // Pure transport checks that need no listener: header capture into the
    // protocol context, and the loopback-only contract of Requirement 10.1.
    HttpRequest request;
    request.method = "POST";
    request.target = "/mcp";
    request.body = "{}";
    request.headers.emplace_back("mcp-session-id", "abc123");
    request.headers.emplace_back("authorization", "Bearer token-value");
    request.headers.emplace_back("origin", "http://localhost:3000");
    EXPECT_NE(request.header("Mcp-Session-Id"), nullptr);
    EXPECT_EQ(*request.header("MCP-SESSION-ID"), "abc123");
    EXPECT_EQ(*request.header("Origin"), "http://localhost:3000");
    EXPECT_EQ(request.header("x-absent"), nullptr);

    McpServer server;
    BindDecision routable;
    routable.host = "203.0.113.7";
    routable.port = 0;
    routable.loopbackOnly = true;
    const Result<void> refusedHost = server.start(routable);
    ASSERT_TRUE(refusedHost.isError());
    EXPECT_EQ(refusedHost.error().code(), ErrorCode::InvalidArgument);
    EXPECT_FALSE(server.running());

    BindDecision tls = BindDecision::loopback(0);
    tls.tlsEnabled = true;
    const Result<void> refusedTls = server.start(tls);
    ASSERT_TRUE(refusedTls.isError());
    EXPECT_EQ(refusedTls.error().code(), ErrorCode::Unsupported);
    EXPECT_FALSE(server.running());

    // The loopback default binds, and reports the loopback host it bound.
    ASSERT_TRUE(server.start(BindDecision::loopback(0)).isOk());
    EXPECT_TRUE(server.running());
    EXPECT_EQ(server.boundHost(), "127.0.0.1");
    EXPECT_GT(server.boundPort(), 0);
    server.stop();
    EXPECT_TRUE(server.boundHost().empty());
}

}  // namespace
}  // namespace palmier::services
