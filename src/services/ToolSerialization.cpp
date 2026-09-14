// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/ToolSerialization.cpp — rendering a project into the JSON the read tools return.
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
// Used only by this file, so still internal. Anything another translation unit
// needs is declared in ToolRegistryInternal.hpp instead.
// ---------------------------------------------------------------------------

namespace {

// ---------------------------------------------------------------------------
// Timeline -> JSON serialization (the `timeline.read` payload).
// ---------------------------------------------------------------------------

Json serializeEffect(const Effect& effect) {
    Json params = Json::object();
    for (const auto& [key, value] : effect.parameters) {
        params.set(key, Json(value));
    }
    Json out = Json::object();
    out.set("id", effect.id.toString());
    out.set("type", std::string(effectTypeName(effect.type)));
    out.set("parameters", std::move(params));
    return out;
}

Json serializeTransition(const Transition& t) {
    Json out = Json::object();
    out.set("id", t.id.toString());
    out.set("kind", std::string(transitionKindName(t.kind)));
    out.set("durationNs", static_cast<std::int64_t>(t.duration.nanoseconds()));
    return out;
}

Json serializeClip(const Clip& clip) {
    Json out = Json::object();
    out.set("id", clip.id.toString());
    out.set("assetId", clip.assetRef.assetId.toString());
    out.set("sourcePath", clip.assetRef.sourcePath);
    out.set("timelineStartNs", static_cast<std::int64_t>(clip.timelineStart.nanoseconds()));
    out.set("sourceInNs", static_cast<std::int64_t>(clip.sourceIn.nanoseconds()));
    out.set("sourceOutNs", static_cast<std::int64_t>(clip.sourceOut.nanoseconds()));
    out.set("durationNs", static_cast<std::int64_t>(clip.duration().nanoseconds()));
    out.set("opacity", clip.opacity);
    out.set("gain", clip.gain);

    Json effects = Json::array();
    for (const Effect& e : clip.effects) effects.push_back(serializeEffect(e));
    out.set("effects", std::move(effects));

    if (clip.transitionIn.has_value()) {
        out.set("transitionIn", serializeTransition(*clip.transitionIn));
    } else {
        out.set("transitionIn", Json(nullptr));
    }
    return out;
}

Json serializeTrack(const Track& track) {
    Json out = Json::object();
    out.set("id", track.id.toString());
    out.set("kind", std::string(trackKindName(track.kind)));
    out.set("muted", track.muted);
    out.set("locked", track.locked);
    Json clips = Json::array();
    for (const Clip& c : track.clips) clips.push_back(serializeClip(c));
    out.set("clips", std::move(clips));
    return out;
}

}  // namespace

/// Total clip count across every track — the `clipCount` the session tools report
/// (Requirements 3.4, 3.7, 3.10).
std::size_t countClips(const Project& project) {
    std::size_t count = 0;
    for (const Track& track : project.tracks) count += track.clips.size();
    return count;
}

/// `{numerator, denominator}`, the shape every tool result reports a frame rate in.
Json serializeFrameRate(FrameRate fps) {
    Json out = Json::object();
    out.set("numerator", static_cast<std::int64_t>(fps.numerator()));
    out.set("denominator", static_cast<std::int64_t>(fps.denominator()));
    return out;
}

/// `{width, height}`, the shape every tool result reports a canvas in.
Json serializeCanvas(Resolution canvas) {
    Json out = Json::object();
    out.set("width", static_cast<std::int64_t>(canvas.width));
    out.set("height", static_cast<std::int64_t>(canvas.height));
    return out;
}

/// A document path result field: the path when one is recorded, JSON null when the
/// project has never been written or loaded (the design's `documentPath:null`).
Json serializeDocumentPath(const std::optional<std::filesystem::path>& path) {
    return path.has_value() ? Json(path->string()) : Json(nullptr);
}

Json serializeProject(const Project& project) {
    Json out = Json::object();
    out.set("id", project.id.toString());
    out.set("name", project.name);

    Json fps = Json::object();
    fps.set("numerator", static_cast<std::int64_t>(project.timelineFps.numerator()));
    fps.set("denominator", static_cast<std::int64_t>(project.timelineFps.denominator()));
    out.set("timelineFps", std::move(fps));

    Json canvas = Json::object();
    canvas.set("width", static_cast<std::int64_t>(project.canvas.width));
    canvas.set("height", static_cast<std::int64_t>(project.canvas.height));
    out.set("canvas", std::move(canvas));

    out.set("colorSpace", std::string(toStringView(project.colorSpace)));
    out.set("version", project.version.toString());

    Json tracks = Json::array();
    for (const Track& t : project.tracks) tracks.push_back(serializeTrack(t));
    out.set("tracks", std::move(tracks));

    Json assets = Json::array();
    for (const MediaAssetRef& a : project.assets) {
        Json ref = Json::object();
        ref.set("assetId", a.assetId.toString());
        ref.set("sourcePath", a.sourcePath);
        assets.push_back(std::move(ref));
    }
    out.set("assets", std::move(assets));

    Json markers = Json::array();
    for (const Marker& m : project.markers) {
        Json entry = Json::object();
        entry.set("markerId", m.id.toString());
        entry.set("timelineNs", static_cast<std::int64_t>(m.timelineNs.nanoseconds()));
        entry.set("label", m.label);
        markers.push_back(std::move(entry));
    }
    out.set("markers", std::move(markers));

    out.set("durationNs", static_cast<std::int64_t>(timelineDuration(project).nanoseconds()));
    return out;
}

}  // namespace palmier::services::detail
