// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/McpToolExecutor.hpp — the MCP tool-execution policy (task 15.3).
//
// design.md "Component 2: MCP Server" describes an McpServer that maps MCP tool
// calls to EditCommands on the TimelineEngine and reuses the exact same command
// path as the UI. That component naturally splits into two concerns:
//
//   * transport / lifecycle — binding the loopback HTTP endpoint at
//     `http://127.0.0.1:19789/mcp`, accepting/serving requests, and stopping
//     within the time budgets (Requirements 7.1-7.3, 7.9). This is the McpServer
//     (task 15.2).
//   * execution policy — given a decoded tool name + JSON arguments, run the
//     tool against the current project *safely*: within a time budget, atomic,
//     and with well-defined errors (Requirements 7.4-7.7, 7.10). That is THIS
//     component.
//
// The McpToolExecutor is deliberately transport-agnostic: it knows nothing about
// HTTP, sockets, or MCP framing. The HTTP server (task 15.2) decodes a request
// into a tool name + arguments and calls into this executor; the in-app agent
// (task 16.x) can drive the identical policy. It wraps a `ToolRegistry` (the
// shared tool surface from task 15.1) plus the `TimelineEngine` the tools mutate,
// and enforces:
//
//   * 7.4  A recognized tool executes on the current project and returns its
//          result within the time budget (default 30 seconds).
//   * 7.5  An unrecognized tool name leaves the project unchanged and returns an
//          error indicating the tool name is unknown (ErrorCode::NotFound).
//   * 7.6  If a tool's execution fails, the project is rolled back to its exact
//          pre-invocation state and the failure error is returned.
//   * 7.7  If a tool does not complete within the time budget, the operation is
//          aborted, the project is rolled back to its pre-invocation state, and a
//          timeout error is returned (ErrorCode::Timeout).
//   * 7.10 If a request targets a project operation while no project is open, an
//          error indicating no project is open is returned (ErrorCode::
//          FailedPrecondition); and tool inputs are validated against the tool's
//          declared JSON input schema BEFORE any EditCommand is created.
//
// Rollback strategy. The TimelineEngine is already atomic per command (a failed
// apply leaves the project byte-for-byte unchanged and records nothing). The
// executor adds the guarantee the requirements need across the *whole*
// invocation — including a tool that applied one or more commands successfully
// and then failed a later step, and the timeout case where a synchronous handler
// completed but overran its budget: it observes how many commands the invocation
// applied and, on any abort, undoes exactly that many through the engine's own
// undo path. That restores the pre-invocation project state and removes the
// aborted commands from the undo history (no undo residue), while preserving the
// undo history that existed before the invocation.

#ifndef PALMIER_SERVICES_MCPTOOLEXECUTOR_HPP
#define PALMIER_SERVICES_MCPTOOLEXECUTOR_HPP

#include <chrono>
#include <functional>
#include <string>
#include <string_view>

#include "core/Result.hpp"
#include "services/Json.hpp"

namespace palmier {
class TimelineEngine;  // core/TimelineEngine.hpp — the mutation target.
}  // namespace palmier

namespace palmier::services {

class ToolRegistry;  // services/ToolRegistry.hpp — the shared tool surface.

// ---------------------------------------------------------------------------
// McpToolExecutor
// ---------------------------------------------------------------------------

/// Runs editor tools from a ToolRegistry against a TimelineEngine under the MCP
/// execution policy (Requirements 7.4-7.7, 7.10). Transport-agnostic: the HTTP
/// server (task 15.2) and the in-app agent (task 16.x) both call into it.
class McpToolExecutor {
public:
    /// Monotonic clock source. Injectable so tests can drive the timeout policy
    /// deterministically without real waiting; defaults to std::steady_clock.
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    /// Tuning knobs. `timeBudget` is the per-invocation completion budget
    /// (Requirements 7.4/7.7; default 30 seconds). `clock`, when set, overrides
    /// the monotonic clock used to measure a tool's elapsed time.
    struct Options {
        std::chrono::milliseconds timeBudget = std::chrono::seconds(30);
        Clock                     clock;  // empty -> std::steady_clock::now
    };

