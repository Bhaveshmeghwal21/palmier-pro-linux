// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/TimelineTextTools.cpp — the text/title clip tools and the caption cue tools.
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
// Text and titles (usable-editor task 12; Requirement 9).
//
// Creating a text clip publishes as its own tool (kAddTextClip) rather than
// overloading timeline.add_clip, because the two shapes barely overlap: a text
// clip takes no assetId/sourcePath/sourceInNs at all (it has no source media)
// and instead takes the six TextStyle fields Requirement 9.1 names, so sharing
// one schema would make most of its arguments meaningless depending on which
// "mode" a caller picked. The command underneath both tools is the identical
// AddClipCommand, though: this handler simply constructs a Clip whose
// textStyle is populated and whose assetRef is left at its default
// (nil/"invalid") value, and AddClipCommand's own asset-registration step
// already skips exactly that case — see EditCommands.hpp's "Text and titles"
// section for why that needed no new command.
// ---------------------------------------------------------------------------

/// A closed set of alignment values, spelled the way TextAlignment::toStringView
/// already renders them, so the published enum and the parser share one
/// vocabulary (mirroring trackKindValues()/parseTrackKind()).
const std::vector<std::string>& textAlignmentValues() {
    static const std::vector<std::string> values = {
        std::string(toStringView(TextAlignment::Left)),
        std::string(toStringView(TextAlignment::Center)),
        std::string(toStringView(TextAlignment::Right))};
    return values;
}

std::optional<TextAlignment> parseTextAlignment(std::string_view s) {
    if (s == toStringView(TextAlignment::Left)) return TextAlignment::Left;
    if (s == toStringView(TextAlignment::Center)) return TextAlignment::Center;
    if (s == toStringView(TextAlignment::Right)) return TextAlignment::Right;
    return std::nullopt;
}

/// A normalized-[0,1] number argument, the convention TextStyle's colour
/// channels and screen position both use (matching EffectType::CropTransform's
/// rect, the one other place this domain model already places something in
/// normalized canvas space).
ArgSpec unitArg(std::string name, bool required, std::string description, double defaultValue) {
    return ArgSpec{.name = std::move(name),
                   .kind = JsonKind::Number,
                   .required = required,
                   .description = std::move(description) + " In [0,1]; default " +
                                  std::to_string(defaultValue) + ".",
                   .minNum = 0.0,
                   .maxNum = 1.0};
}

}  // namespace

