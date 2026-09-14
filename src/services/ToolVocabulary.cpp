// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/ToolVocabulary.cpp — the closed value vocabularies the tool schemas publish — effect type,
// curve channel, curve-point operation, transition kind, track kind,
// colour space and export codec — each with the name it renders to and
// the parse that rejects anything outside the set.
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

/// The accepted `timeline.export` `codec` values, declared once: this list is both
/// the `ArgSpec::enumValues` the schema publishes and the exact set
/// `services::exportRequestFromToolArguments` parses, so the advertised enum and
/// the translation agree (Requirement 9.12). They are the three codecs the
/// Encoder_Selector supports (Requirement 8.2).
const std::vector<std::string>& exportCodecValues() {
    static const std::vector<std::string> values = {"h264", "hevc", "vp9"};
    return values;
}

// ---------------------------------------------------------------------------
// Enum <-> string helpers
// ---------------------------------------------------------------------------

std::string_view effectTypeName(EffectType type) {
    switch (type) {
        case EffectType::Brightness:
            return "brightness";
        case EffectType::Contrast:
            return "contrast";
        case EffectType::Blur:
            return "blur";
        case EffectType::CropTransform:
            return "crop_transform";
        case EffectType::ColorGrade:
            return "color_grade";
        case EffectType::InvertColors:
            return "invert_colors";
        case EffectType::ToneCurve:
            return "tone_curve";
        case EffectType::Lut:
            return "lut";
        case EffectType::Denoise:
            return "denoise";
        case EffectType::SecondaryGrade:
            return "secondary_grade";
        case EffectType::Custom:
            return "custom";
    }
    return "custom";
}

/// The accepted `type` values of `timeline.add_effect`, declared once: this list
/// is both the `ArgSpec::enumValues` the schema publishes and the exact set
/// `parseEffectType` accepts, so the advertised enum and the handler agree
/// (Requirement 9.12). `invert_colors` is the ported upstream PR 408 effect
/// (Requirement 14.4); `denoise` and `secondary_grade` close the upstream
/// `denoise` category and the secondary-grading half of the `color` category.
const std::vector<std::string>& effectTypeValues() {
    static const std::vector<std::string> values = {
        "brightness", "contrast", "blur",    "crop_transform",  "color_grade", "invert_colors",
        "tone_curve", "lut",      "denoise", "secondary_grade", "custom"};
    return values;
}

std::optional<EffectType> parseEffectType(std::string_view s) {
    if (s == "brightness") return EffectType::Brightness;
    if (s == "contrast") return EffectType::Contrast;
    if (s == "blur") return EffectType::Blur;
    if (s == "crop_transform") return EffectType::CropTransform;
    if (s == "color_grade") return EffectType::ColorGrade;
    if (s == "invert_colors") return EffectType::InvertColors;
    if (s == "tone_curve") return EffectType::ToneCurve;
    if (s == "lut") return EffectType::Lut;
    if (s == "denoise") return EffectType::Denoise;
    if (s == "secondary_grade") return EffectType::SecondaryGrade;
    if (s == "custom") return EffectType::Custom;
    return std::nullopt;
}

/// The tone-curve channels and point operations, named for the tool surface.
///
/// Both vocabularies live in core (core::curveChannelToolName,
/// core::curvePointOperationToolName) rather than here, because the Inspector names the
/// same operations and a second copy would be a second thing to forget. These are the
/// thin adapters the ArgSpec vocabulary wants.
const std::vector<std::string>& curveChannelValues() { return curveChannelToolNames(); }

std::optional<CurveChannel> parseCurveChannel(std::string_view s) {
    return parseCurveChannelToolName(s);
}

const std::vector<std::string>& curvePointOperationValues() {
    return curvePointOperationToolNames();
}

std::optional<EditCurvePointCommand::Operation> parseCurvePointOperation(std::string_view s) {
    return parseCurvePointOperationToolName(s);
}

std::string_view transitionKindName(TransitionKind kind) {
    switch (kind) {
        case TransitionKind::Crossfade:
            return "crossfade";
        case TransitionKind::DipToColor:
            return "dip_to_color";
        case TransitionKind::Wipe:
            return "wipe";
        case TransitionKind::Slide:
            return "slide";
        case TransitionKind::Fade:
            return "fade";
    }
    return "crossfade";
}

