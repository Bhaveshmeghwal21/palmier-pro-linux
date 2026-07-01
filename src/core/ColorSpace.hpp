// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/ColorSpace.hpp — the working/target color space of a project.
//
// Project.colorSpace names the canvas color space, e.g. Rec.709 or Rec.2020
// (design.md Data Models). Color management is applied via LittleCMS and
// shader-based transforms; this enum is the stable identifier persisted in the
// project and passed across the media/GPU boundary. Rec.709 is the default for
// SDR HD content.

#ifndef PALMIER_CORE_COLORSPACE_HPP
#define PALMIER_CORE_COLORSPACE_HPP

#include <string_view>

namespace palmier {

/// Named color spaces supported by the compositor and color pipeline.
enum class ColorSpace {
    Unknown = 0,
    Srgb,       ///< sRGB (IEC 61966-2-1), typical for imported images/UI.
    Rec709,     ///< ITU-R BT.709, SDR HD video (default working space).
    Rec2020,    ///< ITU-R BT.2020, wide-gamut UHD/HDR container.
    Rec2100Pq,  ///< BT.2100 PQ (HDR10) transfer.
    Rec2100Hlg, ///< BT.2100 HLG transfer.
    DisplayP3,  ///< Display P3 (Apple wide gamut).
    LinearSrgb, ///< Linear-light sRGB primaries (scene-linear compositing).
};

/// The default working color space for a new project (SDR HD).
[[nodiscard]] constexpr ColorSpace defaultColorSpace() noexcept { return ColorSpace::Rec709; }

/// Stable, human-readable name (matches the design's "Rec.709" style labels).
[[nodiscard]] constexpr std::string_view toStringView(ColorSpace cs) noexcept {
    switch (cs) {
        case ColorSpace::Unknown:     return "Unknown";
        case ColorSpace::Srgb:        return "sRGB";
        case ColorSpace::Rec709:      return "Rec.709";
        case ColorSpace::Rec2020:     return "Rec.2020";
        case ColorSpace::Rec2100Pq:   return "Rec.2100 PQ";
        case ColorSpace::Rec2100Hlg:  return "Rec.2100 HLG";
        case ColorSpace::DisplayP3:   return "Display P3";
        case ColorSpace::LinearSrgb:  return "Linear sRGB";
    }
    return "Unknown";
}

/// True for wide-gamut spaces (useful for pipeline/tone-mapping decisions).
[[nodiscard]] constexpr bool isWideGamut(ColorSpace cs) noexcept {
    switch (cs) {
        case ColorSpace::Rec2020:
        case ColorSpace::Rec2100Pq:
        case ColorSpace::Rec2100Hlg:
        case ColorSpace::DisplayP3:
            return true;
        default:
            return false;
    }
}

/// True for high-dynamic-range transfer functions.
[[nodiscard]] constexpr bool isHdr(ColorSpace cs) noexcept {
    return cs == ColorSpace::Rec2100Pq || cs == ColorSpace::Rec2100Hlg;
}

} // namespace palmier

#endif // PALMIER_CORE_COLORSPACE_HPP
