// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/ProjectTools.cpp — the project lifecycle tools — create, open, save, info — and mutable
// project settings.
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
// Session tools (task 4.3) — project.create / open / save / info
//
// These four are NOT EditCommands and are NOT undoable (design.md D1): each
// either commits a whole new project through ProjectSession (create/open) or only
// reads/persists the current one (info/save). ProjectSession builds the complete
// Project value locally and commits with TimelineEngine::reset only on full
// success, so a rejected create or open leaves the previous project, its document
// path, its modified flag and its undo history untouched (Requirements 3.9, 4.10).
//
// Each tool has a session-backed default implementation and accepts an optional
// hook override, so the headless sequence of Requirement 3.6 runs against a bare
// session while the GUI can still interpose its destination prompt on a save.
// ---------------------------------------------------------------------------

Tool makeProjectCreateTool(ProjectSession* session, Tool::Handler hook) {
    Tool t;
    t.name = kProjectCreate;
    t.description = "Create a new project with the given name, frame rate, canvas and "
                    "colour space, and make it the current project.";
    // Every bound Requirement 3.8 names is declared here, so an out-of-range
    // argument is rejected by the published schema before ProjectSession is asked:
    // the name length (1..128 characters), the frame rate (1..240 fps), the canvas
    // (16..7680 x 16..4320 pixels) and the colour space (the closed set the domain
    // core exposes). ProjectSession re-checks each of them — it is reachable from
    // the GUI and the agent too — so the two agree by construction and the tool's
    // error names the rejected argument either way.
    t.schema
        .arg(ArgSpec{.name = "name",
                     .kind = JsonKind::String,
                     .required = true,
                     .description = "Project name (1..128 characters).",
                     .minLength = kMinProjectNameLength,
                     .maxLength = kMaxProjectNameLength})
        .arg(ArgSpec{.name = "fps",
                     .kind = JsonKind::Number,
                     .required = true,
                     .description = "Timeline frame rate in frames per second (1..240).",
                     .minNum = static_cast<double>(kMinFramesPerSecond),
                     .maxNum = static_cast<double>(kMaxFramesPerSecond)})
        .arg(ArgSpec{.name = "width",
                     .kind = JsonKind::Integer,
                     .required = true,
                     .description = "Canvas width in pixels (16..7680).",
                     .minInt = static_cast<std::int64_t>(kMinCanvasWidth),
                     .maxInt = static_cast<std::int64_t>(kMaxCanvasWidth)})
        .arg(ArgSpec{.name = "height",
                     .kind = JsonKind::Integer,
                     .required = true,
                     .description = "Canvas height in pixels (16..4320).",
                     .minInt = static_cast<std::int64_t>(kMinCanvasHeight),
                     .maxInt = static_cast<std::int64_t>(kMaxCanvasHeight)})
        .arg(ArgSpec{.name = "colorSpace",
                     .kind = JsonKind::String,
                     .description = "Working colour space; defaults to Rec.709.",
                     .enumValues = colorSpaceValues()});
    t.handler = [session, hook = std::move(hook)](const Json& in) -> Result<Json> {
        if (hook) return hook(in);
        // Requirement 3.5 exempts this tool, so there is no "no project is open"
        // path: with neither a session nor a hook the capability is simply absent.
        if (session == nullptr) {
            return err<Json>(
                unsupported("project.create is not available: no project session is bound"));
        }

        Result<std::string> name = requireString(in, "name");
        if (name.isError()) return err<Json>(std::move(name).error());
        Result<double> fps = requireNumber(in, "fps");
        if (fps.isError()) return err<Json>(std::move(fps).error());
        Result<std::int64_t> width = requireInt(in, "width");
        if (width.isError()) return err<Json>(std::move(width).error());
        Result<std::int64_t> height = requireInt(in, "height");
        if (height.isError()) return err<Json>(std::move(height).error());

        // The declared frame-rate bound again, for the same reason as the canvas
        // below: reached directly, an astronomically large number would otherwise
        // be converted to a rational through an out-of-range integer cast.
        if (!(fps.value() >= static_cast<double>(kMinFramesPerSecond)) ||
            fps.value() > static_cast<double>(kMaxFramesPerSecond)) {
            return err<Json>(outOfRange("rejected argument 'fps': the frame rate must lie "
                                        "between " +
                                        std::to_string(kMinFramesPerSecond) + " and " +
                                        std::to_string(kMaxFramesPerSecond) +
                                        " frames per second"));
        }

        const std::string colorSpaceName =
            in.stringOr("colorSpace", std::string(toStringView(kDefaultProjectColorSpace)));
        const std::optional<ColorSpace> colorSpace = parseColorSpace(colorSpaceName);
        if (!colorSpace) {
            return err<Json>(invalidArgument("rejected argument 'colorSpace': '" + colorSpaceName +
                                             "' is not a colour space the domain core exposes"));
        }
        // The declared bounds again, because a handler is reachable without the
        // schema (the registry can be invoked directly) and the canvas fields are
        // unsigned: an unchecked cast of an out-of-range integer would wrap into a
        // plausible-looking canvas instead of being rejected (Requirement 3.8).
        if (width.value() < static_cast<std::int64_t>(kMinCanvasWidth) ||
            width.value() > static_cast<std::int64_t>(kMaxCanvasWidth)) {
            return err<Json>(
                outOfRange("rejected argument 'width': " + std::to_string(width.value()) +
                           " is outside " + std::to_string(kMinCanvasWidth) + "-" +
                           std::to_string(kMaxCanvasWidth) + " pixels"));
        }
        if (height.value() < static_cast<std::int64_t>(kMinCanvasHeight) ||
            height.value() > static_cast<std::int64_t>(kMaxCanvasHeight)) {
            return err<Json>(
                outOfRange("rejected argument 'height': " + std::to_string(height.value()) +
                           " is outside " + std::to_string(kMinCanvasHeight) + "-" +
                           std::to_string(kMaxCanvasHeight) + " pixels"));
        }

        const FrameRate rate = frameRateFromFps(fps.value());
        const Resolution canvas{static_cast<std::uint32_t>(width.value()),
                                static_cast<std::uint32_t>(height.value())};

        Result<Uuid> created = session->createProject(name.value(), rate, canvas, *colorSpace);
        if (created.isError()) return err<Json>(std::move(created).error());

        Json out = Json::object();
        out.set("projectId", created.value().toString());
        out.set("name", name.value());
        out.set("fps", serializeFrameRate(rate));
        out.set("canvas", serializeCanvas(canvas));
        out.set("colorSpace", std::string(toStringView(*colorSpace)));
        out.set("modified", session->modified());
        out.set("documentPath", serializeDocumentPath(session->documentPath()));
        return out;
    };
    return t;
}

