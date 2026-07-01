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

#include "services/McpServer.hpp"
#include "services/McpToolExecutor.hpp"
#include "services/ToolRegistry.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <memory>
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
};

// Perform a single blocking HTTP request to 127.0.0.1:port and return the reply.
HttpReply httpRequest(std::uint16_t port, const std::string& method,
                      const std::string& path, const std::string& body) {
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
    req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "Connection: close\r\n\r\n";
    req += body;
    ::send(fd, req.data(), req.size(), 0);

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
    }
    return reply;
}

// ---------------------------------------------------------------------------
// A wired MCP stack: engine + default tool registry + executor + HTTP server,
// started on an ephemeral loopback port. Allows registering extra tools BEFORE
// building the executor (used for the failure-rollback case).
// ---------------------------------------------------------------------------

class McpStack {
public:
    // `withProject == false` models "no project open": the executor is given a
    // null engine, while the registry is still built over the (untouched) engine
    // so the tool surface is identical.
    explicit McpStack(bool withProject = true) {
        engine_ = std::make_unique<TimelineEngine>(makeProject(trackId_, assetId_));
        registry_ = buildDefaultToolRegistry(*engine_);
    }

    // Register an extra tool into the surface before starting. Must be called
    // before start().
    void addTool(Tool tool) { registry_.add(std::move(tool)); }

    // Build the executor + server and bind an ephemeral loopback port.
    void start(bool withProject = true) {
        executor_ = std::make_unique<McpToolExecutor>(
            registry_, withProject ? engine_.get() : nullptr);
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

    TimelineEngine& engine() { return *engine_; }
    const Uuid& trackId() const { return trackId_; }
    const Uuid& assetId() const { return assetId_; }
    bool started() const { return started_; }
    std::uint16_t port() const { return port_; }

private:
    Uuid                             trackId_;
    Uuid                             assetId_;
    std::unique_ptr<TimelineEngine>  engine_;
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
    failing.inputSchema = Json::object();  // no declared constraints
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

}  // namespace
}  // namespace palmier::services
