// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/Effect.hpp — a per-clip visual/audio effect in the timeline model.
//
// A Clip carries an ordered list of Effects (design.md Data Models). Effects are
// realized at render time as SPIR-V compute kernels registered with the Compositor
// (design.md "Effects as SPIR-V compute kernels": brightness/contrast, blur,
// crop/transform, color grade, transitions). This type is the UI-agnostic,
// serializable description of one such effect: a stable id, a kind, and a bag of
// named scalar parameters. The design's example usage constructs effects through
// named factories (e.g. Effect::brightness(0.1)).

#ifndef PALMIER_CORE_EFFECT_HPP
#define PALMIER_CORE_EFFECT_HPP

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <string>

#include "core/Uuid.hpp"

namespace palmier {

/// The kind of processing an Effect applies. `Custom` covers effects supplied by
/// a registered SPIR-V kernel not enumerated here.
enum class EffectType {
    Brightness,
    Contrast,
    Blur,
    CropTransform,
    ColorGrade,
    /// Per-channel colour inversion (upstream PR 408, Requirements 14.4): each
    /// 8-bit red, green and blue value becomes 255 minus the input value, while
    /// every alpha value is left unchanged. Takes no parameters.
    InvertColors,
    /// Tone curve (monitoring-and-grading Requirement 5): maps input intensity to
    /// output intensity through user-placed control points, with a master curve plus
    /// independent red, green and blue curves. The control points live in
    /// `parameters` under indexed names — see core/ToneCurve.hpp for the encoding,
    /// the interpolation and why the curve is baked to a 256-entry table.
    ToneCurve,

    /// A 3D lookup table loaded from a `.cube` file (monitoring-and-grading
    /// Requirement 7). The path lives in `resourcePath`, not in `parameters`, because a
    /// path is not a double -- which is the whole reason schema 1.5 exists. See
    /// gpu/CubeLut.hpp for the parser and the interpolator.
    Lut,

    /// Edge-preserving noise reduction (upstream parity: the `denoise` tool category).
    ///
    /// A bilateral average, NOT a blur, and the distinction is the whole point of the
    /// effect: neighbours are weighted by how close their luma is to the centre pixel's,
    /// so flat noisy regions average together while an edge -- where the luma difference
    /// is large -- keeps its neighbours' weights near zero and stays sharp. `EffectType::Blur`
    /// already exists and averages indiscriminately; an editor needs both.
    ///
    /// Two parameters, `strength` (0-1, the range sigma, how readily differing pixels are
    /// mixed) and `radius` (the window half-width in pixels). A `strength` of 0 is an exact
    /// no-op, so a freshly added denoise effect changes nothing until it is given a value --
    /// matching Brightness at `amount` 0 rather than surprising the caller.
    Denoise,

    /// Secondary (qualifier-masked) grade -- upstream parity: the `color` tool category's
    /// "secondary grading" half.
    ///
    /// The SAME correction ColorGrade applies -- per-channel gain, lift, gamma and a
    /// saturation mix, in that order -- but applied only where an HSL qualifier selects the
    /// pixel. That is the entire difference between a primary and a secondary grade: a
    /// primary moves the whole frame, a secondary moves the sky, or the skin tones, or one
    /// jacket, and leaves the rest alone.
    ///
    /// The qualifier is a hue arc (`hueCenter`, `hueWidth`), a saturation band
    /// (`satMin`, `satMax`), a luma band (`lumaMin`, `lumaMax`) and a `softness` that
    /// feathers all three, so a selection does not show a hard edge where it stops.
    ///
    /// The DEFAULT qualifier selects every pixel, which makes a default SecondaryGrade
    /// behave exactly like a ColorGrade. That is deliberate: the qualifier narrows a
    /// correction, so "no qualifier set" must mean "not yet narrowed", never "selects
    /// nothing". A test pins the two against each other byte for byte.
    SecondaryGrade,
    Custom,
};

struct Effect {
    Uuid id;  ///< Stable per-effect identity.
    EffectType type = EffectType::Custom;
    std::map<std::string, double> parameters;  ///< Named scalar parameters.