Tool makeProjectOpenTool(ProjectSession* session, Tool::Handler hook) {
    Tool t;
    t.name = kProjectOpen;
    t.description = "Open a .palmier document and make it the current project.";
    t.schema.arg(ArgSpec{.name = "path",
                         .kind = JsonKind::String,
                         .required = true,
                         .description = "Filesystem path of the .palmier document to open.",
                         .minLength = kMinPathLength,
                         .maxLength = kMaxPathLength});
    t.handler = [session, hook = std::move(hook)](const Json& in) -> Result<Json> {
        if (hook) return hook(in);
        if (session == nullptr) {
            return err<Json>(
                unsupported("project.open is not available: no project session is bound"));
        }

        Result<std::string> path = requireString(in, "path");
        if (path.isError()) return err<Json>(std::move(path).error());

        // ProjectSession::openProject is all-or-nothing: a missing, unreadable,
        // malformed or unsupported-schema document is reported with the path and the
        // reason named, and the project that was current stays current, keeping its
        // document path, modified flag and undo history (Requirements 3.9, 4.10).
        Result<ProjectSession::Status> opened =
            session->openProject(std::filesystem::path(path.value()));
        if (opened.isError()) return err<Json>(std::move(opened).error());

        const ProjectSession::Status& status = opened.value();
        Json out = Json::object();
        out.set("projectId", status.projectId.toString());
        out.set("name", status.name);
        out.set("trackCount", static_cast<std::int64_t>(status.trackCount));
        out.set("clipCount", static_cast<std::int64_t>(status.clipCount));
        out.set("documentPath", serializeDocumentPath(status.documentPath));
        out.set("modified", status.modified);
        return out;
    };
    return t;
}

