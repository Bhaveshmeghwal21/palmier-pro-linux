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
    Custom,
};

struct Effect {
    Uuid                        id;         ///< Stable per-effect identity.
    EffectType                  type = EffectType::Custom;
    std::map<std::string, double> parameters; ///< Named scalar parameters.

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

} // namespace palmier

#endif // PALMIER_CORE_EFFECT_HPP
