// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/McpServer.hpp — the MCP HTTP transport + server lifecycle (task 15.2).
//
// design.md "Component 2: MCP Server" describes an `McpServer` constructed with a
// TimelineEngine and a ToolRegistry that "serve[s] MCP over HTTP at
// http://127.0.0.1:19789/mcp (loopback only, matching the original)". This file
// owns ONLY the transport half of that component:
//
//   * bind a loopback-only listening socket at `127.0.0.1:19789` and accept HTTP
//     requests on the single path `/mcp` (Requirement 7.1);
//   * begin accepting connections when `start()` is called and stop accepting
//     within the design's time budgets when `stop()` is called (Requirements 7.2
//     start-within-5s, 7.9 stop-within-5s — a socket bind/listen and a thread
//     join are effectively instantaneous, comfortably inside those budgets);
//   * on a port-in-use failure, refuse to start the endpoint, leave the current
//     project unchanged (this layer never touches the project), and surface an
//     error that the port is unavailable (Requirement 7.3/7.9).
//
// It does NOT implement tool execution, input validation, rollback, or the
// per-tool timeout policy — that is the executor's job (task 15.3, Requirements
// 7.4-7.7, 7.10). Instead the server delegates every well-formed request to an
// injected request handler (`McpRequestHandler`, or an `IMcpRequestHandler`
// implementation). The composition root (task 21.1) constructs the executor and
// plugs it in here, so this transport stays free of any domain / project
// dependency and compiles + unit-tests standalone.
//
// Testability
// -----------
// The socket layer and the request-routing layer are deliberately separated so
// the behaviour can be verified without a real long-running server:
//
//   * `dispatch()` maps a parsed `HttpRequest` to an `HttpResponse` purely in
//     memory (path routing, method checks, JSON body parsing, delegation to the
//     handler). It touches no sockets, so path/method/parse routing is unit
//     testable directly.
//   * `isLoopbackHost()` exposes the loopback-only admission check.
//   * `start()`/`stop()`/`running()`/`boundPort()` expose the real bind/listen/
//     accept lifecycle; binding `port == 0` lets a test bind an ephemeral port
//     and observe the port-in-use path deterministically.
//
// The implementation uses only POSIX sockets and the C++ standard library, so it
// builds without Qt/FFmpeg/Vulkan and links only against Palmier::core (for
// Result/Error) and the service-layer Json value.

// Task 5.3 — the transport half of design.md D3
// ---------------------------------------------
// The transport gains exactly the mechanics JSON-RPC 2.0 needs and no protocol
// knowledge:
//
//   * `start(const BindDecision&)` — the bind is now described by a value
//     (services/RemoteAccessTypes.hpp), so the loopback default of Requirement
//     10.1 and any later non-loopback decision arrive through one door. The
//     original `start(host, port)` remains as a thin adapter over it.
//   * Header capture — the request line, `Mcp-Session-Id`, `Authorization` and
//     `Origin` are parsed into an `McpRequestContext` together with the peer
//     address, through the pure `contextFor()`.
//   * `Mcp-Session-Id` emission — an `McpReply::newSessionId` becomes a response
//     header (Requirement 9.11).
//   * 202-with-empty-body — a zero-byte body is serialized with
//     `Content-Length: 0` and no `Content-Type` (Requirement 9.10).
//   * A 1 MiB body cap yielding JSON-RPC -32700 (Requirements 9.1, 9.6).
//   * Delegation to `services::McpProtocolHandler` through the
//     `McpProtocolDelegate` seam, so this translation unit depends on the
//     protocol layer's *types* but on none of its code.
//
// `dispatch()` keeps its original pure, socket-free, JSON-in/JSON-out shape and
// its original bespoke-envelope behaviour, so the transport unit tests written
// against it remain valid; the JSON-RPC path lives in the equally pure
// `dispatchWithContext()`, which falls back to `dispatch()` when no protocol
// delegate is wired.

#ifndef PALMIER_SERVICES_MCPSERVER_HPP
#define PALMIER_SERVICES_MCPSERVER_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "core/Result.hpp"
#include "services/Json.hpp"
#include "services/McpProtocolHandler.hpp"
#include "services/RemoteAccessTypes.hpp"

namespace palmier::services {

// ---------------------------------------------------------------------------
// Minimal HTTP request / response
// ---------------------------------------------------------------------------

/// A parsed HTTP request, reduced to what the MCP endpoint needs. `target` is the
/// raw request target (may carry a `?query`); `path()` returns just the path.
struct HttpRequest {
    std::string method;  ///< Upper-case HTTP method, e.g. "POST".
    std::string target;  ///< Request target, e.g. "/mcp" or "/mcp?x=1".
    std::string body;    ///< Raw request body (expected to be JSON for `/mcp`).

    /// Request headers with lower-case names, in arrival order. Captured so the
    /// protocol layer can be handed an `McpRequestContext` (task 5.3).
    std::vector<std::pair<std::string, std::string>> headers;

