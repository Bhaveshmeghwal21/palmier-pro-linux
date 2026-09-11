// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/ToolRegistry.cpp — the ToolRegistry container, and the registration
// order that defines the default tool surface (task 15.1).
//
// This file used to hold the whole surface: every tool name, every shared helper and
// all 42 factories, in one anonymous namespace across 2767 lines. It now holds only
// what is genuinely about the REGISTRY — the container (add/has/find/describe/invoke)
// and the order tools are published in — while the tools themselves live one
// translation unit per subject beside it, sharing ToolRegistryInternal.hpp:
//
//   ToolVocabulary.cpp          the closed value sets the schemas publish
//   ToolSerialization.cpp       project -> JSON for the read tools
//   ToolArguments.cpp           argument extraction, ArgSpec shapes, atomic apply
//   TimelineClipTools.cpp       read, and the clip structural edits
//   TimelineEffectTools.cpp     effects, tone curves, transitions
//   TimelineTextTools.cpp       text/title clips and caption cues
//   ProjectTools.cpp            project create/open/save/info, and settings
//   MediaTools.cpp              media import, list and tagging
//   TrackTools.cpp              tracks, and undo/redo
//   GenerationExportTools.cpp   the hook-backed tools, and export
//
// Nothing about the surface changed: the same 42 tools are registered in the same
// order with the same schemas and handlers, so `tools/list` and docs/TOOLS.md are
// untouched by the split.
//
// Each structural-edit tool still parses its JSON arguments, constructs the same
// concrete EditCommand the UI uses, and applies it atomically through
// TimelineEngine::apply, so the UI, the MCP server and the in-app agent all drive one
// undoable/observable editing path (design.md Property P4; Requirements 7.4, 7.8).
// Each tool's accepted arguments are still declared exactly once as a `ToolSchema`
// (design.md D3, "Schema/handler agreement"), rendered into the published
// `inputSchema` and enforced by the pre-execution validator (Requirements 9.3, 9.9,
// 9.12).

#include "services/ToolRegistry.hpp"

#include <string>
#include <string_view>
#include <utility>

#include "core/Error.hpp"
#include "services/ProjectSession.hpp"
#include "services/ToolRegistryInternal.hpp"

namespace palmier::services {

// ---------------------------------------------------------------------------
// ToolRegistry
// ---------------------------------------------------------------------------

void ToolRegistry::add(Tool tool) {
    for (Tool& existing : tools_) {
        if (existing.name == tool.name) {
            existing = std::move(tool);
            return;
        }
    }
    tools_.push_back(std::move(tool));
}

bool ToolRegistry::has(std::string_view name) const { return find(name) != nullptr; }

const Tool* ToolRegistry::find(std::string_view name) const {
    for (const Tool& t : tools_) {
        if (t.name == name) return &t;
    }
    return nullptr;
}

Json ToolRegistry::describe() const {
    Json out = Json::array();
    for (const Tool& t : tools_) {
        Json entry = Json::object();
        entry.set("name", t.name);
        entry.set("description", t.description);
        entry.set("inputSchema", t.inputSchema());
        out.push_back(std::move(entry));
    }
    return out;
}

Result<Json> ToolRegistry::invoke(std::string_view name, const Json& input) const {
    const Tool* tool = find(name);
    if (tool == nullptr) {
        return err<Json>(notFound(std::string("unknown tool '") + std::string(name) + "'"));
    }
    if (!tool->handler) {
        return err<Json>(makeError(ErrorCode::Internal,
                                   std::string("tool '") + std::string(name) + "' has no handler"));
    }
    return tool->handler(input);
}

// ---------------------------------------------------------------------------
// Default surface
// ---------------------------------------------------------------------------

ToolRegistry buildDefaultToolRegistry(ProjectSession& session, ToolRegistryHooks hooks) {
    return buildDefaultToolRegistry(&session, std::move(hooks));
}

ToolRegistry buildDefaultToolRegistry(ProjectSession* session, ToolRegistryHooks hooks) {
    ToolRegistry registry;
    // Registration order is the order `tools/list` publishes, so it reads as the
    // life cycle of a project: read, then the session tools, the media library, the
    // track tools, the clip edits, generation and finally export.
    registry.add(detail::makeReadTool(session));
    registry.add(detail::makeProjectCreateTool(session, std::move(hooks.createProject)));
    registry.add(detail::makeProjectOpenTool(session, std::move(hooks.openProject)));
    registry.add(detail::makeProjectSaveTool(session, std::move(hooks.saveProject)));
    registry.add(detail::makeProjectInfoTool(session, std::move(hooks.projectInfo)));
    // Mutable project settings sits right after the read it complements (task 10.1).
    registry.add(detail::makeSetProjectSettingsTool(session));
    registry.add(detail::makeMediaImportTool(session, std::move(hooks.importMedia)));
    registry.add(detail::makeMediaListTool(session, std::move(hooks.listMedia)));
    registry.add(detail::makeMediaSetTagsTool(session));
    registry.add(detail::makeAddTrackTool(session));
    registry.add(detail::makeRemoveTrackTool(session));
    registry.add(detail::makeSetTrackMutedTool(session));
    registry.add(detail::makeAddClipTool(session));
    registry.add(detail::makeDeleteClipTool(session));
    registry.add(detail::makeMoveClipTool(session));
    registry.add(detail::makeSetClipPropertiesTool(session));
    registry.add(detail::makeTrimClipTool(session));
    registry.add(detail::makeSplitClipTool(session));
    registry.add(detail::makeReorderClipsTool(session));
    // Ripple editing sits with the clip edits it is a variant of (task 8.2).
    registry.add(detail::makeRippleDeleteTool(session));
    registry.add(detail::makeRippleTrimTool(session));
    registry.add(detail::makeCloseGapTool(session));
    registry.add(detail::makeAddEffectTool(session));
    // Effect lifecycle sits right after the append it completes (task 9.2).
    registry.add(detail::makeRemoveEffectTool(session));
    registry.add(detail::makeReorderEffectsTool(session));
    registry.add(detail::makeSetEffectParameterTool(session));
    registry.add(detail::makeEditCurvePointTool(session));
    registry.add(detail::makeSetEffectResourceTool(session));
    registry.add(detail::makeAddTransitionTool(session));
    // Text and titles sits right after transitions (usable-editor task 12.2).
    registry.add(detail::makeAddTextClipTool(session));
    registry.add(detail::makeSetTextContentTool(session));
    registry.add(detail::makeSetTextStyleTool(session));
    // Captions and transcription sits right after text and titles
    // (usable-editor task 13.2).
    registry.add(detail::makeAddCaptionCueTool(session));
    registry.add(detail::makeSetCaptionTextTool(session));
    registry.add(detail::makeRetimeCaptionCueTool(session));
    registry.add(detail::makeRemoveCaptionCueTool(session));
    registry.add(detail::makeUndoTool(session));
    registry.add(detail::makeRedoTool(session));
    registry.add(detail::makeGenerateTool(session, std::move(hooks.generate)));
    registry.add(detail::makeListModelsTool(session, std::move(hooks.listModels)));
    registry.add(
        detail::makeTranscribeToCaptionsTool(session, std::move(hooks.transcribeToCaptions)));
    registry.add(detail::makeCaptureFrameTool(session, std::move(hooks.captureFrame)));
    registry.add(detail::makeExportTool(session, std::move(hooks.exportTimeline)));
    return registry;
}

}  // namespace palmier::services
