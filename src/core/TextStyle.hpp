// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/TextStyle.hpp — the on-screen text carried by a text clip (usable-editor
// task 12; Requirement 9).
//
// Requirement 9.1: "THE domain core SHALL carry a text clip type holding at
// least the string, font family, point size, colour, alignment and screen
// position, and it SHALL round-trip through save and open." A Clip carrying a
// TextStyle (Clip::textStyle, core/Clip.hpp) *is* that text clip type: rather
// than a parallel struct with its own timeline geometry, a text clip is an
// ordinary Clip whose textStyle is present, so it gets every existing
// timeline operation — move, trim, split, ripple-delete, undo/redo, drag in the
// graphical timeline (task 11) — for free, through the exact same command path
// every other clip already uses. What makes it a *text* clip rather than a
// media clip is that it carries no usable assetRef and instead carries this.
//
// Such a clip must live on a TrackKind::Text track (core/Track.hpp):
// gpu::Compositor's video-layer gathering calls a ClipFrameProvider that expects
// real decodable media (media/DecoderClipFrameProvider.hpp — "the clip names no
// asset" is one of its own documented error paths), so a text clip cannot be
// gathered alongside ordinary video clips without either corrupting that
// contract or special-casing every video-clip consumer to skip it. A dedicated
// track kind keeps the two collections (and the two very different per-layer
// rendering paths, decode-then-composite versus rasterize-then-composite)
// cleanly apart while still participating in the identical
// "z = track.index, painter's order" layering rule renderAt() already applies
// (gpu/Compositor.hpp) — a text track is just another track in Project.tracks,
// so a title can sit above or below any video track by its position in the
// list, exactly like today's multi-video-track compositing already works.
//
// Colour is three normalized [0,1] channels plus alpha, matching the
// compositor's own working convention (gpu::RgbaColor is the 8-bit rendering of
// the same idea) without giving core:: a dependency on gpu::. Position is
// normalized [0,1] over the project canvas, the same convention
// EffectType::CropTransform's rect already uses (core/Effect.hpp), so "screen
// position" reads consistently with the one other place this domain model
// already places something in normalized canvas space.

#ifndef PALMIER_CORE_TEXTSTYLE_HPP
#define PALMIER_CORE_TEXTSTYLE_HPP

#include <string>
#include <string_view>

namespace palmier {

/// Horizontal alignment of the text block around its anchor position.
enum class TextAlignment {
    Left,
    Center,
    Right,
};

struct TextStyle {
    std::string   content;                       ///< The text to display.
    std::string   fontFamily{"sans-serif"};       ///< Requested font family.
    double        pointSize = 24.0;               ///< Font size in points; must be > 0.
    double        colorR = 1.0;                   ///< Red channel, [0,1].
    double        colorG = 1.0;                   ///< Green channel, [0,1].
    double        colorB = 1.0;                   ///< Blue channel, [0,1].
    double        colorA = 1.0;                   ///< Alpha channel, [0,1].
    TextAlignment alignment = TextAlignment::Center;
    double        x = 0.5;                        ///< Normalized anchor x, [0,1].
    double        y = 0.5;                        ///< Normalized anchor y, [0,1].

    /// Internal well-formedness a command can check without any dependency
    /// beyond this header: a positive point size and every channel/position
    /// value within its documented [0,1] range. Requirement 9's own numeric
    /// bounds (e.g. a maximum point size) are the Tool_Surface's to enforce,
    /// mirroring how Clip's own opacity/gain split the same two checks between
    /// core (well-formedness) and services:: (declared range) — see
    /// core/EditCommands.hpp's SetProjectSettingsCommand for the established
    /// precedent of that split.
    [[nodiscard]] bool isValid() const noexcept {
        const auto inUnit = [](double v) noexcept { return v >= 0.0 && v <= 1.0; };
        return pointSize > 0.0 && inUnit(colorR) && inUnit(colorG) && inUnit(colorB) &&
               inUnit(colorA) && inUnit(x) && inUnit(y);
    }
};

/// Stable, lower-case name for an alignment value (used by the Tool_Surface and
/// the document serializer so both speak the same vocabulary).
[[nodiscard]] constexpr std::string_view toStringView(TextAlignment alignment) noexcept {
    switch (alignment) {
        case TextAlignment::Left:   return "left";
        case TextAlignment::Center: return "center";
        case TextAlignment::Right:  return "right";
    }
    return "center";
}

} // namespace palmier

#endif // PALMIER_CORE_TEXTSTYLE_HPP
