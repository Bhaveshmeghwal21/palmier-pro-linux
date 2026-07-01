// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/Transition.hpp — an incoming transition attached to a clip.
//
// A Clip may carry an optional transitionIn (design.md Data Models). A transition
// defines the blend applied over an explicit region at the clip's leading edge;
// this region is the one place adjacent clips on a track are permitted to overlap
// (design.md Track validation: "non-overlapping outside explicit transition
// regions"). Transitions are rendered as SPIR-V effect kernels like other effects
// (design.md "Effects as SPIR-V compute kernels"). This type is the serializable
// description: a stable id, a kind, and the transition's duration.

#ifndef PALMIER_CORE_TRANSITION_HPP
#define PALMIER_CORE_TRANSITION_HPP

#include "core/Duration.hpp"
#include "core/Uuid.hpp"

namespace palmier {

/// The kind of blend a Transition performs over its region.
enum class TransitionKind {
    Crossfade,
    DipToColor,
    Wipe,
    Slide,
    Fade,
};

struct Transition {
    Uuid           id;       ///< Stable per-transition identity.
    TransitionKind kind = TransitionKind::Crossfade;
    Duration       duration; ///< Length of the transition region.

    Transition() = default;
    Transition(Uuid id_, TransitionKind kind_, Duration duration_)
        : id(id_), kind(kind_), duration(duration_) {}

    /// A transition region must have a non-negative length.
    [[nodiscard]] bool isValid() const noexcept { return !duration.isNegative(); }
};

} // namespace palmier

#endif // PALMIER_CORE_TRANSITION_HPP