    /// Path to an external resource this effect needs, or empty for none.
    ///
    /// Schema 1.5 (monitoring-and-grading Requirement 7.2). Carries a `.cube` file's
    /// path for a LUT effect. EMPTY MEANS "no resource" rather than "not yet loaded",
    /// which is what lets every 1.0-1.4 document open unchanged: the key is simply
    /// absent, exactly as Clip::captionText and MediaAssetRef::tags already established.
    ///
    /// Deliberately a plain string and NOT a core::MediaAssetRef (audit finding 14): an
    /// asset is timeline content with a duration, a decoder and a media library entry,
    /// and a LUT is none of those. Reusing the type would put LUTs in the media browser
    /// and give them import semantics they cannot satisfy.
    ///
    /// Not validated here. Whether the file exists is a question for the moment it is
    /// read, and Requirement 7.8 requires a missing LUT to leave the effect in the chain
    /// and render un-graded rather than to fail the open -- so a constructor that
    /// rejected an unreadable path would make that impossible.
    std::string resourcePath;

    Effect() = default;
    Effect(Uuid id_, EffectType type_, std::map<std::string, double> params = {})
        : id(id_), type(type_), parameters(std::move(params)) {}

    // --- Named factories (fresh random identity per instance) --------------
    [[nodiscard]] static Effect brightness(double amount) {
        return Effect{Uuid::generateV4(), EffectType::Brightness, {{"amount", amount}}};
    }
    [[nodiscard]] static Effect contrast(double amount) {
        return Effect{Uuid::generateV4(), EffectType::Contrast, {{"amount", amount}}};
    }
    [[nodiscard]] static Effect blur(double radius) {
        return Effect{Uuid::generateV4(), EffectType::Blur, {{"radius", radius}}};
    }
    /// Colour inversion; parameterless (see EffectType::InvertColors).
    [[nodiscard]] static Effect invertColors() {
        return Effect{Uuid::generateV4(), EffectType::InvertColors, {}};
    }
    /// Edge-preserving noise reduction (see EffectType::Denoise).
    [[nodiscard]] static Effect denoise(double strength, double radius) {
        return Effect{
            Uuid::generateV4(), EffectType::Denoise, {{"strength", strength}, {"radius", radius}}};
    }
};

// ---------------------------------------------------------------------------
// Lift / gamma / gain primary grade (monitoring-and-grading Requirement 4)
// ---------------------------------------------------------------------------

/// The ten values an EffectType::ColorGrade effect renders with, each resolved to
/// either its stored value or its default.
///
/// The DEFAULTS are the interesting part, and they live here — in core, beside the
/// effect itself — rather than in the renderer, because two unrelated places need
/// exactly the same answer and must not drift: `gpu::applyEffectSoftware` renders
/// with them, and the Inspector displays them as "the current value of each"
/// (Requirement 4.7). If the Inspector reproduced them independently it would show a
/// per-channel lift of 0 for an old project while the renderer used the legacy
/// scalar, and the panel would be quietly lying about what is on screen.
struct ColorGradeValues {
    double gainR = 1.0, gainG = 1.0, gainB = 1.0;
    double liftR = 0.0, liftG = 0.0, liftB = 0.0;
    double gammaR = 1.0, gammaG = 1.0, gammaB = 1.0;
    double saturation = 1.0;

