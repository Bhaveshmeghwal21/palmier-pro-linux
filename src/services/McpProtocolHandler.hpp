// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/McpProtocolHandler.hpp — JSON-RPC 2.0 method dispatch for the MCP
// endpoint (task 5.2; Requirements 9.1-9.16).
//
// design.md D3 splits what used to be one bespoke path into three components.
// This is the middle one: *protocol only*. It validates the JSON-RPC envelope,
// dispatches `initialize`, `notifications/initialized`, `tools/list` and
// `tools/call`, maps every fault onto its assigned JSON-RPC error code, and looks
// up / enforces session state. It knows nothing about sockets: it takes a request
// context (the few request headers the protocol cares about) plus the raw body and
// returns an `McpReply` the transport serializes. That is what makes the whole
// protocol layer unit-testable without a listening socket, and what keeps
// `McpServer` free of protocol knowledge.
//
// It holds a `ToolRegistry&` (the advertised surface), an `McpToolExecutor&` (the
// one execution policy the GUI and the in-app agent also use, so `tools/call`
// cannot acquire different validation, rollback or undo behaviour) and an
// `McpSessionRegistry&` (identity, the session maximum, the idle timeout).
//
// Dispatch order — fixed, because the requirements assign a different code to
// each stage and a later stage must never mask an earlier one:
//
//   1. body size / JSON parse            -> -32700  (Requirement 9.6)
//   2. JSON-RPC envelope shape           -> -32600  (Requirement 9.7)
//   3. method recognition                -> -32601  (Requirement 9.8)
//   4. session state (unknown / expired / not initialized)
//                                        -> -32001 / -32002 (Requirements 9.14, 9.15)
//   5. `ToolSchema::validate` of the arguments, and the tool's existence
//                                        -> -32602  (Requirements 9.3, 9.9, 9.12)
//   6. execution through `McpToolExecutor`          (Requirements 9.4, 9.5, 9.16)
//
// Only stage 6 can create an edit command, so every fault in stages 1-5 provably
// leaves the project untouched.
//
// Main-thread marshalling (design.md D5, Requirement 9.16). `tools/call` runs the
// execution step through a `MainThreadInvoker` seam carrying the 60-second budget:
// the Qt composition posts the work to the main thread and waits, returning a
// Timeout error when the budget elapses; tests and headless builds supply
// `inlineMainThreadInvoker()`, which runs the work on the calling thread. A
// Timeout from either path is reported the way Requirement 9.16 specifies — a
// result with `isError` true naming the tool and the exceeded limit, not a
// transport failure.
//
// Dependency-light: core Result/Error plus the service-layer `Json` value. The
// collaborators are forward-declared, so including this header costs the transport
// nothing.

#ifndef PALMIER_SERVICES_MCPPROTOCOLHANDLER_HPP
#define PALMIER_SERVICES_MCPPROTOCOLHANDLER_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "core/Result.hpp"
#include "services/Json.hpp"

namespace palmier::services {

class ToolRegistry;         // services/ToolRegistry.hpp
class McpToolExecutor;      // services/McpToolExecutor.hpp
class McpSessionRegistry;   // services/McpSessionRegistry.hpp

// ---------------------------------------------------------------------------
// Request context / reply
// ---------------------------------------------------------------------------

/// The request facts the protocol layer needs, captured by the transport. Headers
/// the protocol does not use are deliberately absent; `authorization` and `origin`
/// are carried because the remote-access gate (task 6.2) admits requests from the
/// same value, upstream of this handler.
struct McpRequestContext {
    std::string                sourceAddress;
    std::optional<std::string> sessionId;
    std::optional<std::string> authorization;
    std::optional<std::string> origin;
    std::size_t                bodyBytes = 0;
    bool                       secureTransport = false;
};

/// What the transport should send back. An empty `body` means a zero-byte body
/// (Requirement 9.10's 202 answer to `notifications/initialized`), and
/// `newSessionId`, when present, is emitted as the `Mcp-Session-Id` response
/// header (Requirement 9.11).
struct McpReply {
    int                        httpStatus = 200;
    std::string                body;
    std::optional<std::string> newSessionId;
};

// ---------------------------------------------------------------------------
// Main-thread marshalling seam
// ---------------------------------------------------------------------------

/// Runs `work` where the project may legally be mutated (the main thread, per
/// design.md D5) and waits at most `budget` for it, returning a Timeout error when
/// the budget elapses (Requirement 9.16).
using MainThreadInvoker =
    std::function<Result<Json>(std::function<Result<Json>()> work,
                               std::chrono::milliseconds budget)>;

/// An invoker that runs the work on the calling thread and never times out by
/// itself. Used by tests and by headless builds, where the calling thread already
/// *is* the only thread touching the project.
[[nodiscard]] MainThreadInvoker inlineMainThreadInvoker();

// ---------------------------------------------------------------------------
// McpProtocolHandler
// ---------------------------------------------------------------------------

/// JSON-RPC 2.0 dispatch for the MCP endpoint.
class McpProtocolHandler {
public:
    /// Protocol revisions this handler speaks, newest first. `initialize`
    /// negotiates the client's requested version when it appears here and
    /// otherwise the highest one, which is `kSupportedProtocolVersions[0]`
    /// (Requirement 9.2).
    static constexpr std::string_view kSupportedProtocolVersions[] = {"2025-06-18",
                                                                     "2025-03-26"};

