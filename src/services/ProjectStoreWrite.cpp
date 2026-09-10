// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/ProjectStoreWrite.cpp - writing a Project out as a `.palmier` document,
// one function per entity, from a Uuid up to the whole project.
//
// Split out of services/ProjectStore.cpp; the shared contract is
// services/ProjectStoreInternal.hpp. Every definition was moved, not rewritten.

#include "services/ProjectStoreInternal.hpp"

namespace palmier::services::detail {

void writeUuid(JsonWriter& w, std::string_view field, const Uuid& id) {
    w.key(field);
    w.valueString(id.toString());
}

void writeDuration(JsonWriter& w, std::string_view field, Duration d) {
    w.key(field);
    w.valueInt(d.ticks());
}

void writeMediaAssetRef(JsonWriter& w, const MediaAssetRef& ref) {
    w.beginObject();
    writeUuid(w, "assetId", ref.assetId);
    w.key("sourcePath");
    w.valueString(ref.sourcePath);
    // Schema 1.4 addition (usable-editor tasks.md task 15): always written as
    // an array, empty when the asset carries no tags, mirroring how every
    // other array-shaped field in this format is always present rather than
    // conditionally omitted like the optional-object fields (textStyle,
    // transitionIn) are.
    w.key("tags");
    w.beginArray();
    for (const std::string& tag : ref.tags) {
        w.valueString(tag);
    }
    w.endArray();
    w.endObject();
}

void writeEffect(JsonWriter& w, const Effect& fx) {
    w.beginObject();
    writeUuid(w, "id", fx.id);
    w.key("type");
    w.valueString(effectTypeKey(fx.type));
    w.key("parameters");
    w.beginObject();
    for (const auto& [name, value] : fx.parameters) {
        w.key(name);
        w.valueDouble(value);
    }
    w.endObject();
    // Schema 1.5 addition (monitoring-and-grading task 7; Requirement 7.2): omitted
    // ENTIRELY when empty, not written as "". That is what keeps a project with no LUT
    // byte-identical to what a 1.4 build wrote, so the bump costs nothing to any existing
    // document and a 1.4 reader meets no key it does not know.
    if (!fx.resourcePath.empty()) {
        w.key("resourcePath");
        w.valueString(fx.resourcePath);
    }
    w.endObject();
}

void writeTransition(JsonWriter& w, const Transition& t) {
    w.beginObject();
    writeUuid(w, "id", t.id);
    w.key("kind");
    w.valueString(transitionKindKey(t.kind));
    writeDuration(w, "duration", t.duration);
    w.endObject();
}

void writeTextStyle(JsonWriter& w, const TextStyle& style) {
    w.beginObject();
    w.key("content");
    w.valueString(style.content);
    w.key("fontFamily");
    w.valueString(style.fontFamily);
    w.key("pointSize");
    w.valueDouble(style.pointSize);
    w.key("colorR");
    w.valueDouble(style.colorR);
    w.key("colorG");
    w.valueDouble(style.colorG);
    w.key("colorB");
    w.valueDouble(style.colorB);
    w.key("colorA");
    w.valueDouble(style.colorA);
    w.key("alignment");
    w.valueString(textAlignmentKey(style.alignment));
    w.key("x");
    w.valueDouble(style.x);
    w.key("y");
    w.valueDouble(style.y);
    w.endObject();
}

void writeClip(JsonWriter& w, const Clip& clip) {
    w.beginObject();
    writeUuid(w, "id", clip.id);
    w.key("assetRef");
    writeMediaAssetRef(w, clip.assetRef);
    writeDuration(w, "timelineStart", clip.timelineStart);
    writeDuration(w, "sourceIn", clip.sourceIn);
    writeDuration(w, "sourceOut", clip.sourceOut);
    w.key("effects");
    w.beginArray();
    for (const Effect& fx : clip.effects) writeEffect(w, fx);
    w.endArray();
    w.key("transitionIn");
    if (clip.transitionIn.has_value()) {
        writeTransition(w, *clip.transitionIn);
    } else {
        w.valueNull();
    }
    w.key("gain");
    w.valueDouble(clip.gain);
    w.key("opacity");
    w.valueDouble(clip.opacity);
    // Schema 1.2 (Requirement 9). Present iff this is a text clip.
    w.key("textStyle");
    if (clip.textStyle.has_value()) {
        writeTextStyle(w, *clip.textStyle);
    } else {
        w.valueNull();
    }
    // Schema 1.3 (Requirement 10). Present iff this is a caption cue. A plain
    // string, unlike textStyle's nested object, since a caption cue carries no
    // styling of its own (Requirement 10 asks for none).
    w.key("captionText");
    if (clip.captionText.has_value()) {
        w.valueString(*clip.captionText);
    } else {
        w.valueNull();
    }
    w.endObject();
}

void writeTrack(JsonWriter& w, const Track& track) {
    w.beginObject();
    writeUuid(w, "id", track.id);
    w.key("kind");
    w.valueString(trackKindKey(track.kind));
    // Schema 1.1: always written, optional on read (default "").
    w.key("name");
    w.valueString(track.name);
    w.key("muted");
    w.valueBool(track.muted);
    w.key("locked");
    w.valueBool(track.locked);
    w.key("clips");
    w.beginArray();
    for (const Clip& clip : track.clips) writeClip(w, clip);
    w.endArray();
    w.endObject();
}

void writeClipGroup(JsonWriter& w, const ClipGroup& group) {
    w.beginObject();
    writeUuid(w, "id", group.id);
    w.key("clipIds");
    w.beginArray();
    for (const ClipId& clipId : group.clipIds) w.valueString(clipId.toString());
    w.endArray();
    w.endObject();
}

void writeProject(JsonWriter& w, const Project& p) {
    w.beginObject();
    writeUuid(w, "id", p.id);
    w.key("name");
    w.valueString(p.name);
    w.key("timelineFps");
    w.beginObject();
    w.key("num");
    w.valueInt(p.timelineFps.numerator());
    w.key("den");
    w.valueInt(p.timelineFps.denominator());
    w.endObject();
    w.key("canvas");
    w.beginObject();
    w.key("width");
    w.valueUint(p.canvas.width);
    w.key("height");
    w.valueUint(p.canvas.height);
    w.endObject();
    w.key("colorSpace");
    w.valueString(colorSpaceKey(p.colorSpace));
    w.key("tracks");
    w.beginArray();
    for (const Track& track : p.tracks) writeTrack(w, track);
    w.endArray();
    w.key("assets");
    w.beginArray();
    for (const MediaAssetRef& asset : p.assets) writeMediaAssetRef(w, asset);
    w.endArray();
    // Schema 1.1: always written, optional on read (default []). Persisted
    // faithfully and interpreted by nothing — see core/ClipGroup.hpp.
    w.key("clipGroups");
    w.beginArray();
    for (const ClipGroup& group : p.clipGroups) writeClipGroup(w, group);
    w.endArray();
    w.endObject();
}

}  // namespace palmier::services::detail