    /// Every value at its default, so the effect is a no-op.
    [[nodiscard]] bool isIdentity() const noexcept {
        return gainR == 1.0 && gainG == 1.0 && gainB == 1.0 && liftR == 0.0 && liftG == 0.0 &&
               liftB == 0.0 && isGammaIdentity() && saturation == 1.0;
    }
    /// No gamma to apply. Separate because the gamma STEP must be skipped entirely at
    /// unity rather than computed as pow(x, 1) — see the kernel's own comment.
    [[nodiscard]] bool isGammaIdentity() const noexcept {
        return gammaR == 1.0 && gammaG == 1.0 && gammaB == 1.0;
    }
};

/// The parameter names a ColorGrade effect understands, in presentation order.
/// Exposed so the Inspector can group them and a test can assert the set is the same
/// one the renderer reads, rather than two lists that happen to match today.
/// Every EffectType, in declaration order.
///
/// Exists so a test can assert that the Tool_Surface's closed value set is EXACTLY the core's
/// set (monitoring-and-grading Requirement 8.1) without a hand-written list -- which is the
/// thing that falls behind the enum. An effect type that renders but cannot be created through
/// a tool is invisible to the agent surface and to every scripted workflow, with nothing
/// failing to say so; this spec added three effect types, and the risk was identical each time.
inline constexpr std::array<EffectType, 11> kAllEffectTypes{
    EffectType::Brightness,     EffectType::Contrast,   EffectType::Blur,
    EffectType::CropTransform,  EffectType::ColorGrade, EffectType::InvertColors,
    EffectType::ToneCurve,      EffectType::Lut,        EffectType::Denoise,
    EffectType::SecondaryGrade, EffectType::Custom};

inline constexpr std::array<const char*, 10> kColorGradeParameterNames{
    "liftR",  "liftG", "liftB", "gammaR", "gammaG",
    "gammaB", "gainR", "gainG", "gainB",  "saturation"};

// ---------------------------------------------------------------------------
// Colour wheels (upstream parity: the `color` category's colour-wheel control)
// ---------------------------------------------------------------------------

/// The six parameter names that drive the three colour wheels, in presentation order.
///
/// A colour wheel is a polar control: the ANGLE picks a hue to push a tonal band toward,
/// and the RADIUS picks how far. Each of the three wheels therefore needs exactly two
/// numbers, and each drives the grade control that band is conventionally shaped by --
/// shadows through lift, midtones through gamma, highlights through gain.
inline constexpr std::array<const char*, 6> kColorWheelParameterNames{
    "shadowsHue",     "shadowsAmount", "midtonesHue",
    "midtonesAmount", "highlightsHue", "highlightsAmount"};

/// How far a wheel at full deflection moves its band. Deliberately gentle: a wheel is a
/// balance control, and one that reached its band's limit at the edge of the circle would
/// be unusable for the small corrections it exists to make.
inline constexpr double kColorWheelScale = 0.5;

/// A wheel position's per-channel push, as three offsets that sum to zero.
///
/// Summing to zero is the defining property of a colour BALANCE control: pushing toward a
/// hue must shift the ratio between channels without also changing overall level, or every
/// wheel would double as a brightness control and grading with one would be guesswork. It
/// is achieved by taking the fully saturated RGB for the hue and subtracting its own mean.
///
/// `amount` is clamped to [0,1]; 0 returns all zeros, so an unset wheel is an exact no-op.
struct ColorWheelOffsets {
    double r = 0.0, g = 0.0, b = 0.0;
};

[[nodiscard]] inline ColorWheelOffsets colorWheelOffsets(double hueDegrees,
                                                         double amount) noexcept {
    const double magnitude = amount < 0.0 ? 0.0 : (amount > 1.0 ? 1.0 : amount);
    if (magnitude == 0.0) return {};

    // Hue to fully saturated RGB. Floor-based modulo, matching qualifierMask's reason:
    // a negative angle must wrap rather than reflect.
    double hue = hueDegrees - 360.0 * std::floor(hueDegrees / 360.0);
    const double sector = hue / 60.0;
    const int index = static_cast<int>(std::floor(sector)) % 6;
    const double f = sector - std::floor(sector);

    double r{}, g{}, b{};
    switch (index) {
        case 0:
            r = 1.0;
            g = f;
            b = 0.0;
            break;
        case 1:
            r = 1.0 - f;
            g = 1.0;
            b = 0.0;
            break;
        case 2:
            r = 0.0;
            g = 1.0;
            b = f;
            break;
        case 3:
            r = 0.0;
            g = 1.0 - f;
            b = 1.0;
            break;
        case 4:
            r = f;
            g = 0.0;
            b = 1.0;
            break;
        default:
            r = 1.0;
            g = 0.0;
            b = 1.0 - f;
            break;
    }

    const double mean = (r + g + b) / 3.0;
    return {(r - mean) * magnitude, (g - mean) * magnitude, (b - mean) * magnitude};
}

/// Resolve a ColorGrade effect's parameter map to the values it renders with.
///
/// Requirement 4.2 and 4.3 are carried by one line here: each per-channel lift falls
/// back to the LEGACY SCALAR `lift` rather than to 0. A project saved before
/// per-channel lift existed carries only that scalar, so it keeps rendering exactly
/// as it did with no migration step and no user action.
[[nodiscard]] inline ColorGradeValues colorGradeValues(
    const std::map<std::string, double>& parameters) {
    const auto get = [&parameters](const char* name, double fallback) {
        const auto it = parameters.find(name);
        return it == parameters.end() ? fallback : it->second;
    };
    const double legacyLift = get("lift", 0.0);

    ColorGradeValues v;
    v.gainR = get("gainR", 1.0);
    v.gainG = get("gainG", 1.0);
    v.gainB = get("gainB", 1.0);
    v.liftR = get("liftR", legacyLift);
    v.liftG = get("liftG", legacyLift);
    v.liftB = get("liftB", legacyLift);
    v.gammaR = get("gammaR", 1.0);
    v.gammaG = get("gammaG", 1.0);
    v.gammaB = get("gammaB", 1.0);
    v.saturation = get("saturation", 1.0);

    // --- colour wheels -----------------------------------------------------
    //
    // The three wheels are folded into the nine numbers HERE, host-side, rather than being
    // carried into the renderer as six more values. That is what keeps the GPU kernel
    // untouched by this feature: the kernel's push constants are these resolved values, so
    // a wheel cannot make the two paths disagree, and no shader needed changing to add it.
    //
    // Each band folds in the way that band is naturally controlled -- lift is an offset so
    // the wheel ADDS to it, gain and gamma are multipliers so the wheel SCALES them. All
    // three are no-ops at amount 0, which is why an old project resolves bit-for-bit as it
    // did before wheels existed.
    const ColorWheelOffsets shadows =
        colorWheelOffsets(get("shadowsHue", 0.0), get("shadowsAmount", 0.0));
    v.liftR += shadows.r * kColorWheelScale;
    v.liftG += shadows.g * kColorWheelScale;
    v.liftB += shadows.b * kColorWheelScale;

    const ColorWheelOffsets midtones =
        colorWheelOffsets(get("midtonesHue", 0.0), get("midtonesAmount", 0.0));
    v.gammaR *= 1.0 + midtones.r * kColorWheelScale;
    v.gammaG *= 1.0 + midtones.g * kColorWheelScale;
    v.gammaB *= 1.0 + midtones.b * kColorWheelScale;

    const ColorWheelOffsets highlights =
        colorWheelOffsets(get("highlightsHue", 0.0), get("highlightsAmount", 0.0));
    v.gainR *= 1.0 + highlights.r * kColorWheelScale;
    v.gainG *= 1.0 + highlights.g * kColorWheelScale;
    v.gainB *= 1.0 + highlights.b * kColorWheelScale;

    return v;
}

[[nodiscard]] inline ColorGradeValues colorGradeValues(const Effect& effect) {
    return colorGradeValues(effect.parameters);
}

// ---------------------------------------------------------------------------
// Denoise (upstream parity: the `denoise` tool category)
// ---------------------------------------------------------------------------

/// The parameter names an EffectType::Denoise effect understands, in presentation order.
inline constexpr std::array<const char*, 2> kDenoiseParameterNames{"strength", "radius"};

/// The largest denoise window half-width, in pixels.
///
/// The filter is O(radius^2) per pixel, so an unbounded radius is a denial of service on
/// a 4K frame rather than a stronger denoise. Clamped rather than rejected, for the same
/// reason blur clamps its own radius: a caller asking for too much gets the strongest
/// filter available instead of an error it cannot act on.
inline constexpr int kMaxDenoiseRadius = 16;

/// The two values an EffectType::Denoise effect renders with, resolved to either the
/// stored value or its default.
///
/// Defined here beside the effect, exactly as ColorGradeValues is and for the same reason:
/// `gpu::applyEffectSoftware`, the GLSL kernel's push constants and the Inspector's
/// displayed numbers must all be the same two values, and three independent copies of a
/// default is how they stop being.
struct DenoiseValues {
    /// Range sigma in normalized luma, clamped to [0,1]. 0 means "do nothing".
    double strength = 0.0;
    /// Window half-width in pixels, clamped to [0, kMaxDenoiseRadius].
    int radius = 1;