Tool makeAddTextClipTool(ProjectSession* session) {
    Tool t;
    t.name = kAddTextClip;
    t.description = "Add a text clip (a title/lower third) onto a text track at a timeline "
                    "position.";
    t.schema.arg(uuidArg("trackId", true, "UUID of the target TEXT track."))
        .arg(uuidArg("clipId", false, "Optional explicit clip UUID; generated when omitted."))
        .arg(intArg("timelineStartNs", false, "Timeline start position in nanoseconds.", 0))
        .arg(intArg("durationNs", true, "On-screen duration in nanoseconds (> 0).", 1))
        .arg(stringArg("content", true, "The text to display."))
        .arg(stringArg("fontFamily", false,
                       "Requested font family (default 'sans-serif'); substituted with a "
                       "documented default and reported when unavailable (Requirement 9.6)."))
        .arg(ArgSpec{.name = "pointSize",
                     .kind = JsonKind::Number,
                     .description = "Font size in points, >= 1 (default 24).",
                     .minNum = 1.0})
        .arg(unitArg("colorR", false, "Red channel.", 1.0))
        .arg(unitArg("colorG", false, "Green channel.", 1.0))
        .arg(unitArg("colorB", false, "Blue channel.", 1.0))
        .arg(unitArg("colorA", false, "Alpha channel.", 1.0))
        .arg(ArgSpec{.name = "alignment",
                     .kind = JsonKind::String,
                     .description = "Horizontal alignment (default 'center').",
                     .enumValues = textAlignmentValues()})
        .arg(unitArg("x", false, "Normalized anchor x.", 0.5))
        .arg(unitArg("y", false, "Normalized anchor y.", 0.5));
    t.handler = [session](const Json& in) -> Result<Json> {
        if (session == nullptr) return err<Json>(noProjectOpen(kAddTextClip));
        TimelineEngine& engine = session->engine();
        Result<Uuid> trackId = requireUuid(in, "trackId");
        if (trackId.isError()) return err<Json>(std::move(trackId).error());
        Result<std::int64_t> durationNs = requireInt(in, "durationNs");
        if (durationNs.isError()) return err<Json>(std::move(durationNs).error());
        if (durationNs.value() <= 0) {
            return err<Json>(invalidArgument("durationNs must be > 0"));
        }
        Result<std::string> content = requireString(in, "content");
        if (content.isError()) return err<Json>(std::move(content).error());

        TextAlignment alignment = TextAlignment::Center;
        if (const Json* a = in.find("alignment"); a != nullptr && a->isString()) {
            std::optional<TextAlignment> parsed = parseTextAlignment(a->asString());
            if (!parsed) return err<Json>(invalidArgument("field 'alignment' is not known"));
            alignment = *parsed;
        }

        Clip clip;
        if (const Json* cid = in.find("clipId"); cid != nullptr && cid->isString()) {
            std::optional<Uuid> parsed = Uuid::parse(cid->asString());
            if (!parsed) return err<Json>(invalidArgument("field 'clipId' is not a valid UUID"));
            clip.id = *parsed;
        } else {
            clip.id = Uuid::generateV4();
        }
        clip.timelineStart = Duration::fromNanoseconds(in.intOr("timelineStartNs", 0));
        clip.sourceIn = Duration::zero();
        clip.sourceOut = Duration::fromNanoseconds(durationNs.value());

        TextStyle style;
        style.content = content.value();
        style.fontFamily = in.stringOr("fontFamily", style.fontFamily);
        style.pointSize = in.doubleOr("pointSize", style.pointSize);
        style.colorR = in.doubleOr("colorR", style.colorR);
        style.colorG = in.doubleOr("colorG", style.colorG);
        style.colorB = in.doubleOr("colorB", style.colorB);
        style.colorA = in.doubleOr("colorA", style.colorA);
        style.alignment = alignment;
        style.x = in.doubleOr("x", style.x);
        style.y = in.doubleOr("y", style.y);
        if (!style.isValid()) {
            return err<Json>(invalidArgument(
                "the resulting text style is not internally well-formed (pointSize must be "
                "> 0 and every colour/position value must lie within [0, 1])"));
        }
        clip.textStyle = std::move(style);

        Json out = Json::object();
        out.set("clipId", clip.id.toString());
        return applyCommand(engine,
                            std::make_unique<AddClipCommand>(trackId.value(), std::move(clip)),
                            std::move(out));
    };
    return t;
}

Tool makeSetTextContentTool(ProjectSession* session) {
    Tool t;
    t.name = kSetTextContent;
    t.description = "Change a text clip's displayed string.";
    t.schema.arg(uuidArg("clipId", true, "UUID of the text clip."))
        .arg(stringArg("content", true, "The new text to display."));
    t.handler = [session](const Json& in) -> Result<Json> {
        if (session == nullptr) return err<Json>(noProjectOpen(kSetTextContent));
        TimelineEngine& engine = session->engine();
        Result<Uuid> clipId = requireUuid(in, "clipId");
        if (clipId.isError()) return err<Json>(std::move(clipId).error());
        Result<std::string> content = requireString(in, "content");
        if (content.isError()) return err<Json>(std::move(content).error());
        Json out = Json::object();
        out.set("clipId", clipId.value().toString());
        return applyCommand(
            engine, std::make_unique<SetTextContentCommand>(clipId.value(), content.value()),
            std::move(out));
    };
    return t;
}

