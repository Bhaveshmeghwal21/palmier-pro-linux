// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/Clip.hpp — a single media segment placed on a track.
//
// A Clip references a media asset and positions a source range on the timeline
// (design.md Data Models). Its fields:
//   * id            — stable per-clip identity (ClipId).
//   * assetRef      — the media asset this clip draws from (must resolve in
//                     Project.assets).
//   * timelineStart — where the clip begins on the timeline.
//   * sourceIn      — in-point within the source media.
//   * sourceOut     — out-point within the source media (must exceed sourceIn).
//   * effects       — ordered per-clip effects.
//   * transitionIn  — optional incoming transition at the clip's leading edge.
//   * gain          — audio gain; 1.0 = unity, must be >= 0.
//   * opacity       — video opacity in [0, 1].
//
// Validation rules (enforced in ProjectValidation): sourceOut > sourceIn, the clip
// duration equals sourceOut - sourceIn, opacity in [0,1], and gain >= 0.
//
// ClipId is the identity primitive for clips; like Track/Project ids it is a Uuid
// (see Uuid.hpp), exposed as a named alias so signatures such as
// TimelineEngine::clip(ClipId) read intently.

#ifndef PALMIER_CORE_CLIP_HPP
#define PALMIER_CORE_CLIP_HPP

#include <optional>
#include <vector>

#include "core/Duration.hpp"
#include "core/Effect.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/Transition.hpp"
#include "core/Uuid.hpp"

namespace palmier {

/// Stable identity of a clip. Shares the Uuid identity primitive.
using ClipId = Uuid;

struct Clip {
    ClipId                    id;
    MediaAssetRef             assetRef;
    Duration                  timelineStart; ///< Position on the timeline.
    Duration                  sourceIn;      ///< In-point within the source.
    Duration                  sourceOut;     ///< Out-point within the source.
    std::vector<Effect>       effects;
    std::optional<Transition> transitionIn;
    double                    gain = 1.0;    ///< Audio gain; unity = 1.0.
    double                    opacity = 1.0; ///< Video opacity in [0, 1].

    /// Duration occupied on the timeline: sourceOut - sourceIn.
    [[nodiscard]] Duration duration() const noexcept { return sourceOut - sourceIn; }

    /// End position on the timeline: timelineStart + duration().
    [[nodiscard]] Duration timelineEnd() const noexcept { return timelineStart + duration(); }
};

} // namespace palmier

#endif // PALMIER_CORE_CLIP_HPP
