// SPDX-License-Identifier: GPL-3.0-or-later
//
// gpu/LayerPlacement.cpp — the transition and fitting arithmetic declared in the header.

#include "gpu/LayerPlacement.hpp"

#include <algorithm>
#include <cmath>

namespace palmier::gpu {

FittedRect fitToCanvas(std::uint32_t srcWidth, std::uint32_t srcHeight,
                       std::uint32_t canvasWidth, std::uint32_t canvasHeight) noexcept {
    if (srcWidth == 0 || srcHeight == 0 || canvasWidth == 0 || canvasHeight == 0) {
        return FittedRect{};
    }

    // The largest uniform scale that keeps the source inside the canvas on both axes. Using
    // the smaller of the two ratios is what preserves aspect ratio and produces the letter-
    // or pillarbox; the larger would crop, which is what the top-left blit effectively did.
    const double scale = std::min(static_cast<double>(canvasWidth) / srcWidth,
                                  static_cast<double>(canvasHeight) / srcHeight);

    // At least one pixel on each axis: a source far narrower than the canvas's aspect ratio
    // must still contribute something rather than round away to nothing.
    const auto width = static_cast<std::uint32_t>(
        std::max(1.0, std::min(static_cast<double>(canvasWidth),
                               std::round(static_cast<double>(srcWidth) * scale))));
    const auto height = static_cast<std::uint32_t>(
        std::max(1.0, std::min(static_cast<double>(canvasHeight),
                               std::round(static_cast<double>(srcHeight) * scale))));

    return FittedRect{(static_cast<std::int64_t>(canvasWidth) - width) / 2,
                      (static_cast<std::int64_t>(canvasHeight) - height) / 2, width, height};
}

std::optional<double> transitionProgress(const Clip& clip, Duration position) noexcept {
    if (!clip.transitionIn.has_value()) return std::nullopt;

    const Duration duration = clip.transitionIn->duration;
    if (duration.ticks() <= 0) return std::nullopt;  // a zero-length region blends nothing

    const std::int64_t into = position.ticks() - clip.timelineStart.ticks();
    if (into < 0 || into >= duration.ticks()) return std::nullopt;

    return static_cast<double>(into) / static_cast<double>(duration.ticks());
}

std::vector<LayerPlacement> layerPlacements(const std::vector<PlacedLayer>& layers,
                                            Duration position) {
    std::vector<LayerPlacement> placements(layers.size());

    for (std::size_t i = 0; i < layers.size(); ++i) {
        const Clip* clip = layers[i].clip;
        if (clip == nullptr) continue;

        placements[i].opacity = clip->opacity;

        const std::optional<double> progress = transitionProgress(*clip, position);
        if (!progress.has_value()) continue;
        const double p = *progress;

        // The outgoing layer is the one drawn immediately before this on the same track. It is
        // absent when the incoming clip has no overlapping neighbour, which is legal: a
        // transition at the head of a track fades up from the cleared canvas.
        const bool hasOutgoing = i > 0 && layers[i - 1].z == layers[i].z;

        switch (clip->transitionIn->kind) {
            case TransitionKind::Crossfade:
            case TransitionKind::Fade:
                placements[i].opacity = clip->opacity * p;
                break;

            case TransitionKind::DipToColor:
                // Second half only for the incoming clip, so the midpoint of the region is
                // the clear colour rather than a half-and-half mix of the two shots.
                placements[i].opacity = clip->opacity * std::max(0.0, 2.0 * p - 1.0);
                if (hasOutgoing) {
                    const Clip* outgoing = layers[i - 1].clip;
                    const double outgoingOpacity = outgoing != nullptr ? outgoing->opacity : 1.0;
                    placements[i - 1].opacity = outgoingOpacity * std::max(0.0, 1.0 - 2.0 * p);
                }
                break;

            case TransitionKind::Wipe:
                placements[i].reveal = p;
                break;

            case TransitionKind::Slide:
                // 1.0 at the region's start puts the layer one full canvas width to the right,
                // i.e. entirely off screen, and it arrives aligned as p reaches 1.
                placements[i].slideX = 1.0 - p;
                break;
        }
    }

    return placements;
}

} // namespace palmier::gpu
