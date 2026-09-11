// SPDX-License-Identifier: GPL-3.0-or-later
//
// gpu/LayerPlacement.cpp — the transition and fitting arithmetic declared in the header.

#include "gpu/LayerPlacement.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace palmier::gpu {

FittedRect fitToCanvas(std::uint32_t srcWidth, std::uint32_t srcHeight, std::uint32_t canvasWidth,
                       std::uint32_t canvasHeight) noexcept {
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

InverseMap inverseMapFor(std::uint32_t srcWidth, std::uint32_t srcHeight, std::uint32_t canvasWidth,
                         std::uint32_t canvasHeight, const Transform& transform,
                         double slideX) noexcept {
    InverseMap map;
    if (srcWidth == 0 || srcHeight == 0 || canvasWidth == 0 || canvasHeight == 0) return map;

    const FittedRect fit = fitToCanvas(srcWidth, srcHeight, canvasWidth, canvasHeight);
    if (fit.isEmpty()) return map;

    const double scale = transform.scale > 0.0 ? transform.scale : 0.0;
    if (scale <= 0.0) return map;  // a non-positive scale places nothing

    // Canvas pixels per source pixel on each axis. Equal for an aspect-preserving fit; kept
    // separate so a future non-uniform transform needs no change here.
    const double kx = static_cast<double>(fit.width) * scale / static_cast<double>(srcWidth);
    const double ky = static_cast<double>(fit.height) * scale / static_cast<double>(srcHeight);
    if (kx <= 0.0 || ky <= 0.0) return map;

    const double fx = transform.flipHorizontal ? -1.0 : 1.0;
    const double fy = transform.flipVertical ? -1.0 : 1.0;

    const double radians = transform.rotationDegrees * std::numbers::pi / 180.0;
    const double cosT = std::cos(radians);
    const double sinT = std::sin(radians);

    const double cx = (transform.centerX + slideX) * static_cast<double>(canvasWidth);
    const double cy = transform.centerY * static_cast<double>(canvasHeight);
    const double halfSrcW = static_cast<double>(srcWidth) / 2.0;
    const double halfSrcH = static_cast<double>(srcHeight) / 2.0;

    // Forward, for a source pixel centre (u, v):
    //   su = (u - halfSrcW) * fx * kx        sv = (v - halfSrcH) * fy * ky
    //   x  = cx + su*cosT - sv*sinT          y  = cy + su*sinT + sv*cosT
    // Inverting the rotation and the scale gives the affine map below.
    const double invFxKx = 1.0 / (fx * kx);
    const double invFyKy = 1.0 / (fy * ky);

    map.ux = cosT * invFxKx;
    map.uy = sinT * invFxKx;
    map.uc = -(cx * cosT + cy * sinT) * invFxKx + halfSrcW;
    map.vx = -sinT * invFyKy;
    map.vy = cosT * invFyKy;
    map.vc = (cx * sinT - cy * cosT) * invFyKy + halfSrcH;

    map.sourcePerCanvasX = std::abs(1.0 / kx);
    map.sourcePerCanvasY = std::abs(1.0 / ky);

    // The canvas box to iterate: the forward image of the source's four corners, widened by a
    // pixel so a partially covered edge pixel is still visited, then clipped to the canvas.
    const double corners[4][2] = {{0.0, 0.0},
                                  {static_cast<double>(srcWidth), 0.0},
                                  {0.0, static_cast<double>(srcHeight)},
                                  {static_cast<double>(srcWidth), static_cast<double>(srcHeight)}};
    double minX = 0.0;
    double maxX = 0.0;
    double minY = 0.0;
    double maxY = 0.0;
    for (int i = 0; i < 4; ++i) {
        const double su = (corners[i][0] - halfSrcW) * fx * kx;
        const double sv = (corners[i][1] - halfSrcH) * fy * ky;
        const double x = cx + su * cosT - sv * sinT;
        const double y = cy + su * sinT + sv * cosT;
        if (i == 0) {
            minX = maxX = x;
            minY = maxY = y;
        } else {
            minX = std::min(minX, x);
            maxX = std::max(maxX, x);
            minY = std::min(minY, y);
            maxY = std::max(maxY, y);
        }
    }

    map.left = std::max<std::int64_t>(0, static_cast<std::int64_t>(std::floor(minX)) - 1);
    map.top = std::max<std::int64_t>(0, static_cast<std::int64_t>(std::floor(minY)) - 1);
    map.right = std::min<std::int64_t>(static_cast<std::int64_t>(canvasWidth),
                                       static_cast<std::int64_t>(std::ceil(maxX)) + 1);
    map.bottom = std::min<std::int64_t>(static_cast<std::int64_t>(canvasHeight),
                                        static_cast<std::int64_t>(std::ceil(maxY)) + 1);
    return map;
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

        // A clip's own edge fades multiply its opacity. They are independent of any transition:
        // a transition belongs to the boundary between two clips, a fade to one clip's own edges,
        // and a clip may carry both.
        placements[i].opacity =
            clip->opacity * fadeCoverage(clip->timelineStart, clip->timelineEnd(), clip->fadeIn,
                                         clip->fadeOut, position);
        placements[i].transform = clip->transform;

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
                placements[i].opacity *= p;
                break;

            case TransitionKind::DipToColor:
                // Second half only for the incoming clip, so the midpoint of the region is
                // the clear colour rather than a half-and-half mix of the two shots.
                placements[i].opacity *= std::max(0.0, 2.0 * p - 1.0);
                if (hasOutgoing) {
                    placements[i - 1].opacity *= std::max(0.0, 1.0 - 2.0 * p);
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

}  // namespace palmier::gpu