    /// No filtering to do, so the effect is an exact no-op.
    [[nodiscard]] bool isIdentity() const noexcept { return strength <= 0.0 || radius <= 0; }
};

/// Resolve a Denoise effect's parameter map to the values it renders with.
[[nodiscard]] inline DenoiseValues denoiseValues(const std::map<std::string, double>& parameters) {
    const auto get = [&parameters](const char* name, double fallback) {
        const auto it = parameters.find(name);
        return it == parameters.end() ? fallback : it->second;
    };
    DenoiseValues v;
    v.strength = get("strength", 0.0);
    if (v.strength < 0.0) v.strength = 0.0;
    if (v.strength > 1.0) v.strength = 1.0;

    const double requested = get("radius", 1.0);
    // Round half away from zero, matching the blur kernel's own radius rounding.
    int radius = static_cast<int>(requested + (requested >= 0 ? 0.5 : -0.5));
    if (radius < 0) radius = 0;
    if (radius > kMaxDenoiseRadius) radius = kMaxDenoiseRadius;
    v.radius = radius;
    return v;
}

[[nodiscard]] inline DenoiseValues denoiseValues(const Effect& effect) {
    return denoiseValues(effect.parameters);
}

// ---------------------------------------------------------------------------
// Secondary (qualifier-masked) grade
// ---------------------------------------------------------------------------

/// The HSL qualifier that decides WHERE an EffectType::SecondaryGrade applies.
///
/// Every default is deliberately the widest possible value, so an unset qualifier selects
/// the whole frame. A qualifier's job is to NARROW a correction; if the defaults selected
/// nothing, adding a secondary grade and then setting a gain would appear to do nothing at
/// all, and the caller would have no way to tell a mis-set qualifier from a broken effect.
struct QualifierValues {
    /// Centre of the selected hue arc, in degrees. Wraps, so 350 and -10 mean the same arc.
    double hueCenter = 0.0;
    /// Total width of the selected hue arc, in degrees. 360 selects every hue.
    double hueWidth = 360.0;
    /// Selected saturation band, as HSV saturation in [0,1].
    double satMin = 0.0;
    double satMax = 1.0;
    /// Selected luma band, as Rec.601 luma in [0,1].
    double lumaMin = 0.0;
    double lumaMax = 1.0;
    /// Feathering outside each band, in [0,1]. 0 gives a hard-edged selection.
    double softness = 0.0;

