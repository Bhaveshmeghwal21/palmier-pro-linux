// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/ToolRegistry.hpp — the editor's tool surface, shared by the MCP
// server and the in-app agent (task 15.1).
//
// design.md "Component 2: MCP Server" describes an McpServer constructed with a
// TimelineEngine and a ToolRegistry, where each tool carries a name, a JSON input
// schema, and a handler that "translates to EditCommand". Requirement 7.8 further
// requires that the MCP server expose the *same* set of tools available to the
// Agent_Chat, and Requirement 7.4 that an invoked tool executes on the current
// project. Property P4 (design.md) makes this explicit: an edit issued via the
// UI, an MCP tool call, or the in-app agent must produce the same resulting
// project state.
//
// This ToolRegistry is the single, transport-agnostic definition of that surface.
// It deliberately knows nothing about HTTP (task 15.2 binds it to a socket) or
// about the agent's chat loop (task 16.1 drives it from parsed intents): it is
// just a named collection of {schema, handler} tools over a `TimelineEngine`.
// Both callers construct one via `buildDefaultToolRegistry` so they share
// identical handlers — the mechanism that guarantees P4 equivalence.
//
// Tool categories in the default surface:
//
//   * Read              — `timeline.read` returns the current project state
//                         (tracks, clips, effects, transitions) as JSON.
//   * Structural edits  — `timeline.add_clip` / `delete_clip` / `move_clip` /
//                         `trim_clip` / `split_clip` / `reorder_clips` /
//                         `add_effect` / `add_transition`. Each parses its JSON
//                         arguments, constructs the SAME concrete EditCommand the
//                         UI uses (AddClipCommand, DeleteClipCommand, ...), and
//                         applies it through TimelineEngine::apply — the one
//                         atomic, undoable, observable path. `add_transition`,
//                         which has no dedicated core command, applies an
//                         equivalent atomic edit through the same engine path.
//   * Generation        — `generation.generate` triggers in-timeline generative
//                         media (Requirement 6). Generation needs the hosted
//                         backend + placement coordinator (task 14.2), which pull
//                         in dependencies this layer must not hard-wire, so it is
//                         supplied as an injectable hook by the composition root
//                         (task 21.1). Absent a hook the tool reports Unsupported.
//   * Export            — `timeline.export` renders the timeline to a file
//                         (Requirement 11). Likewise supplied as an injectable
//                         hook (the Export Engine is task 10.x).
//
// A handler returns Result<Json>: an Error is surfaced to the caller verbatim
// (the MCP layer maps it to an error response and, per Requirement 7.6, rolls
// back), and a Json payload is the tool's success result. Handlers themselves do
// not implement the timeout/rollback policy of Requirement 7.5/7.6/7.7 — that is
// the executor's job in task 15.3 — they simply map arguments to the engine and
// report the outcome.

#ifndef PALMIER_SERVICES_TOOLREGISTRY_HPP
#define PALMIER_SERVICES_TOOLREGISTRY_HPP

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "core/Result.hpp"
#include "services/Json.hpp"

namespace palmier {
class TimelineEngine;  // core/TimelineEngine.hpp — the mutation target.
}  // namespace palmier

namespace palmier::services {

// ---------------------------------------------------------------------------
// Tool descriptor
// ---------------------------------------------------------------------------

/// One exposed editor tool: an MCP-style name, a human-readable description, a
/// JSON-Schema object describing its accepted input, and the handler that runs
/// it. The handler receives already-parsed JSON arguments and returns either a
/// success payload or an Error.
struct Tool {
    using Handler = std::function<Result<Json>(const Json& input)>;

    std::string name;         ///< e.g. "timeline.add_clip".
    std::string description;  ///< One-line human description.
    Json        inputSchema;  ///< JSON-Schema (draft-07 style "object" schema).
    Handler     handler;      ///< Argument-parsing + engine-mapping implementation.
};

// ---------------------------------------------------------------------------
// Injectable hooks for the non-EditCommand tools
// ---------------------------------------------------------------------------

/// Seams for the tools that cannot be realized purely against the TimelineEngine
/// (they need the generative backend / export engine). The composition root
/// (task 21.1) wires these; when a hook is empty the corresponding tool is still
/// advertised (so the surface is identical across configurations) but returns an
/// Unsupported error when invoked.
struct ToolRegistryHooks {
    /// Handles `generation.generate` (Requirement 6): validate + gate + generate
    /// + place. Typically an adapter over GenerativeMediaCoordinator (task 14.2).
    Tool::Handler generate;

    /// Handles `timeline.export` (Requirement 11): render the timeline to a file.
    /// Typically an adapter over the Export Engine (task 10.x).
    Tool::Handler exportTimeline;
};

// ---------------------------------------------------------------------------
// ToolRegistry
// ---------------------------------------------------------------------------

/// A named collection of editor tools. Transport-agnostic: the MCP HTTP server
/// (task 15.2/15.3) and the in-app agent orchestrator (task 16.1) both hold one
/// and dispatch calls into it, guaranteeing identical behavior (Requirement 7.8,
/// Property P4).
class ToolRegistry {
public:
    ToolRegistry() = default;

    /// Register (or replace, by name) a tool. A later registration with an
    /// existing name overwrites the earlier one.
    void add(Tool tool);

    /// True iff a tool with `name` is registered.
    [[nodiscard]] bool has(std::string_view name) const;

    /// The tool named `name`, or nullptr when none is registered.
    [[nodiscard]] const Tool* find(std::string_view name) const;

    /// The registered tools in registration order.
    [[nodiscard]] const std::vector<Tool>& tools() const noexcept { return tools_; }

    /// Number of registered tools.
    [[nodiscard]] std::size_t size() const noexcept { return tools_.size(); }

    /// The MCP `tools/list` payload: an array of `{name, description, inputSchema}`
    /// objects, one per tool, in registration order.
    [[nodiscard]] Json describe() const;

    /// Invoke the tool `name` with `input`. An unknown tool name yields a
    /// NotFound Error (Requirement 7.5 — the executor maps this to "unknown
    /// tool"); otherwise the tool's handler result is returned verbatim.
    [[nodiscard]] Result<Json> invoke(std::string_view name, const Json& input) const;

private:
    std::vector<Tool> tools_;
};

// ---------------------------------------------------------------------------
// Default surface
// ---------------------------------------------------------------------------

/// Build the standard editor tool surface bound to `engine`. The structural-edit
/// and read tools are wired directly to the TimelineEngine (mapping to the same
/// EditCommands the UI uses); `generation.generate` and `timeline.export` are
/// wired to `hooks` when supplied. `engine` must outlive the returned registry
/// (its handlers capture it by reference).
[[nodiscard]] ToolRegistry buildDefaultToolRegistry(TimelineEngine& engine,
                                                    ToolRegistryHooks hooks = {});

}  // namespace palmier::services

#endif  // PALMIER_SERVICES_TOOLREGISTRY_HPP
