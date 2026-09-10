// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/ProjectStoreVocabulary.cpp - the on-disk spelling of every enumerated
// value in the `.palmier` format: colour space, track kind, effect type, transition
// kind and text alignment, each with the key it writes and the parse that rejects
// anything outside the set.
//
// These are the format's compatibility surface: changing a key here changes what
// previously saved projects mean, which is why they are gathered in one file.
//
// Split out of services/ProjectStore.cpp; the shared contract is
// services/ProjectStoreInternal.hpp. Every definition was moved, not rewritten.

#include "services/ProjectStoreInternal.hpp"

namespace palmier::services::detail {

std::string_view colorSpaceKey(ColorSpace cs) {
    switch (cs) {
        case ColorSpace::Unknown:
            return "unknown";
        case ColorSpace::Srgb:
            return "srgb";
        case ColorSpace::Rec709:
            return "rec709";
        case ColorSpace::Rec2020:
            return "rec2020";
        case ColorSpace::Rec2100Pq:
            return "rec2100pq";
        case ColorSpace::Rec2100Hlg:
            return "rec2100hlg";
        case ColorSpace::DisplayP3:
            return "displayp3";
        case ColorSpace::LinearSrgb:
            return "linearsrgb";
    }
    return "unknown";
}

std::optional<ColorSpace> colorSpaceFromKey(std::string_view k) {
    if (k == "unknown") return ColorSpace::Unknown;
    if (k == "srgb") return ColorSpace::Srgb;
    if (k == "rec709") return ColorSpace::Rec709;
    if (k == "rec2020") return ColorSpace::Rec2020;
    if (k == "rec2100pq") return ColorSpace::Rec2100Pq;
    if (k == "rec2100hlg") return ColorSpace::Rec2100Hlg;
    if (k == "displayp3") return ColorSpace::DisplayP3;
    if (k == "linearsrgb") return ColorSpace::LinearSrgb;
    return std::nullopt;
}

std::string_view trackKindKey(TrackKind k) {
    switch (k) {
        case TrackKind::Audio:
            return "audio";
        // Schema 1.2 addition (usable-editor task 12; Requirement 9). An older
        // 1.1 reader sees an unrecognised key here and rejects the whole
        // document via trackKindFromKey's std::nullopt below (fail("unknown
        // track kind")), which is the intended cross-version behaviour: a 1.1
        // build cannot round-trip a text track, so it must not silently accept
        // one (SchemaVersion.hpp's compatibility rule).
        case TrackKind::Text:
            return "text";
        // Schema 1.3 addition (usable-editor task 13; Requirement 10). Same
        // cross-version behaviour as "text" above, one schema minor later.
        case TrackKind::Caption:
            return "caption";
        case TrackKind::Video:
            return "video";
    }
    return "video";
}

std::optional<TrackKind> trackKindFromKey(std::string_view k) {
    if (k == "video") return TrackKind::Video;
    if (k == "audio") return TrackKind::Audio;
    if (k == "text") return TrackKind::Text;
    if (k == "caption") return TrackKind::Caption;
    return std::nullopt;
}

std::string_view effectTypeKey(EffectType t) {
    switch (t) {
        case EffectType::Brightness:
            return "brightness";
        case EffectType::Contrast:
            return "contrast";
        case EffectType::Blur:
            return "blur";
        case EffectType::CropTransform:
            return "cropTransform";
        case EffectType::ColorGrade:
            return "colorGrade";
        case EffectType::InvertColors:
            return "invert_colors";
        case EffectType::Custom:
            return "custom";
        case EffectType::ToneCurve:
            return "tone_curve";
        case EffectType::Lut:
            return "lut";
    }
    return "custom";
}

std::optional<EffectType> effectTypeFromKey(std::string_view k) {
    if (k == "brightness") return EffectType::Brightness;
    if (k == "contrast") return EffectType::Contrast;
    if (k == "blur") return EffectType::Blur;
    if (k == "cropTransform") return EffectType::CropTransform;
    if (k == "colorGrade") return EffectType::ColorGrade;
    if (k == "invert_colors") return EffectType::InvertColors;
    if (k == "custom") return EffectType::Custom;
    if (k == "tone_curve") return EffectType::ToneCurve;
    if (k == "lut") return EffectType::Lut;
    return std::nullopt;
}

std::string_view transitionKindKey(TransitionKind k) {
    switch (k) {
        case TransitionKind::Crossfade:
            return "crossfade";
        case TransitionKind::DipToColor:
            return "dipToColor";
        case TransitionKind::Wipe:
            return "wipe";
        case TransitionKind::Slide:
            return "slide";
        case TransitionKind::Fade:
            return "fade";
    }
    return "crossfade";
}

std::optional<TransitionKind> transitionKindFromKey(std::string_view k) {
    if (k == "crossfade") return TransitionKind::Crossfade;
    if (k == "dipToColor") return TransitionKind::DipToColor;
    if (k == "wipe") return TransitionKind::Wipe;
    if (k == "slide") return TransitionKind::Slide;
    if (k == "fade") return TransitionKind::Fade;
    return std::nullopt;
}

std::string_view textAlignmentKey(TextAlignment a) { return toStringView(a); }

std::optional<TextAlignment> textAlignmentFromKey(std::string_view k) {
    if (k == toStringView(TextAlignment::Left)) return TextAlignment::Left;
    if (k == toStringView(TextAlignment::Center)) return TextAlignment::Center;
    if (k == toStringView(TextAlignment::Right)) return TextAlignment::Right;
    return std::nullopt;
}

}  // namespace palmier::services::detail
