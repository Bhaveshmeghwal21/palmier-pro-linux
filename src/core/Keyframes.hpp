// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/Keyframes.hpp — animating a clip's properties over its length.
//
// Until this existed every per-clip property was constant for the clip's whole duration: an
// opacity, a gain, a placement. Nothing could ramp, drift, push in or move, so any animation had to
// be faked by cutting a clip into pieces and giving each piece a different constant — which loses
// the relationship between them and cannot be adjusted afterwards.
//
// The model is deliberately small: a property is animated by a list of (time, value) pairs, and
// asking for its value at a timeline position either interpolates between the surrounding pair or,
// when the property carries no keyframes at all, answers the clip's own static value. That last
// part is what makes the feature additive — a clip with no keyframes behaves exactly as before, so
// nothing that predates this needs to know it exists.
//
// Times are TIMELINE positions, not offsets into the clip, and so are independent of the clip's
// speed: moving a clip does not move its keyframes relative to the cut, and changing its speed does
// not redistribute them. That is the same convention transitions use, and it keeps a keyframe's
// meaning ("at this moment in the edit") stable under every operation that does not move the clip.

#ifndef PALMIER_CORE_KEYFRAMES_HPP
#define PALMIER_CORE_KEYFRAMES_HPP

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

#include "core/Duration.hpp"

namespace palmier {

/// A per-clip property that can be animated.
///
/// Deliberately a closed set rather than a free-form name: each entry has to be read by the
/// compositor or the audio mix at render time, so a property nothing evaluates would be a keyframe
/// track that silently does nothing — the exact defect class this port has already had to fix
/// twice.
enum class AnimatableProperty {
    Opacity,          ///< Video coverage, clamped to [0, 1] where it is used.
    Gain,             ///< Audio gain, unity = 1.0.
    CenterX,          ///< Transform: picture centre across the canvas.
    CenterY,          ///< Transform: picture centre down the canvas.
    Scale,            ///< Transform: multiplier on the fitted size.
    RotationDegrees,  ///< Transform: clockwise rotation.
};

/// Every animatable property, for callers that need to iterate them (serialization, the tool
/// surface's value set, and the evaluation the compositor does per frame).
[[nodiscard]] const std::vector<AnimatableProperty>& allAnimatableProperties();

/// The stable wire name of `property`, as the project format and the tool surface spell it.
[[nodiscard]] std::string_view animatablePropertyName(AnimatableProperty property) noexcept;

/// The property named by `name`, or nullopt when it names none.
[[nodiscard]] std::optional<AnimatableProperty> parseAnimatableProperty(std::string_view name);

/// How a value travels from one keyframe to the next.
enum class Interpolation {
    /// Constant rate. Correct when the motion should read as mechanical, and the honest default.
    Linear,
    /// Eased at both ends (smoothstep). What a move usually wants so it does not start and stop
    /// abruptly.
    Smooth,
};

/// The wire name of `interpolation`.
[[nodiscard]] std::string_view interpolationName(Interpolation interpolation) noexcept;

/// The interpolation named by `name`, or nullopt when it names none.
[[nodiscard]] std::optional<Interpolation> parseInterpolation(std::string_view name);

/// One point on an animated property.
struct Keyframe {
    /// Timeline position at which the property holds `value`.
    Duration time{Duration::zero()};
    double value{0.0};
    /// How the value travels from HERE to the next keyframe. Held on the earlier keyframe of each
    /// pair because that is the segment it describes; the last keyframe's setting is unused.
    Interpolation interpolation{Interpolation::Linear};

    [[nodiscard]] friend bool operator==(const Keyframe&, const Keyframe&) = default;
};

/// One animated property of one clip.
struct KeyframeTrack {
    AnimatableProperty property{AnimatableProperty::Opacity};
    /// Ascending by time. `sortKeyframes` establishes that; evaluation relies on it.
    std::vector<Keyframe> keyframes;

    [[nodiscard]] bool isEmpty() const noexcept { return keyframes.empty(); }

    [[nodiscard]] friend bool operator==(const KeyframeTrack&, const KeyframeTrack&) = default;
};

/// Sort `keyframes` ascending by time and drop all but the last of any sharing a time.
///
/// Two keyframes at one instant have no consistent meaning — the property cannot hold two values —
/// so rather than refuse the edit, the later wins, which is what a caller re-keying a moment
/// intends. Called on the way in so evaluation can assume order.
void sortKeyframes(std::vector<Keyframe>& keyframes);

/// The value of `track` at `position`.
///
/// Returns nullopt when the track holds no keyframes, which is how a caller distinguishes "not
/// animated" from "animated and currently zero". Before the first keyframe the first value holds,
/// and after the last the last value holds, so a property never jumps to or from an implied zero
/// outside the keyframed span.
[[nodiscard]] std::optional<double> evaluateKeyframeTrack(const KeyframeTrack& track,
                                                          Duration position);

/// The animated value of `property` at `position` among `tracks`, or `fallback` when `property` has
/// no keyframes there.
///
/// Takes the track list rather than a Clip so this header stays independent of Clip.hpp, which
/// includes it.
[[nodiscard]] double evaluateProperty(const std::vector<KeyframeTrack>& tracks,
                                      AnimatableProperty property, Duration position,
                                      double fallback);

/// True when any track in `tracks` holds at least one keyframe.
///
/// Worth asking because the per-frame evaluation can be skipped entirely for the overwhelmingly
/// common unanimated clip.
[[nodiscard]] bool hasAnyKeyframes(const std::vector<KeyframeTrack>& tracks) noexcept;

}  // namespace palmier

#endif  // PALMIER_CORE_KEYFRAMES_HPP