Tool makeSetTextStyleTool(ProjectSession* session) {
    Tool t;
    t.name = kSetTextStyle;
    t.description = "Change a text clip's font, size, colour, alignment and/or screen "
                    "position. Every argument but clipId is optional; an omitted field is "
                    "left unchanged.";
    t.schema.arg(uuidArg("clipId", true, "UUID of the text clip."))
        .arg(stringArg("fontFamily", false, "New font family."))
        .arg(ArgSpec{.name = "pointSize",
                     .kind = JsonKind::Number,
                     .description = "New font size in points, >= 1.",
                     .minNum = 1.0})
        .arg(unitArg("colorR", false, "New red channel.", 1.0))
        .arg(unitArg("colorG", false, "New green channel.", 1.0))
        .arg(unitArg("colorB", false, "New blue channel.", 1.0))
        .arg(unitArg("colorA", false, "New alpha channel.", 1.0))
        .arg(ArgSpec{.name = "alignment",
                     .kind = JsonKind::String,
                     .description = "New horizontal alignment.",
                     .enumValues = textAlignmentValues()})
        .arg(unitArg("x", false, "New normalized anchor x.", 0.5))
        .arg(unitArg("y", false, "New normalized anchor y.", 0.5));
    t.handler = [session](const Json& in) -> Result<Json> {
        if (session == nullptr) return err<Json>(noProjectOpen(kSetTextStyle));
        TimelineEngine& engine = session->engine();
        Result<Uuid> clipId = requireUuid(in, "clipId");
        if (clipId.isError()) return err<Json>(std::move(clipId).error());

        std::optional<TextAlignment> alignment;
        if (const Json* a = in.find("alignment"); a != nullptr && a->isString()) {
            alignment = parseTextAlignment(a->asString());
            if (!alignment) return err<Json>(invalidArgument("field 'alignment' is not known"));
        }

        const auto optionalNumber = [&](const char* key) -> std::optional<double> {
            const Json* v = in.find(key);
            if (v == nullptr || !v->isNumber()) return std::nullopt;
            return v->isInt() ? static_cast<double>(v->asInt()) : v->asDouble();
        };
        const auto optionalStr = [&](const char* key) -> std::optional<std::string> {
            const Json* v = in.find(key);
            if (v == nullptr || !v->isString()) return std::nullopt;
            return v->asString();
        };

        Json out = Json::object();
        out.set("clipId", clipId.value().toString());
        return applyCommand(
            engine,
            std::make_unique<SetTextStyleCommand>(
                clipId.value(), optionalStr("fontFamily"), optionalNumber("pointSize"),
                optionalNumber("colorR"), optionalNumber("colorG"), optionalNumber("colorB"),
                optionalNumber("colorA"), alignment, optionalNumber("x"), optionalNumber("y")),
            std::move(out));
    };
    return t;
}

// ---------------------------------------------------------------------------
// Captions and transcription (usable-editor task 13; Requirement 10).
//
// Creating a caption cue publishes as its own tool (kAddCaptionCue), for the
// identical reason timeline.add_text_clip does (task 12): the shape barely
// overlaps timeline.add_clip's (no assetId/sourcePath at all), and this
// handler simply constructs a Clip whose captionText is populated and whose
// assetRef is left at its default value, which AddClipCommand's own
// asset-registration step already skips.
// ---------------------------------------------------------------------------

Tool makeAddCaptionCueTool(ProjectSession* session) {
    Tool t;
    t.name = kAddCaptionCue;
    t.description = "Add a caption cue onto a caption track at a timeline position.";
    t.schema.arg(uuidArg("trackId", true, "UUID of the target CAPTION track."))
        .arg(uuidArg("clipId", false, "Optional explicit clip UUID; generated when omitted."))
        .arg(intArg("timelineStartNs", false, "Timeline start position in nanoseconds.", 0))
        .arg(intArg("durationNs", true, "On-screen duration in nanoseconds (> 0).", 1))
        .arg(stringArg("text", true, "The cue's caption text."));
    t.handler = [session](const Json& in) -> Result<Json> {
        if (session == nullptr) return err<Json>(noProjectOpen(kAddCaptionCue));
        TimelineEngine& engine = session->engine();
        Result<Uuid> trackId = requireUuid(in, "trackId");
        if (trackId.isError()) return err<Json>(std::move(trackId).error());
        Result<std::int64_t> durationNs = requireInt(in, "durationNs");
        if (durationNs.isError()) return err<Json>(std::move(durationNs).error());
        if (durationNs.value() <= 0) {
            return err<Json>(invalidArgument("durationNs must be > 0"));
        }
        Result<std::string> text = requireString(in, "text");
        if (text.isError()) return err<Json>(std::move(text).error());
        if (text.value().empty()) {
            return err<Json>(invalidArgument("text must not be empty"));
        }

        Clip clip;
        if (const Json* cid = in.find("clipId"); cid != nullptr && cid->isString()) {
            std::optional<Uuid> parsed = Uuid::parse(cid->asString());
            if (!parsed) return err<Json>(invalidArgument("field 'clipId' is not a valid UUID"));
            clip.id = *parsed;
        } else {
            clip.id = Uuid::generateV4();
        }
        clip.timelineStart = Duration::fromNanoseconds(in.intOr("timelineStartNs", 0));
        clip.sourceIn = Duration::zero();
        clip.sourceOut = Duration::fromNanoseconds(durationNs.value());
        clip.captionText = text.value();

        Json out = Json::object();
        out.set("clipId", clip.id.toString());
        return applyCommand(engine,
                            std::make_unique<AddClipCommand>(trackId.value(), std::move(clip)),
                            std::move(out));
    };
    return t;
}

