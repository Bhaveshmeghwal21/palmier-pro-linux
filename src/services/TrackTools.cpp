// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/TrackTools.cpp — the track tools and the undo/redo tools.
//
// Split out of services/ToolRegistry.cpp, which held the entire tool surface in
// one 2767-line translation unit. The internal contract these units share is
// services/ToolRegistryInternal.hpp; the public one is unchanged and remains
// services/ToolRegistry.hpp. Every definition below was moved rather than
// rewritten, so the surface this file contributes is byte-identical to before.
//
// One include, because ToolRegistryInternal.hpp states the surface's shared
// dependency set once for all of these files.

#include "services/ToolRegistryInternal.hpp"

namespace palmier::services::detail {

// ---------------------------------------------------------------------------
// Track tools (task 4.5) — timeline.add_track / timeline.remove_track
//
// Unlike the session tools above, these two ARE ordinary edits: they are applied
// as the core AddTrackCommand / RemoveTrackCommand through TimelineEngine::apply,
// so they are atomic, invariant-checked, observable and undoable through exactly
// the same path as every clip edit (Requirements 3.3, 3.10).
// ---------------------------------------------------------------------------

Tool makeAddTrackTool(ProjectSession* session) {
    Tool t;
    t.name = kAddTrack;
    t.description = "Append a video or audio track after the last existing track of that "
                    "kind.";
    // `kind` is the closed set `parseTrackKind` accepts. NOT expressible: the
    // 64-tracks-per-kind cap of Requirement 3.8 counts the project's existing
    // tracks, which the schema never sees, so AddTrackCommand remains its
    // enforcement point.
    t.schema.arg(ArgSpec{.name = "kind",
                         .kind = JsonKind::String,
                         .required = true,
                         .description = "Track kind: 'video' or 'audio'.",
                         .enumValues = trackKindValues()});
    t.handler = [session](const Json& in) -> Result<Json> {
        if (session == nullptr) return err<Json>(noProjectOpen(kAddTrack));
        TimelineEngine& engine = session->engine();
        Result<std::string> kindStr = requireString(in, "kind");
        if (kindStr.isError()) return err<Json>(std::move(kindStr).error());
        const std::optional<TrackKind> kind = parseTrackKind(kindStr.value());
        if (!kind) {
            return err<Json>(invalidArgument("rejected argument 'kind': '" + kindStr.value() +
                                             "' is not a track kind ('video' or 'audio')"));
        }

        auto cmd = std::make_unique<AddTrackCommand>(*kind);
        AddTrackCommand* raw = cmd.get();  // owned by the undo stack after a successful apply
        CommandResult result = engine.apply(std::move(cmd));
        if (result.isError()) return err<Json>(result.error());

        const Project project = engine.snapshot();
        Json out = Json::object();
        out.set("trackId", raw->trackId().toString());
        out.set("kind", std::string(trackKindName(*kind)));
        out.set("index", static_cast<std::int64_t>(raw->insertedIndex().value_or(0)));
        out.set("trackCount", static_cast<std::int64_t>(project.tracks.size()));
        out.set("status", "applied");
        return out;
    };
    return t;
}

Tool makeRemoveTrackTool(ProjectSession* session) {
    Tool t;
    t.name = kRemoveTrack;
    t.description = "Remove a track and every clip on it, preserving the order of the "
                    "remaining tracks.";
    // NOT expressible: "the track is present in the current project" is a
    // state-dependent rule (Requirement 3.8), enforced by RemoveTrackCommand.
    t.schema.arg(uuidArg("trackId", true, "UUID of the track to remove."));
    t.handler = [session](const Json& in) -> Result<Json> {
        if (session == nullptr) return err<Json>(noProjectOpen(kRemoveTrack));
        TimelineEngine& engine = session->engine();
        Result<Uuid> trackId = requireUuid(in, "trackId");
        if (trackId.isError()) return err<Json>(std::move(trackId).error());

        CommandResult result = engine.apply(std::make_unique<RemoveTrackCommand>(trackId.value()));
        if (result.isError()) return err<Json>(result.error());

        const Project project = engine.snapshot();
        Json out = Json::object();
        out.set("trackId", trackId.value().toString());
        out.set("trackCount", static_cast<std::int64_t>(project.tracks.size()));
        out.set("clipCount", static_cast<std::int64_t>(countClips(project)));
        out.set("status", "applied");
        return out;
    };
    return t;
}

// ---------------------------------------------------------------------------
// Track mute tool (task 10.1) — timeline.set_track_muted
//
// The third track edit, and like the other two an ordinary undoable edit: it is
// applied as the core SetTrackMutedCommand through TimelineEngine::apply, so a
// mute is reversed by exactly one undo like any other edit. It exists because the
// offline interpreter's documented "mute track N" / "unmute track N" phrases must
// resolve to a tool that is actually in the Tool_Surface (Requirements 11.2,
// 11.3); the phrase's 1-based track ordinal is resolved to this identifier by the
// interpreter, which is the only party that knows the current project's tracks.
// ---------------------------------------------------------------------------

Tool makeSetTrackMutedTool(ProjectSession* session) {
    Tool t;
    t.name = kSetTrackMuted;
    t.description = "Mute or unmute a track, leaving its clips and every other track "
                    "untouched.";
    // NOT expressible: "the track is present in the current project" is a
    // state-dependent rule, enforced by SetTrackMutedCommand.
    t.schema.arg(uuidArg("trackId", true, "UUID of the track to mute or unmute."))
        .arg(ArgSpec{.name = "muted",
                     .kind = JsonKind::Bool,
                     .required = true,
                     .description = "True to mute the track, false to unmute it."});
    t.handler = [session](const Json& in) -> Result<Json> {
        if (session == nullptr) return err<Json>(noProjectOpen(kSetTrackMuted));
        TimelineEngine& engine = session->engine();
        Result<Uuid> trackId = requireUuid(in, "trackId");
        if (trackId.isError()) return err<Json>(std::move(trackId).error());
        const Json* muted = in.find("muted");
        if (muted == nullptr || !muted->isBool()) {
            return err<Json>(invalidArgument("field 'muted' is required and must be a boolean"));
        }

        Json out = Json::object();
        out.set("trackId", trackId.value().toString());
        out.set("muted", muted->asBool());
        return applyCommand(
            engine, std::make_unique<SetTrackMutedCommand>(trackId.value(), muted->asBool()),
            std::move(out));
    };
    return t;
}

// ---------------------------------------------------------------------------
// History tools (task 10.1) — edit.undo / edit.redo
//
// These two are NOT EditCommands: they drive the engine's existing undo/redo
// stack, which is what makes them the tool-surface expression of Requirement 2.9.
// They exist for the same reason the mute tool does — the offline interpreter's
// documented `undo` and `redo` phrases have to resolve to real tools.
//
// An empty history is the engine's documented no-op-with-an-indication
// (Requirement 2.10), not a failure: the tool succeeds and reports `undone`/
// `redone` false plus the engine's indication, so a caller can tell "there was
// nothing to undo" from "the undo failed" without either being an error the
// executor would roll back.
// ---------------------------------------------------------------------------

Tool makeUndoTool(ProjectSession* session) {
    Tool t;
    t.name = kUndo;
    t.description = "Revert the most recently applied edit; reports a no-op when the undo "
                    "history is empty.";
    // No arguments.
    t.handler = [session](const Json&) -> Result<Json> {
        if (session == nullptr) return err<Json>(noProjectOpen(kUndo));
        TimelineEngine& engine = session->engine();
        const CommandResult result = engine.undo();
        if (result.isError()) return err<Json>(result.error());

        Json out = Json::object();
        out.set("undone", result.changed());
        out.set("indication", result.message());
        out.set("undoDepth", static_cast<std::int64_t>(engine.undoDepth()));
        out.set("redoDepth", static_cast<std::int64_t>(engine.redoDepth()));
        return out;
    };
    return t;
}

Tool makeRedoTool(ProjectSession* session) {
    Tool t;
    t.name = kRedo;
    t.description = "Re-apply the most recently undone edit; reports a no-op when the redo "
                    "history is empty.";
    // No arguments.
    t.handler = [session](const Json&) -> Result<Json> {
        if (session == nullptr) return err<Json>(noProjectOpen(kRedo));
        TimelineEngine& engine = session->engine();
        const CommandResult result = engine.redo();
        if (result.isError()) return err<Json>(result.error());

        Json out = Json::object();
        out.set("redone", result.changed());
        out.set("indication", result.message());
        out.set("undoDepth", static_cast<std::int64_t>(engine.undoDepth()));
        out.set("redoDepth", static_cast<std::int64_t>(engine.redoDepth()));
        return out;
    };
    return t;
}

}  // namespace palmier::services::detail