    /// The path component of `target` (everything before the first '?').
    [[nodiscard]] std::string path() const;

    /// The value of the header `lowerCaseName`, or nullptr when absent. `name` is
    /// matched case-insensitively, as HTTP requires.
    [[nodiscard]] const std::string* header(std::string_view name) const;
};

/// A minimal HTTP response the transport serializes back to the client.
struct HttpResponse {
    int         status = 200;                       ///< HTTP status code.
    std::string reason = "OK";                      ///< Reason phrase.
    std::string contentType = "application/json";   ///< Content-Type header value.
    std::string body;                               ///< Response body.

    /// Extra response headers, e.g. `Mcp-Session-Id` (Requirement 9.11).
    std::vector<std::pair<std::string, std::string>> headers;

    /// Serialize to an HTTP/1.1 response message (status line + headers + body,
    /// with `Connection: close` and a correct `Content-Length`). An empty
    /// `contentType` omits the `Content-Type` header, which is what a zero-byte
    /// 202 answer needs (Requirement 9.10).
    [[nodiscard]] std::string toWire() const;
};

/// The canonical reason phrase for `status` (falls back to a generic phrase).
[[nodiscard]] std::string_view httpReasonPhrase(int status) noexcept;

// ---------------------------------------------------------------------------
// Request handler seam (implemented by task 15.3's executor)
// ---------------------------------------------------------------------------

/// The transport-facing handler the server calls for each well-formed request.
/// It receives the parsed JSON request body and returns the JSON response body.
/// Task 15.3's tool executor is adapted to this signature; keeping the seam here
/// (and nothing about tools/rollback/timeout) is what lets this transport layer
/// stay independent of the domain core.
using McpRequestHandler = std::function<Json(const Json& request)>;

/// Abstract equivalent of `McpRequestHandler` for callers that prefer an
/// interface (e.g. task 15.3's `McpToolExecutor`). Use `handlerFor()` to adapt an
/// instance into the `McpRequestHandler` the server stores.
class IMcpRequestHandler {
public:
    virtual ~IMcpRequestHandler() = default;

    /// Handle a parsed JSON request object and produce the JSON response object.
    [[nodiscard]] virtual Json handleRequest(const Json& request) = 0;
};

/// Adapt an `IMcpRequestHandler` (borrowed; must outlive the returned callable)
/// into an `McpRequestHandler`.
[[nodiscard]] McpRequestHandler handlerFor(IMcpRequestHandler& handler);

// ---------------------------------------------------------------------------
// McpServer
// ---------------------------------------------------------------------------

/// The MCP HTTP server: a loopback-only listener at `127.0.0.1:19789` serving the
/// single path `/mcp`, delegating each request to an injected handler. Owns the
/// listening socket and one background accept thread between `start()` and
/// `stop()`. Non-copyable and non-movable (it owns OS resources and a thread).
class McpServer {
public:
    /// Canonical endpoint constants (design.md Component 2 / Requirement 7.1).
    static constexpr std::string_view kDefaultHost = "127.0.0.1";
    static constexpr std::uint16_t     kDefaultPort = 19789;
    static constexpr std::string_view  kPath = "/mcp";

    /// The request-body cap of Requirements 9.1 and 9.6: a larger body is refused
    /// with JSON-RPC error -32700 and never reaches the protocol layer.
    static constexpr std::size_t kMaxRequestBodyBytes = 1024u * 1024u;

    /// The request header carrying the session identifier (Requirement 9.11).
    static constexpr std::string_view kSessionHeader = "Mcp-Session-Id";

    /// Construct with the request handler. A null handler is permitted (the
    /// server will answer well-formed requests with a 503 "handler unavailable"
    /// until one is wired); the composition root supplies the real executor.
    explicit McpServer(McpRequestHandler handler = {});

    ~McpServer();

    McpServer(const McpServer&) = delete;
    McpServer& operator=(const McpServer&) = delete;
    McpServer(McpServer&&) = delete;
    McpServer& operator=(McpServer&&) = delete;

    /// Replace the request handler. Safe to call while stopped.
    void setHandler(McpRequestHandler handler);

    /// Wire the JSON-RPC 2.0 protocol layer (task 5.2). Once set, every request to
    /// `/mcp` is answered by the delegate — `initialize`,
    /// `notifications/initialized`, `tools/list`, `tools/call` — and the bespoke
    /// `McpRequestHandler` envelope is no longer consulted. Use
    /// `protocolDelegateFor(handler)` to adapt an `McpProtocolHandler`, which must
    /// outlive the server. Passing an empty delegate restores the legacy path.
    void setProtocolDelegate(McpProtocolDelegate delegate);

    /// True iff a protocol delegate is wired.
    [[nodiscard]] bool hasProtocolDelegate() const noexcept
        { return static_cast<bool>(protocol_); }

