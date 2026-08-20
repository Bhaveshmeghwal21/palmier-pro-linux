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
// shared tool surface from task 15.1) plus the `ProjectSession` holding the
// project the tools mutate, and enforces:
//
//   * 7.4  A recognized tool executes on the current project and returns its
//          result within the time budget (default 60 seconds, Requirement 9.16).
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
// Task 3.4 — the session, the shared validator, the budget and the source tag
// -----------------------------------------------------------------------------
// Four changes, none of them behavioural beyond what the requirements state
// (design.md decisions D1 and D3):
//
//   * The `TimelineEngine*` member became a `ProjectSession*`. The engine is
//     resolved through `session->engine()` at invocation time, exactly as the
//     registry's handlers do, so the executor and the handlers always observe the
//     same engine — including after a project has been loaded into the session
//     (D1). A null session still models "no project open".
//   * `validateAgainstSchema` no longer re-implements a JSON-Schema subset: it
//     delegates to `ToolSchema::validate`, the single declaration each tool
//     publishes through `Tool::inputSchema()` (D3, Requirements 9.9, 9.12).
//   * The default time budget is 60 seconds, matching Requirement 9.16.
//   * `executeTool` takes an `InvocationSource { Gui, Mcp, Agent }`. It is a
//     LOGGING tag only: it names which surface issued the call for the optional
//     invocation log and has no effect on validation, execution, rollback or the
//     budget, so the GUI, the MCP endpoint and the in-app agent remain one
//     execution path (Requirements 1.7, 9.4, 11.5).
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

namespace palmier::services {

class ToolRegistry;    // services/ToolRegistry.hpp — the shared tool surface.
class ToolSchema;      // services/ToolSchema.hpp — the one argument declaration.
class ProjectSession;  // services/ProjectSession.hpp — the current project.

// ---------------------------------------------------------------------------
// InvocationSource
// ---------------------------------------------------------------------------

/// Which surface issued a tool invocation. Recorded for logging only: the
/// execution policy is identical for all three, which is what makes the GUI, the
/// MCP endpoint and the in-app agent one path (Requirements 1.7, 9.4, 11.5).
enum class InvocationSource { Gui, Mcp, Agent };

/// A stable lowercase name for `source` ("gui", "mcp", "agent"), for log records.
[[nodiscard]] std::string_view invocationSourceName(InvocationSource source) noexcept;

// ---------------------------------------------------------------------------
// McpToolExecutor
// ---------------------------------------------------------------------------

/// Runs editor tools from a ToolRegistry against a ProjectSession under the MCP
/// execution policy (Requirements 7.4-7.7, 7.10). Transport-agnostic: the HTTP
/// server (task 15.2) and the in-app agent (task 16.x) both call into it.
class McpToolExecutor {
public:
    /// Monotonic clock source. Injectable so tests can drive the timeout policy
    /// deterministically without real waiting; defaults to std::steady_clock.
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    /// Records one finished invocation. Called with the issuing surface, the tool
    /// name, whether it succeeded and how long it took, AFTER the policy has been
    /// applied. Purely observational — it cannot change the outcome.
    using InvocationLog = std::function<void(InvocationSource source, std::string_view tool,
                                            bool succeeded,
                                            std::chrono::milliseconds elapsed)>;

    /// Tuning knobs. `timeBudget` is the per-invocation completion budget
    /// (Requirements 7.4/7.7/9.16; default 60 seconds). `clock`, when set,
    /// overrides the monotonic clock used to measure a tool's elapsed time.
    /// `invocationLog`, when set, receives one record per completed invocation.
    struct Options {
        std::chrono::milliseconds timeBudget = std::chrono::seconds(60);
        Clock                     clock;  // empty -> std::steady_clock::now
        InvocationLog             invocationLog;  // empty -> nothing is recorded
    };

    /// Construct an executor over `registry` and `session`.
    ///
    /// `session` holds the project the tools mutate and may be null to model "no
    /// project open" (Requirement 7.10): while null, every tool call returns the
    /// no-project error and the registry is never invoked. When non-null it MUST
    /// be the same session the registry's handlers were built against (see
    /// buildDefaultToolRegistry), so the observed/rolled-back mutations line up.
    /// `registry` must outlive the executor.
    ///
    /// The two-argument form uses the default Options (a 60-second budget and the
    /// std::steady_clock). (The overload — rather than a defaulted Options
    /// argument — sidesteps the "default member initializer required before the
    /// end of its enclosing class" rule for the nested Options aggregate.)
    McpToolExecutor(const ToolRegistry& registry, ProjectSession* session);
    McpToolExecutor(const ToolRegistry& registry, ProjectSession* session,
                    Options options);

    /// Point the executor at a different session (or null for "no project open").
    /// Note that a project *load* does not need this: the session keeps one engine
    /// across `project.open` (design.md D1).
    void setSession(ProjectSession* session) noexcept { session_ = session; }

    /// The bound session, or nullptr when none is bound.
    [[nodiscard]] ProjectSession* session() const noexcept { return session_; }

    /// True iff a project is currently open (a session is bound).
    [[nodiscard]] bool hasProject() const noexcept { return session_ != nullptr; }

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
    ///
    /// `source` names the surface that issued the call and is used only for the
    /// invocation log: every source runs the identical policy (Requirements 1.7,
    /// 9.4, 11.5).
    [[nodiscard]] Result<Json> executeTool(std::string_view name, const Json& input,
                                           InvocationSource source = InvocationSource::Mcp);

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
    [[nodiscard]] Json execute(const Json& request,
                               InvocationSource source = InvocationSource::Mcp);

    /// Argument validation run by executeTool before a command is created
    /// (Requirements 7.10, 9.9). It delegates to `ToolSchema::validate`, the same
    /// declaration `Tool::inputSchema()` publishes, so the advertised schema and
    /// the enforced constraints cannot drift (design.md D3, Requirement 9.12).
    /// Returns ok() when no declared constraint is violated.
    [[nodiscard]] static Result<void> validateAgainstSchema(const Json& input,
                                                            const ToolSchema& schema);

private:
    // Undo `appliedCount` most-recently-applied commands to roll the project back
    // to its pre-invocation state (used on failure/timeout aborts).
    void rollback(int appliedCount);

    const ToolRegistry*       registry_;
    ProjectSession*           session_;
    std::chrono::milliseconds budget_;
    Clock                     clock_;
    InvocationLog             invocationLog_;
};

}  // namespace palmier::services

#endif  // PALMIER_SERVICES_MCPTOOLEXECUTOR_HPP