    /// JSON-RPC error codes. The first five are the spec-assigned codes
    /// Requirements 9.6-9.9 name; the session codes are in the JSON-RPC
    /// implementation-defined server range and carry Requirements 9.14, 9.15 and
    /// 10.9.
    static constexpr std::int64_t kErrorParse                = -32700;
    static constexpr std::int64_t kErrorInvalidRequest       = -32600;
    static constexpr std::int64_t kErrorMethodNotFound       = -32601;
    static constexpr std::int64_t kErrorInvalidParams        = -32602;
    static constexpr std::int64_t kErrorInternal             = -32603;
    static constexpr std::int64_t kErrorSessionUnknown       = -32001;
    static constexpr std::int64_t kErrorSessionNotInitialized = -32002;
    static constexpr std::int64_t kErrorSessionLimit          = -32003;

    /// The 1 MiB request-body cap of Requirements 9.1 and 9.6.
    static constexpr std::size_t kDefaultMaxBodyBytes = 1024u * 1024u;

    /// Server identity defaults reported by `initialize` (Requirement 9.2).
    static constexpr std::string_view kDefaultServerName    = "palmier-pro-linux";
    static constexpr std::string_view kDefaultServerVersion = "0.1.0";

    struct Options {
        std::string               serverName{kDefaultServerName};
        std::string               serverVersion{kDefaultServerVersion};
        std::size_t               maxBodyBytes = kDefaultMaxBodyBytes;
        std::chrono::milliseconds toolBudget = std::chrono::seconds(60);
    };

    /// Construct over the tool surface, the execution policy and the session
    /// registry, all borrowed and required to outlive the handler. An empty
    /// `invoker` behaves as `inlineMainThreadInvoker()`.
    ///
    /// The four-argument form uses the default Options (the overload — rather than
    /// a defaulted argument — avoids relying on the nested aggregate's default
    /// member initializers inside its own class scope, matching McpToolExecutor).
    McpProtocolHandler(const ToolRegistry& registry, McpToolExecutor& executor,
                       McpSessionRegistry& sessions, MainThreadInvoker invoker);
    McpProtocolHandler(const ToolRegistry& registry, McpToolExecutor& executor,
                       McpSessionRegistry& sessions, MainThreadInvoker invoker,
                       Options options);

    /// Handle one request. Never throws; always returns a reply the transport can
    /// send verbatim.
    [[nodiscard]] McpReply handle(const McpRequestContext& context,
                                  std::string_view rawBody);

    /// True iff `version` is one of `kSupportedProtocolVersions`.
    [[nodiscard]] static bool isSupportedProtocolVersion(std::string_view version) noexcept;

    /// The highest supported protocol version (Requirement 9.2's fallback).
    [[nodiscard]] static std::string_view latestProtocolVersion() noexcept {
        return kSupportedProtocolVersions[0];
    }

    /// The effective request-body cap.
    [[nodiscard]] std::size_t maxBodyBytes() const noexcept { return options_.maxBodyBytes; }

    /// The per-`tools/call` budget handed to the invoker.
    [[nodiscard]] std::chrono::milliseconds toolBudget() const noexcept {
        return options_.toolBudget;
    }

private:
    // Reply builders. `id` is echoed unchanged in both type and value
    // (Requirement 9.13).
    [[nodiscard]] static McpReply successReply(const Json& id, Json result, int httpStatus = 200);
    [[nodiscard]] static McpReply errorReply(const Json& id, std::int64_t code,
                                             std::string message, int httpStatus = 200);

    // Method implementations.
    [[nodiscard]] McpReply handleInitialize(const McpRequestContext& context, const Json& id,
                                            const Json* params);
    [[nodiscard]] McpReply handleInitializedNotification(const McpRequestContext& context);
    [[nodiscard]] McpReply handleToolsList(const McpRequestContext& context, const Json& id);
    [[nodiscard]] McpReply handleToolsCall(const McpRequestContext& context, const Json& id,
                                           const Json* params);

    /// Resolve and validate the session named by the context for a `tools/list` /
    /// `tools/call` request: unknown or expired -> -32001 (Requirement 9.15), live
    /// but not yet initialized -> -32002 (Requirement 9.14). Returns std::nullopt
    /// when the session is usable, or the reply to send when it is not.
    [[nodiscard]] std::optional<McpReply> checkSession(const McpRequestContext& context,
                                                       const Json& id);

    const ToolRegistry* registry_;
    McpToolExecutor*    executor_;
    McpSessionRegistry* sessions_;
    MainThreadInvoker   invoker_;
    Options             options_;
};

/// Adapt `handler` into the delegate `McpServer` stores (borrowed; `handler` must
/// outlive the returned callable). Mirrors `handlerFor(IMcpRequestHandler&)`: the
/// indirection is what keeps the transport translation unit free of any dependency
/// on the protocol implementation.
using McpProtocolDelegate =
    std::function<McpReply(const McpRequestContext& context, std::string_view rawBody)>;

[[nodiscard]] McpProtocolDelegate protocolDelegateFor(McpProtocolHandler& handler);

}  // namespace palmier::services

#endif  // PALMIER_SERVICES_MCPPROTOCOLHANDLER_HPP