    /// True when the qualifier admits every pixel at full weight, which is what makes a
    /// default SecondaryGrade equivalent to a ColorGrade.
    [[nodiscard]] bool selectsEverything() const noexcept {
        return hueWidth >= 360.0 && satMin <= 0.0 && satMax >= 1.0 && lumaMin <= 0.0 &&
               lumaMax >= 1.0;
    }
};

/// How strongly the qualifier selects one pixel, in [0,1].
///
/// `r`, `g` and `b` are NORMALIZED [0,1]. The result multiplies the correction, so 0 leaves
/// the pixel untouched and 1 applies the correction in full.
///
/// Lives in core, as an inline function, for two reasons: the software renderer and the
/// Inspector's future mask preview must agree on what is selected, and it can be tested
/// exhaustively without a Compositor or a GPU.
///
/// The hue arithmetic uses a FLOOR-based modulo rather than fmod, because the GLSL twin
/// uses `mod()` -- which floors -- and fmod does not agree with it for negative operands.
/// Saturation is HSV saturation (delta/max), the usual basis for a qualifier, and luma is
/// the same Rec.601 luma the rest of this codebase uses.
[[nodiscard]] inline double qualifierMask(double r, double g, double b,
                                          const QualifierValues& q) noexcept {
    // A band weight: 1 inside [lo,hi], falling linearly to 0 across `feather` outside it.
    const auto band = [](double v, double lo, double hi, double feather) noexcept {
        if (v >= lo && v <= hi) return 1.0;
        const double d = v < lo ? lo - v : v - hi;
        if (feather <= 0.0) return 0.0;
        const double w = 1.0 - d / feather;
        return w < 0.0 ? 0.0 : w;
    };

    const double softness = q.softness < 0.0 ? 0.0 : (q.softness > 1.0 ? 1.0 : q.softness);

    const double maxc = std::max(r, std::max(g, b));
    const double minc = std::min(r, std::min(g, b));
    const double delta = maxc - minc;

    // --- hue ---------------------------------------------------------------
    double hueWeight = 1.0;
    const double width = q.hueWidth < 0.0 ? 0.0 : (q.hueWidth > 360.0 ? 360.0 : q.hueWidth);
    if (width < 360.0) {
        double hue = 0.0;
        if (delta > 0.0) {
            double sector{};
            if (maxc == r) {
                sector = (g - b) / delta;
            } else if (maxc == g) {
                sector = (b - r) / delta + 2.0;
            } else {
                sector = (r - g) / delta + 4.0;
            }
            sector -= 6.0 * std::floor(sector / 6.0);  // GLSL mod(), not fmod()
            hue = 60.0 * sector;
        }
        double centre = q.hueCenter - 360.0 * std::floor(q.hueCenter / 360.0);
        double distance = std::fabs(hue - centre);
        if (distance > 180.0) distance = 360.0 - distance;  // shortest way round the circle
        hueWeight = band(distance, 0.0, width * 0.5, softness * 60.0);
    }
    if (hueWeight <= 0.0) return 0.0;

    // --- saturation and luma ----------------------------------------------
    const double saturation = maxc <= 0.0 ? 0.0 : delta / maxc;
    const double satWeight = band(saturation, q.satMin, q.satMax, softness * 0.5);
    if (satWeight <= 0.0) return 0.0;

    const double luma = 0.299 * r + 0.587 * g + 0.114 * b;
    const double lumaWeight = band(luma, q.lumaMin, q.lumaMax, softness * 0.5);

    return hueWeight * satWeight * lumaWeight;
}

/// The parameter names an EffectType::SecondaryGrade effect understands beyond the ten
/// ColorGrade names, which it shares so that one correction vocabulary serves both.
inline constexpr std::array<const char*, 7> kQualifierParameterNames{
    "hueCenter", "hueWidth", "satMin", "satMax", "lumaMin", "lumaMax", "softness"};

/// The values an EffectType::SecondaryGrade renders with: what to change, and where.
struct SecondaryGradeValues {
    ColorGradeValues correction;
    QualifierValues qualifier;