Tool makeProjectSaveTool(ProjectSession* session, Tool::Handler hook) {
    Tool t;
    t.name = kProjectSave;
    t.description = "Write the current project to a .palmier document (defaults to its "
                    "recorded document path).";
    t.schema.arg(ArgSpec{.name = "path",
                         .kind = JsonKind::String,
                         .description = "Destination path; omitted means the project's "
                                        "recorded document path.",
                         .minLength = kMinPathLength,
                         .maxLength = kMaxPathLength});
    t.handler = sessionGuarded(
        session, kProjectSave,
        hook ? std::move(hook) : Tool::Handler([session](const Json& in) -> Result<Json> {
            std::filesystem::path destination;
            if (const Json* requested = in.find("path");
                requested != nullptr && requested->isString()) {
                destination = std::filesystem::path(requested->asString());
            } else if (session->documentPath().has_value()) {
                destination = *session->documentPath();
            } else {
                return err<Json>(failedPrecondition(
                    "project.save needs a destination: the project has no recorded "
                    "document path, so 'path' is required"));
            }

            // ProjectSession writes off the caller's thread (design.md D6,
            // upstream PR 403) and applies the revision guard when the
            // completion is pumped. A tool call is synchronous by contract —
            // its result reports the bytes written — so this handler starts
            // the save and then awaits exactly its own completion. The GUI
            // does not take this path: it installs a `saveProject` hook that
            // returns as soon as the write has been *started* and reports
            // completion through the status bar instead.
            auto captured = std::make_shared<std::optional<ProjectSession::SaveCompletionInfo>>();
            if (Result<void> started = session->requestSave(
                    destination,
                    [captured](const ProjectSession::SaveCompletionInfo& info) {
                        *captured = info;
                    });
                started.isError()) {
                return err<Json>(std::move(started).error());
            }
            while (!captured->has_value()) {
                if (session->awaitSaveCompletions() == 0 && !captured->has_value()) {
                    return err<Json>(makeError(ErrorCode::Internal,
                                               "project.save: the save completion "
                                               "was never delivered"));
                }
            }

            const ProjectSession::SaveCompletionInfo& info = **captured;
            if (!info.succeeded) return err<Json>(info.error);

            Json out = Json::object();
            out.set("documentPath", info.path.string());
            out.set("bytesWritten", static_cast<std::int64_t>(info.bytesWritten));
            // Normally false; true only when the project was edited WHILE the
            // write was running, in which case the written document is still
            // valid but the session is legitimately still dirty (design.md D6).
            out.set("modified", info.stillModified);
            return out;
        }));
    return t;
}

Tool makeProjectInfoTool(ProjectSession* session, Tool::Handler hook) {
    Tool t;
    t.name = kProjectInfo;
    t.description = "Report the current project's identity, settings, counts, modified "
                    "state, document path and undo depth.";
    // No arguments.
    t.handler = sessionGuarded(
        session, kProjectInfo,
        hook ? std::move(hook) : Tool::Handler([session](const Json&) -> Result<Json> {
            const Project project = session->engine().snapshot();
            const ProjectSession::Status status = session->status();

            Json out = Json::object();
            out.set("projectId", project.id.toString());
            out.set("name", project.name);
            out.set("fps", serializeFrameRate(project.timelineFps));
            out.set("canvas", serializeCanvas(project.canvas));
            out.set("colorSpace", std::string(toStringView(project.colorSpace)));
            out.set("trackCount", static_cast<std::int64_t>(project.tracks.size()));
            out.set("clipCount", static_cast<std::int64_t>(countClips(project)));
            // The media library is the authoritative asset catalogue of the
            // current project (MediaImportService registers into it and
            // ProjectSession rebuilds it from an opened document), so
            // `project.info` and `media.list` always agree.
            out.set("assetCount", static_cast<std::int64_t>(session->mediaLibrary().assetCount()));
            out.set("durationNs",
                    static_cast<std::int64_t>(timelineDuration(project).nanoseconds()));
            out.set("modified", status.modified);
            out.set("documentPath", serializeDocumentPath(status.documentPath));
            out.set("undoDepth", static_cast<std::int64_t>(session->engine().undoDepth()));
            return out;
        }));
    return t;
}

// ---------------------------------------------------------------------------
// project.set_settings (task 10.1; Requirement 7) — change the current
// project's frame rate, canvas and/or colour space after creation, accepting
// the identical ranges project.create accepts. Every argument is optional: a
// field left out is left exactly as it was, and any combination that IS
// supplied changes in one undoable edit (Requirement 7.4).
// ---------------------------------------------------------------------------