/// The accepted `kind` values of `timeline.add_transition`, declared once for the
/// same reason as `effectTypeValues()`: the published enum and
/// `parseTransitionKind`'s accepted set are one list.
const std::vector<std::string>& transitionKindValues() {
    static const std::vector<std::string> values = {"crossfade", "dip_to_color", "wipe", "slide",
                                                    "fade"};
    return values;
}

const std::vector<std::string>& layoutKindValues() {
    static const std::vector<std::string> values = {"side_by_side", "picture_in_picture", "grid"};
    return values;
}

const std::vector<std::string>& animatablePropertyValues() {
    // Built from allAnimatableProperties() rather than written out, so a property added to the
    // model appears here automatically instead of being advertised nowhere.
    static const std::vector<std::string> values = [] {
        std::vector<std::string> names;
        for (const AnimatableProperty property : allAnimatableProperties()) {
            names.emplace_back(animatablePropertyName(property));
        }
        return names;
    }();
    return values;
}

const std::vector<std::string>& interpolationValues() {
    static const std::vector<std::string> values = {"linear", "smooth"};
    return values;
}

std::optional<LayoutKind> parseLayoutKind(std::string_view s) {
    if (s == "side_by_side") return LayoutKind::SideBySide;
    if (s == "picture_in_picture") return LayoutKind::PictureInPicture;
    if (s == "grid") return LayoutKind::Grid;
    return std::nullopt;
}

std::optional<TransitionKind> parseTransitionKind(std::string_view s) {
    if (s == "crossfade") return TransitionKind::Crossfade;
    if (s == "dip_to_color") return TransitionKind::DipToColor;
    if (s == "wipe") return TransitionKind::Wipe;
    if (s == "slide") return TransitionKind::Slide;
    if (s == "fade") return TransitionKind::Fade;
    return std::nullopt;
}

std::string_view trackKindName(TrackKind kind) {
    switch (kind) {
        case TrackKind::Audio:
            return "audio";
        case TrackKind::Text:
            return "text";
        case TrackKind::Caption:
            return "caption";
        case TrackKind::Video:
            return "video";
    }
    return "video";
}

/// The accepted `kind` values of the track tools, declared once so the published
/// enum and `parseTrackKind`'s accepted set are one list (Requirements 3.3, 3.8;
/// `text` added by usable-editor task 12, Requirement 9; `caption` by task 13,
/// Requirement 10).
const std::vector<std::string>& trackKindValues() {
    static const std::vector<std::string> values = {"video", "audio", "text", "caption"};
    return values;
}

std::optional<TrackKind> parseTrackKind(std::string_view s) {
    if (s == "video") return TrackKind::Video;
    if (s == "audio") return TrackKind::Audio;
    if (s == "text") return TrackKind::Text;
    if (s == "caption") return TrackKind::Caption;
    return std::nullopt;
}

/// The colour spaces `project.create` accepts: exactly the set the domain core
/// exposes (Requirement 3.2), spelled with the core's own stable display names
/// (`ColorSpace::toStringView`) so the published enum, the validator and the
/// reported result all use one vocabulary. `Unknown` is deliberately absent: it is
/// not a selectable working space, and ProjectSession rejects it.
const std::vector<std::string>& colorSpaceValues() {
    static const std::vector<std::string> values = {
        std::string(toStringView(ColorSpace::Srgb)),
        std::string(toStringView(ColorSpace::Rec709)),
        std::string(toStringView(ColorSpace::Rec2020)),
        std::string(toStringView(ColorSpace::Rec2100Pq)),
        std::string(toStringView(ColorSpace::Rec2100Hlg)),
        std::string(toStringView(ColorSpace::DisplayP3)),
        std::string(toStringView(ColorSpace::LinearSrgb))};
    return values;
}

std::optional<ColorSpace> parseColorSpace(std::string_view s) {
    for (const ColorSpace candidate :
         {ColorSpace::Srgb, ColorSpace::Rec709, ColorSpace::Rec2020, ColorSpace::Rec2100Pq,
          ColorSpace::Rec2100Hlg, ColorSpace::DisplayP3, ColorSpace::LinearSrgb}) {
        if (toStringView(candidate) == s) return candidate;
    }
    return std::nullopt;
}

}  // namespace palmier::services::detail
