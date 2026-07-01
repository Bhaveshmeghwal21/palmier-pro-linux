// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/services/mcp_server_test.cpp — unit tests for the MCP HTTP transport +
// server lifecycle (task 15.2; Requirements 7.1, 7.2, 7.3, 7.9).
//
// Coverage:
//   * dispatch() routing — only `/mcp` is served (404 otherwise), only POST is
//     accepted (405 otherwise), a non-JSON body is a 400 parse error, a wired
//     handler's JSON is returned with 200, and an absent handler yields 503.
//   * isLoopbackHost() admission — loopback literals accepted, routable/public
//     addresses rejected.
//   * start() on a non-loopback host is refused (loopback-only).
//   * Lifecycle — start() begins accepting and running() reflects it; stop()
//     stops within the budget and is idempotent; a real loopback HTTP round-trip
//     reaches the injected handler and returns its response.
//   * Port-in-use — starting on a port already held by another listener is
//     refused with an error and leaves the server stopped (7.3/7.9).
//
// The socket paths are exercised against an ephemeral loopback port (bind port
// 0) so the tests are deterministic and need no fixed free port.

#include "services/McpServer.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include <gtest/gtest.h>

namespace palmier::services {
namespace {

using namespace std::chrono_literals;

HttpRequest post(std::string target, std::string body) {
    HttpRequest r;
    r.method = "POST";
    r.target = std::move(target);
    r.body = std::move(body);
    return r;
}

// ---- dispatch() routing ---------------------------------------------------

TEST(McpServerDispatch, UnknownPathIs404) {
    McpServer server([](const Json&) { return Json("unused"); });
    const HttpResponse r = server.dispatch(post("/other", "{}"));
    EXPECT_EQ(r.status, 404);
}

TEST(McpServerDispatch, NonPostMethodIs405) {
    McpServer server([](const Json&) { return Json("unused"); });
    HttpRequest req;
    req.method = "GET";
    req.target = "/mcp";
    const HttpResponse r = server.dispatch(req);
    EXPECT_EQ(r.status, 405);
}

TEST(McpServerDispatch, InvalidJsonBodyIsParseError) {
    McpServer server([](const Json&) { return Json("unused"); });
    const HttpResponse r = server.dispatch(post("/mcp", "not json"));
    EXPECT_EQ(r.status, 400);
    // JSON-RPC parse-error code surfaced.
    const Result<Json> parsed = Json::parse(r.body);
    ASSERT_TRUE(parsed.isOk());
    const Json* errObj = parsed.value().find("error");
    ASSERT_NE(errObj, nullptr);
    EXPECT_EQ(errObj->intOr("code"), -32700);
}

TEST(McpServerDispatch, PathWithQueryStringStillRoutesToMcp) {
    bool called = false;
    McpServer server([&](const Json&) { called = true; return Json::object({{"ok", Json(true)}}); });
    const HttpResponse r = server.dispatch(post("/mcp?session=abc", "{}"));
    EXPECT_EQ(r.status, 200);
    EXPECT_TRUE(called);
}

TEST(McpServerDispatch, WellFormedRequestReachesHandlerAndReturnsItsJson) {
    McpServer server([](const Json& req) {
        Json out = Json::object();
        out.set("echo", Json(req.stringOr("method")));
        return out;
    });
    const HttpResponse r = server.dispatch(post("/mcp", R"({"method":"tools/list"})"));
    EXPECT_EQ(r.status, 200);
    const Result<Json> parsed = Json::parse(r.body);
    ASSERT_TRUE(parsed.isOk());
    EXPECT_EQ(parsed.value().stringOr("echo"), "tools/list");
}

TEST(McpServerDispatch, MissingHandlerYields503) {
    McpServer server;  // no handler wired
    const HttpResponse r = server.dispatch(post("/mcp", "{}"));
    EXPECT_EQ(r.status, 503);
}

// ---- isLoopbackHost() -----------------------------------------------------

TEST(McpServerLoopback, AcceptsLoopbackForms) {
    EXPECT_TRUE(McpServer::isLoopbackHost("127.0.0.1"));
    EXPECT_TRUE(McpServer::isLoopbackHost("127.0.0.53"));
    EXPECT_TRUE(McpServer::isLoopbackHost("localhost"));
    EXPECT_TRUE(McpServer::isLoopbackHost("::1"));
}

TEST(McpServerLoopback, RejectsNonLoopback) {
    EXPECT_FALSE(McpServer::isLoopbackHost("0.0.0.0"));
    EXPECT_FALSE(McpServer::isLoopbackHost("192.168.1.10"));
    EXPECT_FALSE(McpServer::isLoopbackHost("8.8.8.8"));
    EXPECT_FALSE(McpServer::isLoopbackHost("example.com"));
}

TEST(McpServerStart, RefusesNonLoopbackHost) {
    McpServer server([](const Json&) { return Json(nullptr); });
    const Result<void> r = server.start("0.0.0.0", 0);
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
    EXPECT_FALSE(server.running());
}

// ---- Lifecycle ------------------------------------------------------------

TEST(McpServerLifecycle, StartThenStopTogglesRunning) {
    McpServer server([](const Json&) { return Json(nullptr); });
    ASSERT_FALSE(server.running());

    const Result<void> r = server.start("127.0.0.1", 0);  // ephemeral port
    ASSERT_TRUE(r.isOk()) << (r.isError() ? r.error().toString() : "");
    EXPECT_TRUE(server.running());
    EXPECT_GT(server.boundPort(), 0);

    // stop() must return well within the 5s budget.
    const auto t0 = std::chrono::steady_clock::now();
    server.stop();
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_FALSE(server.running());
    EXPECT_LT(elapsed, 5s);
}

TEST(McpServerLifecycle, StopIsIdempotent) {
    McpServer server([](const Json&) { return Json(nullptr); });
    server.stop();  // not running -> no-op
    ASSERT_TRUE(server.start("127.0.0.1", 0).isOk());
    server.stop();
    server.stop();  // second stop -> no-op
    EXPECT_FALSE(server.running());
}

TEST(McpServerLifecycle, DoubleStartIsRejected) {
    McpServer server([](const Json&) { return Json(nullptr); });
    ASSERT_TRUE(server.start("127.0.0.1", 0).isOk());
    const Result<void> second = server.start("127.0.0.1", 0);
    ASSERT_TRUE(second.isError());
    EXPECT_EQ(second.error().code(), ErrorCode::FailedPrecondition);
    server.stop();
}

// Perform a single blocking HTTP POST to 127.0.0.1:port/mcp and return the raw
// response, or empty string on failure.
std::string httpPost(std::uint16_t port, const std::string& path, const std::string& body) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return {};
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return {};
    }
    std::string req = "POST " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n";
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
    return resp;
}