Tool makeSetCaptionTextTool(ProjectSession* session) {
    Tool t;
    t.name = kSetCaptionText;
    t.description = "Change a caption cue's displayed text.";
    t.schema.arg(uuidArg("clipId", true, "UUID of the caption cue."))
        .arg(stringArg("text", true, "The new caption text."));
    t.handler = [session](const Json& in) -> Result<Json> {
        if (session == nullptr) return err<Json>(noProjectOpen(kSetCaptionText));
        TimelineEngine& engine = session->engine();
        Result<Uuid> clipId = requireUuid(in, "clipId");
        if (clipId.isError()) return err<Json>(std::move(clipId).error());
        Result<std::string> text = requireString(in, "text");
        if (text.isError()) return err<Json>(std::move(text).error());
        Json out = Json::object();
        out.set("clipId", clipId.value().toString());
        return applyCommand(engine,
                            std::make_unique<SetCaptionTextCommand>(clipId.value(), text.value()),
                            std::move(out));
    };
    return t;
}

Tool makeRetimeCaptionCueTool(ProjectSession* session) {
    Tool t;
    t.name = kRetimeCaptionCue;
    t.description = "Change a caption cue's start position and/or duration. At least one of "
                    "timelineStartNs or durationNs must be given.";
    // NOT expressible: "at least one of timelineStartNs/durationNs must be
    // given" is a cross-field relation, the same Class 1 gap
    // project.set_settings's own three-field version already occupies.
    t.schema.arg(uuidArg("clipId", true, "UUID of the caption cue."))
        .arg(intArg("timelineStartNs", false, "New timeline start position in nanoseconds.", 0))
        .arg(intArg("durationNs", false, "New on-screen duration in nanoseconds (> 0).", 1));
    t.handler = [session](const Json& in) -> Result<Json> {
        if (session == nullptr) return err<Json>(noProjectOpen(kRetimeCaptionCue));
        TimelineEngine& engine = session->engine();
        Result<Uuid> clipId = requireUuid(in, "clipId");
        if (clipId.isError()) return err<Json>(std::move(clipId).error());

        const bool hasStart = in.find("timelineStartNs") != nullptr;
        const bool hasDuration = in.find("durationNs") != nullptr;
        if (!hasStart && !hasDuration) {
            return err<Json>(invalidArgument(
                "timeline.retime_caption_cue: at least one of timelineStartNs or durationNs "
                "must be given"));
        }

        std::optional<Duration> newStart;
        if (hasStart) {
            Result<std::int64_t> startNs = requireInt(in, "timelineStartNs");
            if (startNs.isError()) return err<Json>(std::move(startNs).error());
            newStart = Duration::fromNanoseconds(startNs.value());
        }
        std::optional<Duration> newDuration;
        if (hasDuration) {
            Result<std::int64_t> durationNs = requireInt(in, "durationNs");
            if (durationNs.isError()) return err<Json>(std::move(durationNs).error());
            if (durationNs.value() <= 0) {
                return err<Json>(invalidArgument("durationNs must be > 0"));
            }
            newDuration = Duration::fromNanoseconds(durationNs.value());
        }

        Json out = Json::object();
        out.set("clipId", clipId.value().toString());
        return applyCommand(
            engine,
            std::make_unique<RetimeCaptionCueCommand>(clipId.value(), newStart, newDuration),
            std::move(out));
    };
    return t;
}

Tool makeRemoveCaptionCueTool(ProjectSession* session) {
    Tool t;
    t.name = kRemoveCaptionCue;
    t.description = "Remove a caption cue by id from whichever track holds it.";
    // Reuses DeleteClipCommand directly: a caption cue is an ordinary Clip, and
    // "remove it" needs no caption-specific behaviour beyond what removing any
    // other clip already does.
    t.schema.arg(uuidArg("clipId", true, "UUID of the caption cue to remove."));
    t.handler = [session](const Json& in) -> Result<Json> {
        if (session == nullptr) return err<Json>(noProjectOpen(kRemoveCaptionCue));
        TimelineEngine& engine = session->engine();
        Result<Uuid> clipId = requireUuid(in, "clipId");
        if (clipId.isError()) return err<Json>(std::move(clipId).error());
        Json out = Json::object();
        out.set("clipId", clipId.value().toString());
        return applyCommand(engine, std::make_unique<DeleteClipCommand>(clipId.value()),
                            std::move(out));
    };
    return t;
}

}  // namespace palmier::services::detail
