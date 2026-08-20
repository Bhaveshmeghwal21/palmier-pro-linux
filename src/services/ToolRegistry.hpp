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
// just a named collection of {schema, handler} tools over a `ProjectSession`.
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

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "core/Result.hpp"
#include "services/Json.hpp"
#include "services/MediaImportService.hpp"
#include "services/ToolSchema.hpp"

namespace palmier::services {

class ProjectSession;  // services/ProjectSession.hpp — the current project.

// ---------------------------------------------------------------------------
// Tool descriptor
// ---------------------------------------------------------------------------

/// One exposed editor tool: an MCP-style name, a human-readable description, the
/// single `ToolSchema` declaring its accepted arguments, and the handler that
/// runs it. The handler receives already-parsed JSON arguments and returns either
/// a success payload or an Error.
///
/// Per design.md decision D3 ("Schema/handler agreement", Requirement 9.12) the
/// accepted arguments are declared exactly once, in `schema`: the JSON Schema
/// `tools/list` publishes is *rendered* from it (`inputSchema()`) and the
/// validator the executor runs before any command is created enforces the same
/// constraint set (`schema.validate()`), so the advertised surface and the
/// runtime behaviour cannot drift.
struct Tool {
    using Handler = std::function<Result<Json>(const Json& input)>;

    std::string name;         ///< e.g. "timeline.add_clip".
    std::string description;  ///< One-line human description.
    ToolSchema  schema;       ///< The one declaration of the accepted arguments.
    Handler     handler;      ///< Argument-parsing + engine-mapping implementation.

    /// The published JSON Schema (draft-07 style "object" schema), rendered from
    /// `schema`. Never stored separately: there is nothing to keep in step.
    [[nodiscard]] Json inputSchema() const { return schema.toJsonSchema(); }
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
    /// `media.import`'s one operation (task 4.4; Requirement 2.2): probe, validate
    /// and register the file at the given path as exactly one asset of the current
    /// project's media library, reporting the registered asset.
    ///
    /// It is declared as the operation rather than as a whole `Tool::Handler` so
    /// that the *result shape* Requirement 2.2 specifies — the asset id, the
    /// resolved source path, the container format, the duration, the resolution and
    /// frame rate present only for a decodable video stream, and the duplicate flag
    /// — is rendered in exactly one place (this registry) no matter who supplies
    /// the operation. The composition root's hook is a one-line adapter over
    /// `MediaImportService::import`; absent the hook `media.import` is still
    /// advertised (so the tool surface is identical across configurations) and
    /// reports Unsupported when invoked.
    using MediaImportHook =
        std::function<Result<ImportedAsset>(const std::filesystem::path& path)>;

    /// Handles `generation.generate` (Requirement 6): validate + gate + generate
    /// + place. Typically an adapter over GenerativeMediaCoordinator (task 14.2).
    Tool::Handler generate;

    /// Handles `generation.list_models` (usable-editor Phase 2 task 7; PR 406):
    /// list the generation model catalog, grouped by provider. Typically an
    /// adapter over `services::GenerationModelCatalog::listModels()`. Empty ⇒
    /// the tool reports Unsupported ("no model catalog is configured").
    Tool::Handler listModels;

    /// Handles `timeline.export` (Requirement 11): render the timeline to a file.
    /// Typically an adapter over the Export Engine (task 10.x).
    Tool::Handler exportTimeline;

    /// `media.import` (task 4.4). Empty ⇒ the tool reports Unsupported.
    MediaImportHook importMedia;

    /// The session-level tools (task 4.3) and `media.list` (task 4.4). Each of
    /// these five has a default implementation over the bound `ProjectSession`, so
    /// the headless sequence of Requirement 3.6 runs without any composition:
    /// a hook is an *override* for a surface that needs to interpose (the GUI
    /// routes File > Save through its destination prompt, for example), not a
    /// prerequisite. An empty hook keeps the session-backed default.
    Tool::Handler createProject;  ///< `project.create` (Requirements 3.2, 3.8).
    Tool::Handler openProject;    ///< `project.open`  (Requirements 3.4, 3.9).
    Tool::Handler saveProject;    ///< `project.save`  (Requirements 4.1, 4.4).
    Tool::Handler projectInfo;    ///< `project.info`  (Requirement 3.7).
    Tool::Handler listMedia;      ///< `media.list`.
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

/// Build the standard editor tool surface bound to `session`. The structural-edit
/// and read tools are wired to the session's timeline engine (mapping to the same
/// EditCommands the UI uses); `generation.generate` and `timeline.export` are
/// wired to `hooks` when supplied. `session` must outlive the returned registry
/// (its handlers capture it by reference).
///
/// Every handler resolves the engine through `session.engine()` **at invocation
/// time** rather than capturing a `TimelineEngine&` when the registry is built
/// (design.md decision D1). That is what makes a project loaded after the
/// registry was constructed observable to the tools: the session keeps one stable
/// engine object and swaps the project value inside it, so no handler, view model
/// or subscription has to be rebound when the current project changes.
[[nodiscard]] ToolRegistry buildDefaultToolRegistry(ProjectSession& session,
                                                    ToolRegistryHooks hooks = {});

/// As above, where a null `session` models "no project is current" — the state
/// `McpToolExecutor` and `MediaImportService` already represent with a null
/// session pointer (Requirement 3.5). The advertised surface is identical either
/// way: every tool is registered and publishes the same schema, but while no
/// project is current every tool OTHER than `project.create` and `project.open`
/// returns a FailedPrecondition error stating that no project is open, having
/// created no edit command and touched no state.
///
/// `project.create` and `project.open` are exempt because their job is to MAKE a
/// project current. With no session bound they can only run through the
/// `createProject` / `openProject` hooks (which own the session they create into);
/// absent both a session and a hook they report Unsupported — never "no project is
/// open" — so the exemption is observable from the outside.
[[nodiscard]] ToolRegistry buildDefaultToolRegistry(ProjectSession* session,
                                                    ToolRegistryHooks hooks = {});

}  // namespace palmier::services

#endif  // PALMIER_SERVICES_TOOLREGISTRY_HPP