    /// Nothing to apply anywhere. Keyed on the CORRECTION only: a qualifier that selects
    /// nothing still has to be evaluated per pixel to discover that, whereas an identity
    /// correction is a no-op no matter what the qualifier selects.
    [[nodiscard]] bool isIdentity() const noexcept { return correction.isIdentity(); }
};

/// Resolve a SecondaryGrade effect's parameters into its correction and its qualifier.
[[nodiscard]] inline SecondaryGradeValues secondaryGradeValues(
    const std::map<std::string, double>& parameters) {
    const auto get = [&parameters](const char* name, double fallback) {
        const auto it = parameters.find(name);
        return it == parameters.end() ? fallback : it->second;
    };
    SecondaryGradeValues v;
    // The correction shares ColorGrade's ten names and its legacy-lift fallback.
    v.correction = colorGradeValues(parameters);
    v.qualifier.hueCenter = get("hueCenter", 0.0);
    v.qualifier.hueWidth = get("hueWidth", 360.0);
    v.qualifier.satMin = get("satMin", 0.0);
    v.qualifier.satMax = get("satMax", 1.0);
    v.qualifier.lumaMin = get("lumaMin", 0.0);
    v.qualifier.lumaMax = get("lumaMax", 1.0);
    v.qualifier.softness = get("softness", 0.0);
    return v;
}

[[nodiscard]] inline SecondaryGradeValues secondaryGradeValues(const Effect& effect) {
    return secondaryGradeValues(effect.parameters);
}

}  // namespace palmier

#endif  // PALMIER_CORE_EFFECT_HPP