Tool makeSetProjectSettingsTool(ProjectSession* session) {
    Tool t;
    t.name = kSetProjectSettings;
    t.description = "Change the current project's frame rate, canvas and/or colour space.";
    t.schema
        .arg(ArgSpec{.name = "fps",
                     .kind = JsonKind::Number,
                     .description = "New timeline frame rate in frames per second (" +
                                    std::to_string(kMinFramesPerSecond) + ".." +
                                    std::to_string(kMaxFramesPerSecond) + ").",
                     .minNum = static_cast<double>(kMinFramesPerSecond),
                     .maxNum = static_cast<double>(kMaxFramesPerSecond)})
        .arg(ArgSpec{.name = "width",
                     .kind = JsonKind::Integer,
                     .description = "New canvas width in pixels (" +
                                    std::to_string(kMinCanvasWidth) + ".." +
                                    std::to_string(kMaxCanvasWidth) +
                                    "). Must be given "
                                    "together with height.",
                     .minInt = static_cast<std::int64_t>(kMinCanvasWidth),
                     .maxInt = static_cast<std::int64_t>(kMaxCanvasWidth)})
        .arg(ArgSpec{.name = "height",
                     .kind = JsonKind::Integer,
                     .description = "New canvas height in pixels (" +
                                    std::to_string(kMinCanvasHeight) + ".." +
                                    std::to_string(kMaxCanvasHeight) +
                                    "). Must be given "
                                    "together with width.",
                     .minInt = static_cast<std::int64_t>(kMinCanvasHeight),
                     .maxInt = static_cast<std::int64_t>(kMaxCanvasHeight)})
        .arg(ArgSpec{.name = "colorSpace",
                     .kind = JsonKind::String,
                     .description = "New working colour space.",
                     .enumValues = colorSpaceValues()});
    t.handler = [session](const Json& in) -> Result<Json> {
        if (session == nullptr) return err<Json>(noProjectOpen(kSetProjectSettings));
        TimelineEngine& engine = session->engine();

        const bool hasWidth = in.find("width") != nullptr;
        const bool hasHeight = in.find("height") != nullptr;
        if (hasWidth != hasHeight) {
            return err<Json>(invalidArgument(
                "project.set_settings: 'width' and 'height' must be given together"));
        }
        const bool hasFps = in.find("fps") != nullptr;
        const bool hasColorSpace = in.find("colorSpace") != nullptr;
        if (!hasFps && !hasWidth && !hasColorSpace) {
            return err<Json>(invalidArgument(
                "project.set_settings: at least one of fps, width/height or colorSpace "
                "must be given"));
        }

        std::optional<FrameRate> fps;
        if (hasFps) {
            Result<double> value = requireNumber(in, "fps");
            if (value.isError()) return err<Json>(std::move(value).error());
            // The declared bound again, for the same reason project.create checks it
            // directly: reached without the schema, an astronomically large number
            // would otherwise be converted to a rational through an out-of-range cast.
            if (!(value.value() >= static_cast<double>(kMinFramesPerSecond)) ||
                value.value() > static_cast<double>(kMaxFramesPerSecond)) {
                return err<Json>(
                    outOfRange("rejected argument 'fps': the frame rate must lie between " +
                               std::to_string(kMinFramesPerSecond) + " and " +
                               std::to_string(kMaxFramesPerSecond) + " frames per second"));
            }
            fps = frameRateFromFps(value.value());
        }

        std::optional<Resolution> canvas;
        if (hasWidth) {
            Result<std::int64_t> width = requireInt(in, "width");
            if (width.isError()) return err<Json>(std::move(width).error());
            Result<std::int64_t> height = requireInt(in, "height");
            if (height.isError()) return err<Json>(std::move(height).error());
            if (width.value() < static_cast<std::int64_t>(kMinCanvasWidth) ||
                width.value() > static_cast<std::int64_t>(kMaxCanvasWidth)) {
                return err<Json>(
                    outOfRange("rejected argument 'width': " + std::to_string(width.value()) +
                               " is outside " + std::to_string(kMinCanvasWidth) + "-" +
                               std::to_string(kMaxCanvasWidth) + " pixels"));
            }
            if (height.value() < static_cast<std::int64_t>(kMinCanvasHeight) ||
                height.value() > static_cast<std::int64_t>(kMaxCanvasHeight)) {
                return err<Json>(
                    outOfRange("rejected argument 'height': " + std::to_string(height.value()) +
                               " is outside " + std::to_string(kMinCanvasHeight) + "-" +
                               std::to_string(kMaxCanvasHeight) + " pixels"));
            }
            canvas = Resolution{static_cast<std::uint32_t>(width.value()),
                                static_cast<std::uint32_t>(height.value())};
        }

        std::optional<ColorSpace> colorSpace;
        if (hasColorSpace) {
            Result<std::string> colorSpaceStr = requireString(in, "colorSpace");
            if (colorSpaceStr.isError()) return err<Json>(std::move(colorSpaceStr).error());
            colorSpace = parseColorSpace(colorSpaceStr.value());
            if (!colorSpace) {
                return err<Json>(
                    invalidArgument("rejected argument 'colorSpace': '" + colorSpaceStr.value() +
                                    "' is not a colour space the domain core exposes"));
            }
        }

        Json out = Json::object();
        if (fps) out.set("fps", serializeFrameRate(*fps));
        if (canvas) out.set("canvas", serializeCanvas(*canvas));
        if (colorSpace) out.set("colorSpace", std::string(toStringView(*colorSpace)));
        return applyCommand(engine,
                            std::make_unique<SetProjectSettingsCommand>(fps, canvas, colorSpace),
                            std::move(out));
    };
    return t;
}

}  // namespace palmier::services::detail