    /// Bind the loopback endpoint and begin accepting connections.
    ///
    /// `host` MUST be a loopback address (see `isLoopbackHost`) — a non-loopback
    /// host is refused with an InvalidArgument error, matching the "loopback only"
    /// contract. On success the accept thread is running before this returns
    /// (Requirement 7.2: accepting within the start budget).
    ///
    /// Returns an error and leaves the server stopped (and the project untouched)
    /// when: the server is already running (FailedPrecondition); the host is not
    /// loopback (InvalidArgument); or the address/port cannot be bound because it
    /// is already in use (Unavailable-style error whose message states the MCP
    /// endpoint port is unavailable — Requirement 7.3).
    Result<void> start(std::string_view host = kDefaultHost, std::uint16_t port = kDefaultPort);

    /// Bind the endpoint described by `decision` and begin accepting connections
    /// (task 5.3). This is the form the composition root uses: the bind address,
    /// the port, the loopback-only contract and the TLS expectation are one value
    /// produced upstream (`BindDecision::loopback()` is Requirement 10.1's
    /// default), so the transport applies a decision instead of making one.
    ///
    /// Returns an error and leaves the server stopped when: the server is already
    /// running (FailedPrecondition); `decision.loopbackOnly` is set and the host is
    /// not a loopback literal, or the host is not an IPv4 literal this listener can
    /// bind (InvalidArgument); `decision.tlsEnabled` is set, since the TLS
    /// transport is not compiled in yet (Unsupported — task 6.3 supplies it); or
    /// the address is already in use (FailedPrecondition naming the port).
    Result<void> start(const BindDecision& decision);

    /// Stop accepting connections and join the accept thread (idempotent — a no-op
    /// when not running). Returns within the stop budget (Requirement 7.9).
    void stop();

    /// True while the endpoint is bound and the accept thread is running.
    [[nodiscard]] bool running() const noexcept { return running_.load(); }

    /// The actual bound port (useful when starting with `port == 0` to bind an
    /// ephemeral port in tests), or 0 when not running.
    [[nodiscard]] std::uint16_t boundPort() const noexcept { return boundPort_.load(); }

    /// Route a single (already-parsed) HTTP request to a response — the pure,
    /// socket-free core of the transport. Requests to a path other than `/mcp`
    /// yield 404; a method other than POST on `/mcp` yields 405; a body that is
    /// not valid JSON yields a 400 JSON-RPC parse error; otherwise the handler is
    /// invoked and its JSON returned with 200 (or 503 when no handler is wired).
    [[nodiscard]] HttpResponse dispatch(const HttpRequest& request) const;

    /// Route a single (already-parsed) HTTP request with its captured context —
    /// the JSON-RPC path, and equally pure and socket-free (task 5.3).
    ///
    /// A path other than `/mcp` yields 404 and a method other than POST yields
    /// 405, as before. A body larger than `kMaxRequestBodyBytes` yields a
    /// JSON-RPC -32700 error without reaching the protocol layer (Requirement
    /// 9.6). Otherwise, when a protocol delegate is wired, the reply's status and
    /// body are returned verbatim — including a zero-byte 202 body — and a minted
    /// session identifier is emitted as the `Mcp-Session-Id` header (Requirements
    /// 9.10, 9.11). With no delegate wired this falls back to `dispatch()`.
    [[nodiscard]] HttpResponse dispatchWithContext(const HttpRequest& request,
                                                   const McpRequestContext& context) const;

    /// Build the protocol-layer request context from a parsed request, the peer's
    /// address and whether the connection was carried over TLS. Pure, so header
    /// capture is testable without a socket.
    [[nodiscard]] static McpRequestContext contextFor(const HttpRequest& request,
                                                      std::string sourceAddress,
                                                      bool secureTransport);

    /// True iff `host` is an IPv4/IPv6 loopback literal (`127.0.0.0/8`, `::1`, or
    /// the textual forms `localhost`/`ip6-localhost`).
    [[nodiscard]] static bool isLoopbackHost(std::string_view host);

    /// The host this server is bound to, or an empty string when not running.
    [[nodiscard]] std::string boundHost() const;

private:
    void acceptLoop();          ///< Background thread body.
    void closeListenSocket();   ///< Close listen fd if open.

    McpRequestHandler   handler_;
    McpProtocolDelegate protocol_;   ///< JSON-RPC 2.0 layer (task 5.2), or empty.

    std::string      boundHost_;       ///< Host of the current bind ("" when stopped).
    bool             secureTransport_ = false;  ///< True when the listener serves TLS.

    int              listenFd_ = -1;   ///< Listening socket (or -1).
    int              wakeReadFd_ = -1;  ///< Self-pipe read end for stop() wakeup.
    int              wakeWriteFd_ = -1; ///< Self-pipe write end.
    std::thread      acceptThread_;
    std::atomic<bool>          running_{false};
    std::atomic<bool>          stopRequested_{false};
    std::atomic<std::uint16_t> boundPort_{0};
};

}  // namespace palmier::services

#endif  // PALMIER_SERVICES_MCPSERVER_HPP