    /// Construct an executor over `registry` and `engine`.
    ///
    /// `engine` is the project the tools mutate and may be null to model "no
    /// project open" (Requirement 7.10): while null, every tool call returns the
    /// no-project error and the registry is never invoked. When non-null it MUST
    /// be the same engine the registry's handlers were built against (see
    /// buildDefaultToolRegistry), so the observed/rolled-back mutations line up.
    /// `registry` must outlive the executor.
    ///
    /// The two-argument form uses the default Options (a 30-second budget and the
    /// std::steady_clock). (The overload — rather than a defaulted Options
    /// argument — sidesteps the "default member initializer required before the
    /// end of its enclosing class" rule for the nested Options aggregate.)
    McpToolExecutor(const ToolRegistry& registry, TimelineEngine* engine);
    McpToolExecutor(const ToolRegistry& registry, TimelineEngine* engine,
                    Options options);

    /// Point the executor at a different current project (or null for "no project
    /// open"). Used by the composition root when the open project changes.
    void setEngine(TimelineEngine* engine) noexcept { engine_ = engine; }

    /// True iff a project is currently open (an engine is bound).
    [[nodiscard]] bool hasProject() const noexcept { return engine_ != nullptr; }

    /// The per-invocation completion budget.
    [[nodiscard]] std::chrono::milliseconds timeBudget() const noexcept { return budget_; }

    /// Execute the tool `name` with the (already-parsed) JSON `input`, enforcing
    /// the full policy. Returns the tool's success payload, or an Error:
    ///   * NotFound            — unknown tool name (7.5); project untouched.
    ///   * FailedPrecondition  — no project is open (7.10); project untouched.
    ///   * InvalidArgument     — input failed schema validation (7.10); no command
    ///                           was created, project untouched.
    ///   * Timeout             — the tool overran the time budget (7.7); project
    ///                           rolled back to its pre-invocation state.
    ///   * (tool's error)      — the tool's execution failed (7.6); project rolled
    ///                           back to its pre-invocation state.
    [[nodiscard]] Result<Json> executeTool(std::string_view name, const Json& input);

    /// Envelope entry point for the HTTP transport (task 15.2). Accepts a request
    /// object carrying the tool `name` and its `arguments` — either at the top
    /// level (`{"name": ..., "arguments": {...}}`) or nested under `params`
    /// (`{"params": {"name": ..., "arguments": {...}}}`) as JSON-RPC-style MCP
    /// `tools/call` requests do. Never throws; always returns a JSON object:
    ///   * success -> `{"ok": true,  "result": <tool payload>}`
    ///   * failure -> `{"ok": false, "error": {"code": <ErrorCode>, "message": ...}}`
    /// The HTTP layer (task 15.2) is responsible for MCP/JSON-RPC framing around
    /// this payload; keeping framing out of the executor preserves its transport
    /// independence.
    [[nodiscard]] Json execute(const Json& request);

    /// Minimal JSON-Schema validation used by executeTool before a command is
    /// created (Requirement 7.10). Checks, against the draft-07-style object
    /// schemas the ToolRegistry emits: that an `"object"` schema receives an
    /// object; that every entry in `"required"` is present; and that any supplied
    /// property whose schema declares a `"type"` matches that type ("string",
    /// "integer", "number", "boolean", "object", "array", "null"). It is
    /// intentionally not a full validator — just enough to reject malformed tool
    /// arguments up front. Returns ok() when no declared constraint is violated.
    [[nodiscard]] static Result<void> validateAgainstSchema(const Json& input,
                                                            const Json& schema);

private:
    // Undo `appliedCount` most-recently-applied commands to roll the project back
    // to its pre-invocation state (used on failure/timeout aborts).
    void rollback(int appliedCount);

    const ToolRegistry*       registry_;
    TimelineEngine*           engine_;
    std::chrono::milliseconds budget_;
    Clock                     clock_;
};

}  // namespace palmier::services

#endif  // PALMIER_SERVICES_MCPTOOLEXECUTOR_HPP
