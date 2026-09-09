// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/MediaTools.cpp — the media library tools: import, list and tagging.
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
// Media-library tools (task 4.4) — media.import / media.list
// ---------------------------------------------------------------------------

Tool makeMediaImportTool(ProjectSession* session, ToolRegistryHooks::MediaImportHook hook) {
    Tool t;
    t.name = kMediaImport;
    t.description = "Probe, validate and register a media file as an asset of the current "
                    "project's media library.";
    t.schema.arg(ArgSpec{.name = "path",
                         .kind = JsonKind::String,
                         .required = true,
                         .description = "Filesystem path of the media file to import.",
                         .minLength = kMinPathLength,
                         .maxLength = kMaxPathLength});
    t.handler = [session, hook = std::move(hook)](const Json& in) -> Result<Json> {
        if (session == nullptr) return err<Json>(noProjectOpen(kMediaImport));
        if (!hook) {
            return err<Json>(unsupported(
                "media.import is not available: no media import service is configured"));
        }

        Result<std::string> path = requireString(in, "path");
        if (path.isError()) return err<Json>(std::move(path).error());

        Result<ImportedAsset> imported = hook(std::filesystem::path(path.value()));
        if (imported.isError()) return err<Json>(std::move(imported).error());

        // Requirement 2.2's result: the resolution and the frame rate are reported
        // only for an asset carrying a decodable video stream, and are ABSENT (not
        // null) otherwise, so a caller distinguishes "audio only" from "unknown".
        const ImportedAsset& asset = imported.value();
        Json out = Json::object();
        out.set("assetId", asset.assetId.toString());
        out.set("sourcePath", asset.sourcePath.string());
        out.set("containerFormat", asset.containerFormat);
        out.set("durationMs", asset.durationMs);
        if (asset.resolution.has_value()) {
            out.set("width", static_cast<std::int64_t>(asset.resolution->width));
            out.set("height", static_cast<std::int64_t>(asset.resolution->height));
        }
        if (asset.frameRate.has_value()) out.set("fps", asset.frameRate->toDouble());
        out.set("hasVideo", asset.hasVideo);
        out.set("hasAudio", asset.hasAudio);
        out.set("duplicate", asset.duplicate);
        return out;
    };
    return t;
}

Tool makeMediaListTool(ProjectSession* session, Tool::Handler hook) {
    Tool t;
    t.name = kMediaList;
    t.description = "List the assets registered in the current project's media library.";
    // No arguments.
    t.handler = sessionGuarded(
        session, kMediaList,
        hook ? std::move(hook) : Tool::Handler([session](const Json&) -> Result<Json> {
            Json assets = Json::array();
            for (const MediaAssetRef& ref : session->mediaLibrary().library()) {
                Json entry = Json::object();
                entry.set("assetId", ref.assetId.toString());
                entry.set("sourcePath", ref.sourcePath);
                entry.set("displayName", std::filesystem::path(ref.sourcePath).filename().string());
                // Usable-editor tasks.md task 15; no dedicated Requirement.
                Json tags = Json::array();
                for (const std::string& tag : ref.tags) {
                    tags.push_back(Json(tag));
                }
                entry.set("tags", std::move(tags));
                assets.push_back(std::move(entry));
            }
            const std::size_t count = assets.asArray().size();
            Json out = Json::object();
            out.set("assets", std::move(assets));
            out.set("count", static_cast<std::int64_t>(count));
            return out;
        }));
    return t;
}

Tool makeMediaSetTagsTool(ProjectSession* session) {
    Tool t;
    t.name = kMediaSetTags;
    t.description = "Replace a media asset's tag list wholesale (usable-editor tasks.md task 15; "
                    "no dedicated Requirement). Not an undoable edit: tags are informational "
                    "project state, like an asset's sourcePath, not a timeline edit.";
    t.schema.arg(uuidArg("assetId", true, "UUID of the media asset in Project.assets."))
        .arg(ArgSpec{.name = "tags",
                     .kind = JsonKind::Array,
                     .required = true,
                     .description = "The asset's complete new tag list (strings); replaces "
                                    "any existing tags."});
    t.handler = [session](const Json& in) -> Result<Json> {
        if (session == nullptr) return err<Json>(noProjectOpen(kMediaSetTags));
        Result<Uuid> assetId = requireUuid(in, "assetId");
        if (assetId.isError()) return err<Json>(std::move(assetId).error());
        const Json* tagsArg = in.find("tags");
        if (tagsArg == nullptr || !tagsArg->isArray()) {
            return err<Json>(invalidArgument("missing or non-array field 'tags'"));
        }
        std::vector<std::string> tags;
        tags.reserve(tagsArg->asArray().size());
        for (const Json& tag : tagsArg->asArray()) {
            if (!tag.isString()) {
                return err<Json>(invalidArgument("field 'tags' must contain strings"));
            }
            tags.push_back(tag.asString());
        }
        Result<void> applied =
            session->mediaLibrary().setAssetTags(assetId.value(), std::move(tags));
        if (applied.isError()) {
            return err<Json>(std::move(applied).error());
        }
        Json out = Json::object();
        out.set("assetId", assetId.value().toString());
        return out;
    };
    return t;
}

}  // namespace palmier::services::detail
