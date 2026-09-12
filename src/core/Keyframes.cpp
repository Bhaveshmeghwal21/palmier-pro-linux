// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/Keyframes.cpp — the keyframe arithmetic declared in the header.

#include "core/Keyframes.hpp"

#include <algorithm>

namespace palmier {
namespace {

/// Smoothstep: eased at both ends, with zero derivative at 0 and 1.
[[nodiscard]] double smoothstep(double t) noexcept { return t * t * (3.0 - 2.0 * t); }

}  // namespace

const std::vector<AnimatableProperty>& allAnimatableProperties() {
    static const std::vector<AnimatableProperty> all = {
        AnimatableProperty::Opacity, AnimatableProperty::Gain,
        AnimatableProperty::CenterX, AnimatableProperty::CenterY,
        AnimatableProperty::Scale,   AnimatableProperty::RotationDegrees};
    return all;
}

std::string_view animatablePropertyName(AnimatableProperty property) noexcept {
    switch (property) {
        case AnimatableProperty::Opacity:
            return "opacity";
        case AnimatableProperty::Gain:
            return "gain";
        case AnimatableProperty::CenterX:
            return "centerX";
        case AnimatableProperty::CenterY:
            return "centerY";
        case AnimatableProperty::Scale:
            return "scale";
        case AnimatableProperty::RotationDegrees:
            return "rotationDegrees";
    }
    return "opacity";
}

std::optional<AnimatableProperty> parseAnimatableProperty(std::string_view name) {
    for (const AnimatableProperty candidate : allAnimatableProperties()) {
        if (animatablePropertyName(candidate) == name) return candidate;
    }
    return std::nullopt;
}

std::string_view interpolationName(Interpolation interpolation) noexcept {
    return interpolation == Interpolation::Smooth ? "smooth" : "linear";
}

std::optional<Interpolation> parseInterpolation(std::string_view name) {
    if (name == "linear") return Interpolation::Linear;
    if (name == "smooth") return Interpolation::Smooth;
    return std::nullopt;
}

void sortKeyframes(std::vector<Keyframe>& keyframes) {
    // Stable, so that among keyframes sharing a time the LAST one given survives the dedupe below —
    // which is what a caller re-keying an instant means by sending it again.
    std::stable_sort(keyframes.begin(), keyframes.end(),
                     [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });

    // Keep the last of each run of equal times.
    std::vector<Keyframe> deduped;
    deduped.reserve(keyframes.size());
    for (std::size_t i = 0; i < keyframes.size(); ++i) {
        const bool lastOfItsTime =
            i + 1 == keyframes.size() || keyframes[i + 1].time != keyframes[i].time;
        if (lastOfItsTime) deduped.push_back(keyframes[i]);
    }
    keyframes = std::move(deduped);
}

std::optional<double> evaluateKeyframeTrack(const KeyframeTrack& track, Duration position) {
    if (track.keyframes.empty()) return std::nullopt;
    if (track.keyframes.size() == 1) return track.keyframes.front().value;

    // Outside the keyframed span the nearest value holds, rather than the property falling to an
    // implied zero: a clip keyframed only over its first second should stay where the last keyframe
    // left it for the rest of its length.
    if (position <= track.keyframes.front().time) return track.keyframes.front().value;
    if (position >= track.keyframes.back().time) return track.keyframes.back().value;

    // The first keyframe at or after `position`. The bounds above guarantee this is neither begin()
    // nor end(), so `previous` is always valid.
    const auto next =
        std::lower_bound(track.keyframes.begin(), track.keyframes.end(), position,
                         [](const Keyframe& frame, Duration p) { return frame.time < p; });
    const auto previous = std::prev(next);

    const std::int64_t span = next->time.ticks() - previous->time.ticks();
    if (span <= 0) return next->value;  // coincident times; sortKeyframes should have removed these

    const double t =
        static_cast<double>(position.ticks() - previous->time.ticks()) / static_cast<double>(span);
    // The segment's shape is held on its EARLIER keyframe, so `previous` decides.
    const double eased = previous->interpolation == Interpolation::Smooth
                             ? smoothstep(std::clamp(t, 0.0, 1.0))
                             : std::clamp(t, 0.0, 1.0);
    return previous->value + (next->value - previous->value) * eased;
}

double evaluateProperty(const std::vector<KeyframeTrack>& tracks, AnimatableProperty property,
                        Duration position, double fallback) {
    for (const KeyframeTrack& track : tracks) {
        if (track.property != property) continue;
        if (const std::optional<double> value = evaluateKeyframeTrack(track, position)) {
            return *value;
        }
    }
    return fallback;
}

bool hasAnyKeyframes(const std::vector<KeyframeTrack>& tracks) noexcept {
    return std::any_of(tracks.begin(), tracks.end(),
                       [](const KeyframeTrack& track) { return !track.isEmpty(); });
}

}  // namespace palmier
