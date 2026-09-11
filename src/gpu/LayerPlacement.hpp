// SPDX-License-Identifier: GPL-3.0-or-later
//
// gpu/LayerPlacement.hpp — where one layer lands on the canvas for a single frame.
//
// Two concerns the compositor's blend stage needs and previously had nowhere to get.
//
//   * Transitions. core/Transition.hpp defines a transition as "the blend applied over an
//     explicit region at the clip's leading edge", and says that region is the one place
//     adjacent clips on a track are permitted to overlap. Turning that region into per-layer
//     blend weights is pure arithmetic over a position and a Transition, so it lives here
//     where it is testable without a GPU, a decoder or a frame pool.
//
//   * Fitting. A source frame is whatever size its media is; the canvas is whatever the
//     project says. Mapping one onto the other is likewise pure arithmetic. Before this
//     existed the blend stage aligned a source to the canvas's top-left and clipped it, so a
//     3840x2160 source on a 1920x1080 canvas showed only its top-left quarter — and both
//     Compositor::blendOver and applyCropTransform carried a comment saying the *other* one
//     was responsible for scaling.
//
// Everything here is a free function over plain values so the arithmetic can be pinned down
// exactly, which matters because a transition that silently does nothing is indistinguishable
// from a hard cut in anything short of a per-frame measurement.

#ifndef PALMIER_GPU_LAYERPLACEMENT_HPP
#define PALMIER_GPU_LAYERPLACEMENT_HPP

#include <cstdint>
#include <optional>
#include <vector>

#include "core/Clip.hpp"
#include "core/Duration.hpp"

namespace palmier::gpu {

/// The rectangle a source frame occupies on the canvas once fitted to it.
///
/// `x`/`y` are signed because a slide transition offsets the rectangle off-canvas, and
/// because a fitted rectangle is centred (so it is negative only when a caller has shifted
/// it). Width and height are the on-canvas extent, not the source's own size.
struct FittedRect {
    std::int64_t x{0};
    std::int64_t y{0};
    std::uint32_t width{0};
    std::uint32_t height{0};

    [[nodiscard]] bool isEmpty() const noexcept { return width == 0 || height == 0; }
};

/// Fit a `srcWidth` x `srcHeight` source into a `canvasWidth` x `canvasHeight` canvas,
/// preserving the source's aspect ratio and centring it — so a wider-than-canvas source is
/// letterboxed and a taller one is pillarboxed. Scales up as well as down, because a project
/// whose canvas exceeds its media should fill the frame rather than pad it.
///
/// Returns an empty rectangle when any dimension is zero.
[[nodiscard]] FittedRect fitToCanvas(std::uint32_t srcWidth, std::uint32_t srcHeight,
                                     std::uint32_t canvasWidth,
                                     std::uint32_t canvasHeight) noexcept;

/// How one layer is blended onto the canvas for a single frame.
///
/// The identity placement draws the layer fitted, fully covering, with no wipe or slide —
/// which is what every layer outside a transition region gets.
struct LayerPlacement {
    /// Uniform coverage multiplier, already including the clip's own opacity and its fades.
    double opacity{1.0};
    /// Wipe: the fraction of the canvas width revealed from the left edge. 1.0 reveals all.
    double reveal{1.0};
    /// Slide: horizontal offset as a fraction of the canvas width. 0.0 is aligned.
    double slideX{0.0};
    /// Where the layer's picture sits on the canvas.
    Transform transform{};
};

/// The inverse mapping from a canvas pixel to a source pixel for one placed layer.
///
/// Expressing placement as an inverse map rather than a forward blit is what lets scale,
/// rotation and mirroring share a single sampling loop: every canvas pixel in `left..right` x
/// `top..bottom` asks "which source pixel, if any, lands here", which is well defined however
/// the picture is oriented. A forward mapping would have to worry about coverage gaps as soon as
/// anything was rotated or magnified.
///
/// `u = ux*x + uy*y + uc` and `v = vx*x + vy*y + vc`, where x and y are canvas pixel CENTRES and
/// u and v are source pixel centres.
struct InverseMap {
    double ux{1.0};
    double uy{0.0};
    double uc{0.0};
    double vx{0.0};
    double vy{1.0};
    double vc{0.0};

    /// The half-open canvas box worth iterating: outside it no source pixel lands.
    std::int64_t left{0};
    std::int64_t top{0};
    std::int64_t right{0};
    std::int64_t bottom{0};

    /// Source pixels covered per canvas pixel on each axis, for choosing between averaging the
    /// footprint (minifying) and interpolating within it (magnifying).
    double sourcePerCanvasX{1.0};
    double sourcePerCanvasY{1.0};

    [[nodiscard]] bool isEmpty() const noexcept { return right <= left || bottom <= top; }
};

/// Build the inverse map placing a `srcWidth` x `srcHeight` source onto a canvas under
/// `transform`, with `slideX` shifting it horizontally as a fraction of the canvas width.
///
/// The identity transform reproduces fitToCanvas exactly, and when the source already matches the
/// canvas the map is the identity, so an untransformed clip samples at integer coordinates and is
/// bit-identical to a straight blit.
[[nodiscard]] InverseMap inverseMapFor(std::uint32_t srcWidth, std::uint32_t srcHeight,
                                       std::uint32_t canvasWidth, std::uint32_t canvasHeight,
                                       const Transform& transform, double slideX) noexcept;

/// A clip selected for compositing, tagged with its painter's-order depth.
///
/// `z` is the clip's track index in Project.tracks, so layers sharing a `z` share a track —
/// which is how a transition finds the outgoing clip it blends against.
struct PlacedLayer {
    const Clip* clip{nullptr};
    std::size_t z{0};
};

/// Progress through `clip`'s incoming transition region: 0.0 at the region's leading edge
/// rising to 1.0 at its end.
///
/// Returns nullopt when the clip carries no transition, when the transition's duration is
/// not positive, or when `position` lies outside the region — i.e. whenever the frame should
/// be composited normally.
[[nodiscard]] std::optional<double> transitionProgress(const Clip& clip,
                                                       Duration position) noexcept;

/// The placement of every layer for one frame, applying any transition whose region the
/// frame falls inside.
///
/// `layers` must already be in painter's order (ascending z, and ascending timelineStart
/// within a z) because a transition blends against the layer drawn immediately before it on
/// the same track. The result is parallel to `layers`.
///
/// Per kind, where p is the progress through the region:
///
///   * Crossfade / Fade — the incoming layer's coverage ramps 0 -> 1 across the region. With
///     an overlapping outgoing clip beneath this is a cross-dissolve; with nothing beneath it
///     is a fade up from the cleared canvas. The two kinds coincide by construction: the
///     model carries one duration and one region, and "dissolve into whatever is behind" is
///     the honest reading of both names.
///   * DipToColor — the outgoing layer fades out over the first half of the region and the
///     incoming rises over the second, so the frame passes through the cleared canvas at the
///     midpoint. Transition carries no colour field, so the dip is to whatever the render
///     target's clear colour is.
///   * Wipe — the incoming layer is revealed behind a hard vertical edge travelling left to
///     right.
///   * Slide — the incoming layer travels in from the right edge, fully covering.
[[nodiscard]] std::vector<LayerPlacement> layerPlacements(const std::vector<PlacedLayer>& layers,
                                                          Duration position);

}  // namespace palmier::gpu

#endif  // PALMIER_GPU_LAYERPLACEMENT_HPP