TEST(McpServerLifecycle, RealLoopbackRoundTripReachesHandler) {
    McpServer server([](const Json& req) {
        Json out = Json::object();
        out.set("saw", Json(req.stringOr("method")));
        return out;
    });
    ASSERT_TRUE(server.start("127.0.0.1", 0).isOk());
    const std::uint16_t port = server.boundPort();
    ASSERT_GT(port, 0);

    const std::string resp = httpPost(port, "/mcp", R"({"method":"ping"})");
    server.stop();

    ASSERT_FALSE(resp.empty());
    EXPECT_NE(resp.find("200"), std::string::npos);
    // Body should carry the handler's echo.
    EXPECT_NE(resp.find("ping"), std::string::npos);
}

TEST(McpServerLifecycle, RealLoopbackUnknownPathReturns404) {
    McpServer server([](const Json&) { return Json::object(); });
    ASSERT_TRUE(server.start("127.0.0.1", 0).isOk());
    const std::uint16_t port = server.boundPort();
    const std::string resp = httpPost(port, "/nope", "{}");
    server.stop();
    ASSERT_FALSE(resp.empty());
    EXPECT_NE(resp.find("404"), std::string::npos);
}

// ---- Port-in-use (Requirement 7.3) ----------------------------------------

TEST(McpServerPortConflict, RefusesWhenPortAlreadyInUse) {
    // Occupy an ephemeral loopback port with our own listening socket.
    const int occupier = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(occupier, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(0);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    ASSERT_EQ(::bind(occupier, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    ASSERT_EQ(::listen(occupier, 1), 0);

    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    ASSERT_EQ(::getsockname(occupier, reinterpret_cast<sockaddr*>(&bound), &len), 0);
    const std::uint16_t busyPort = ntohs(bound.sin_port);

    // The MCP server must refuse to start on that busy port and stay stopped.
    McpServer server([](const Json&) { return Json(nullptr); });
    const Result<void> r = server.start("127.0.0.1", busyPort);
    EXPECT_TRUE(r.isError());
    EXPECT_FALSE(server.running());
    if (r.isError()) {
        // Message names the port-unavailable condition.
        EXPECT_NE(r.error().message().find("unavailable"), std::string::npos);
    }

    ::close(occupier);
}

}  // namespace
}  // namespace palmier::services
