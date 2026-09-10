// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/ProjectStoreRead.cpp - reading a `.palmier` document back into a Project:
// the typed accessors that turn a parsed JsonValue into domain objects, and the
// per-entity readers they build on.
//
// Split out of services/ProjectStore.cpp; the shared contract is
// services/ProjectStoreInternal.hpp. Every definition was moved, not rewritten.

#include "services/ProjectStoreInternal.hpp"

namespace palmier::services::detail {

[[noreturn]] void fail(std::string message) { throw JsonParseError{std::move(message)}; }

const JsonValue& requireMember(const JsonValue& obj, std::string_view key) {
    const JsonValue* v = obj.find(key);
    if (v == nullptr) fail("missing required field '" + std::string{key} + "'");
    return *v;
}

const JsonValue& requireObject(const JsonValue& v, std::string_view what) {
    if (!v.isObject()) fail(std::string{what} + " must be an object");
    return v;
}

const JsonValue& requireArray(const JsonValue& v, std::string_view what) {
    if (!v.isArray()) fail(std::string{what} + " must be an array");
    return v;
}

std::string requireString(const JsonValue& obj, std::string_view key) {
    const JsonValue& v = requireMember(obj, key);
    if (!v.isString()) fail("field '" + std::string{key} + "' must be a string");
    return v.string;
}

bool requireBool(const JsonValue& obj, std::string_view key) {
    const JsonValue& v = requireMember(obj, key);
    if (!v.isBool()) fail("field '" + std::string{key} + "' must be a boolean");
    return v.boolean;
}

std::int64_t parseInt64(const JsonValue& v, std::string_view key) {
    if (!v.isNumber()) fail("field '" + std::string{key} + "' must be a number");
    std::int64_t out = 0;
    const char* begin = v.number.data();
    const char* end = v.number.data() + v.number.size();
    const auto [ptr, ec] = std::from_chars(begin, end, out);
    if (ec != std::errc{} || ptr != end) {
        fail("field '" + std::string{key} + "' is not a valid integer");
    }
    return out;
}

std::int64_t requireInt64(const JsonValue& obj, std::string_view key) {
    return parseInt64(requireMember(obj, key), key);
}

double parseDouble(const JsonValue& v, std::string_view what) {
    if (!v.isNumber()) fail("field '" + std::string{what} + "' must be a number");
    double out = 0.0;
    const char* begin = v.number.data();
    const char* end = v.number.data() + v.number.size();
    const auto [ptr, ec] = std::from_chars(begin, end, out);
    if (ec != std::errc{} || ptr != end) {
        fail("field '" + std::string{what} + "' is not a valid number");
    }
    return out;
}

double requireDouble(const JsonValue& obj, std::string_view key) {
    return parseDouble(requireMember(obj, key), key);
}

Uuid requireUuid(const JsonValue& obj, std::string_view key) {
    const std::string text = requireString(obj, key);
    std::optional<Uuid> id = Uuid::parse(text);
    if (!id.has_value()) fail("field '" + std::string{key} + "' is not a valid UUID");
    return *id;
}

Duration readDuration(const JsonValue& obj, std::string_view key) {
    return Duration::fromNanoseconds(requireInt64(obj, key));
}

std::string optionalString(const JsonValue& obj, std::string_view key, std::string fallback) {
    const JsonValue* v = obj.find(key);
    if (v == nullptr || v->isNull()) return fallback;
    if (!v->isString()) fail("field '" + std::string{key} + "' must be a string");
    return v->string;
}

Uuid parseUuidString(const JsonValue& v, std::string_view what) {
    if (!v.isString()) fail(std::string{what} + " must be a string");
    const std::optional<Uuid> id = Uuid::parse(v.string);
    if (!id.has_value()) fail(std::string{what} + " is not a valid UUID");
    return *id;
}

ClipGroup readClipGroup(const JsonValue& v) {
    requireObject(v, "clipGroup");
    ClipGroup group;
    group.id = requireUuid(v, "id");
    const JsonValue& clipIds = requireArray(requireMember(v, "clipIds"), "clipGroup.clipIds");
    for (const JsonValue& c : clipIds.array) {
        group.clipIds.push_back(parseUuidString(c, "clipGroup.clipIds entry"));
    }
    return group;
}

std::vector<ClipGroup> readClipGroups(const JsonValue& projectObj) {
    const JsonValue* v = projectObj.find("clipGroups");
    if (v == nullptr || v->isNull()) return {};  // 1.0 default
    requireArray(*v, "clipGroups");
    std::vector<ClipGroup> groups;
    groups.reserve(v->array.size());
    for (const JsonValue& g : v->array) groups.push_back(readClipGroup(g));
    return groups;
}

MediaAssetRef readMediaAssetRef(const JsonValue& v) {
    requireObject(v, "assetRef");
    MediaAssetRef ref;
    ref.assetId = requireUuid(v, "assetId");
    ref.sourcePath = requireString(v, "sourcePath");
    // Schema 1.4 addition (usable-editor tasks.md task 15): absent entirely on
    // every pre-1.4 document, in which case the asset simply has no tags —
    // the identical absent-means-default convention every earlier addition
    // (tracks[].name, textStyle, captionText) already established, just for
    // an array rather than a string or an optional object.
    if (const JsonValue* tags = v.find("tags"); tags != nullptr && !tags->isNull()) {
        const JsonValue& tagsArray = requireArray(*tags, "asset.tags");
        for (const JsonValue& tag : tagsArray.array) {
            if (!tag.isString()) fail("asset.tags entries must be strings");
            ref.tags.push_back(tag.string);
        }
    }
    return ref;
}

Effect readEffect(const JsonValue& v) {
    requireObject(v, "effect");
    Effect fx;
    fx.id = requireUuid(v, "id");
    const std::optional<EffectType> type = effectTypeFromKey(requireString(v, "type"));
    if (!type.has_value()) fail("unknown effect type");
    fx.type = *type;
    const JsonValue& params = requireObject(requireMember(v, "parameters"), "parameters");
    for (const auto& [name, pv] : params.members) {
        fx.parameters.emplace(name, parseDouble(pv, name));
    }
    // Schema 1.5 (monitoring-and-grading task 7; Requirement 7.3): absent means "no
    // resource", so every 1.0-1.4 document loads with an empty path and nothing is
    // migrated in. Materialising a "" here would be indistinguishable, but writeEffect
    // omits an empty path, so a load-then-save of an old project must not GAIN the key.
    fx.resourcePath = optionalString(v, "resourcePath", "");
    return fx;
}

Transition readTransition(const JsonValue& v) {
    requireObject(v, "transitionIn");
    Transition t;
    t.id = requireUuid(v, "id");
    const std::optional<TransitionKind> kind = transitionKindFromKey(requireString(v, "kind"));
    if (!kind.has_value()) fail("unknown transition kind");
    t.kind = *kind;
    t.duration = readDuration(v, "duration");
    return t;
}

TextStyle readTextStyle(const JsonValue& v) {
    requireObject(v, "textStyle");
    TextStyle style;
    style.content = requireString(v, "content");
    style.fontFamily = requireString(v, "fontFamily");
    style.pointSize = requireDouble(v, "pointSize");
    style.colorR = requireDouble(v, "colorR");
    style.colorG = requireDouble(v, "colorG");
    style.colorB = requireDouble(v, "colorB");
    style.colorA = requireDouble(v, "colorA");
    const std::optional<TextAlignment> alignment =
        textAlignmentFromKey(requireString(v, "alignment"));
    if (!alignment.has_value()) fail("unknown text alignment");
    style.alignment = *alignment;
    style.x = requireDouble(v, "x");
    style.y = requireDouble(v, "y");
    return style;
}

Clip readClip(const JsonValue& v) {
    requireObject(v, "clip");
    Clip clip;
    clip.id = requireUuid(v, "id");
    clip.assetRef = readMediaAssetRef(requireMember(v, "assetRef"));
    clip.timelineStart = readDuration(v, "timelineStart");
    clip.sourceIn = readDuration(v, "sourceIn");
    clip.sourceOut = readDuration(v, "sourceOut");

    const JsonValue& effects = requireArray(requireMember(v, "effects"), "clip.effects");
    for (const JsonValue& e : effects.array) clip.effects.push_back(readEffect(e));

    const JsonValue& transition = requireMember(v, "transitionIn");
    if (!transition.isNull()) clip.transitionIn = readTransition(transition);

    clip.gain = requireDouble(v, "gain");
    clip.opacity = requireDouble(v, "opacity");

    // Schema 1.2 (Requirement 9): absent entirely on a 1.1 document, or present
    // but null on a 1.2 document's own non-text clip — both mean "not a text
    // clip", so no textStyle is set either way.
    if (const JsonValue* textStyle = v.find("textStyle");
        textStyle != nullptr && !textStyle->isNull()) {
        clip.textStyle = readTextStyle(*textStyle);
    }
    // Schema 1.3 (Requirement 10): the identical absent-or-null convention
    // textStyle above already established, one schema minor later.
    if (const JsonValue* captionText = v.find("captionText");
        captionText != nullptr && !captionText->isNull()) {
        if (!captionText->isString()) fail("clip.captionText must be a string");
        clip.captionText = captionText->string;
    }
    return clip;
}

Track readTrack(const JsonValue& v) {
    requireObject(v, "track");
    Track track;
    track.id = requireUuid(v, "id");
    const std::optional<TrackKind> kind = trackKindFromKey(requireString(v, "kind"));
    if (!kind.has_value()) fail("unknown track kind");
    track.kind = *kind;
    track.name = optionalString(v, "name", "");  // schema 1.1; default ""
    track.muted = requireBool(v, "muted");
    track.locked = requireBool(v, "locked");
    const JsonValue& clips = requireArray(requireMember(v, "clips"), "track.clips");
    for (const JsonValue& c : clips.array) track.clips.push_back(readClip(c));
    return track;
}

Project readProject(const JsonValue& v) {
    requireObject(v, "project");
    Project p;
    p.id = requireUuid(v, "id");
    p.name = requireString(v, "name");

    const JsonValue& fps = requireObject(requireMember(v, "timelineFps"), "timelineFps");
    p.timelineFps = FrameRate{requireInt64(fps, "num"), requireInt64(fps, "den")};

    const JsonValue& canvas = requireObject(requireMember(v, "canvas"), "canvas");
    const std::int64_t width = requireInt64(canvas, "width");
    const std::int64_t height = requireInt64(canvas, "height");
    if (width < 0 || height < 0) fail("canvas dimensions must be non-negative");
    p.canvas = Resolution{static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};

    const std::optional<ColorSpace> cs = colorSpaceFromKey(requireString(v, "colorSpace"));
    if (!cs.has_value()) fail("unknown colorSpace");
    p.colorSpace = *cs;

    const JsonValue& tracks = requireArray(requireMember(v, "tracks"), "tracks");
    for (const JsonValue& t : tracks.array) p.tracks.push_back(readTrack(t));

    const JsonValue& assets = requireArray(requireMember(v, "assets"), "assets");
    for (const JsonValue& a : assets.array) p.assets.push_back(readMediaAssetRef(a));

    p.clipGroups = readClipGroups(v);  // schema 1.1; default []

    return p;
}

}  // namespace palmier::services::detail
