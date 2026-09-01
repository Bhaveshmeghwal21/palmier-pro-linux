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

#include <array>
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
    Custom,
};

struct Effect {
    Uuid                        id;         ///< Stable per-effect identity.
    EffectType                  type = EffectType::Custom;
    std::map<std::string, double> parameters; ///< Named scalar parameters.

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
inline constexpr std::array<const char*, 10> kColorGradeParameterNames{
    "liftR", "liftG", "liftB", "gammaR", "gammaG", "gammaB",
    "gainR", "gainG", "gainB", "saturation"};

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
    return v;
}

[[nodiscard]] inline ColorGradeValues colorGradeValues(const Effect& effect) {
    return colorGradeValues(effect.parameters);
}

} // namespace palmier

#endif // PALMIER_CORE_EFFECT_HPP
