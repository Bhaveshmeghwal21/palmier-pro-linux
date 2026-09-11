// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/ClipGeometry.hpp — a clip's placement on the canvas and its edge fades.
//
// Two per-clip properties the port had no way to express, both of which upstream Palmier Pro
// carries on every clip:
//
//   * Transform — where the clip sits on the canvas, how large, rotated, and mirrored. Without
//     it a clip could only occupy the whole canvas: no picture-in-picture, no split screen, no
//     rotation, no flip. Composition was limited to stacking full-frame layers and cropping.
//
//   * Fades — a ramp at each edge of the clip, applied to video opacity and to audio gain.
//     Upstream expresses these in frames; this port measures every duration in nanoseconds, so
//     they are Durations here for consistency with the rest of the surface.
//
// A fade differs from a transition even though both ramp coverage: a transition belongs to the
// boundary BETWEEN two clips and is stored on the incoming one, whereas a fade belongs to one
// clip's own edges and needs no neighbour. They compose — a clip may carry both — because
// nothing about either implies the other.
//
// The fade helper lives in the core rather than in the compositor because the audio mix needs
// the identical ramp: a cut that fades its picture but not its sound is a defect, and the only
// way to guarantee they agree is to compute both from one function.

#ifndef PALMIER_CORE_CLIPGEOMETRY_HPP
#define PALMIER_CORE_CLIPGEOMETRY_HPP

#include "core/Duration.hpp"

namespace palmier {

/// Where a clip's picture sits on the canvas.
///
/// The identity transform means "fitted to the canvas preserving aspect ratio and centred",
/// which is what every clip got before this existed, so an identity transform changes nothing.
/// `scale` multiplies that fitted size, so 0.5 is a half-size inset and 1.0 fills the frame.
///
/// Scale is uniform. Upstream carries independent `width` and `height` instead, which also
/// permits a non-uniform stretch; a single factor cannot distort a source by accident, and a
/// split-screen half is expressed here by cropping the source and positioning what remains.
struct Transform {
    /// Centre of the picture in normalized canvas coordinates; 0.5, 0.5 is the middle.
    double centerX = 0.5;
    double centerY = 0.5;
    /// Multiplies the aspect-preserving fitted size. Must be > 0.
    double scale = 1.0;
    /// Clockwise rotation about the picture's own centre, in degrees.
    double rotationDegrees = 0.0;
    /// Mirror about the vertical / horizontal axis, applied before rotation.
    bool flipHorizontal = false;
    bool flipVertical = false;

    /// True when this transform places the clip exactly where the plain fit would.
    ///
    /// Worth asking because the identity case can take the cheaper axis-aligned sampling path,
    /// and because it is the overwhelmingly common one.
    [[nodiscard]] bool isIdentity() const noexcept {
        return centerX == 0.5 && centerY == 0.5 && scale == 1.0 && rotationDegrees == 0.0 &&
               !flipHorizontal && !flipVertical;
    }

    /// True when the placement is a pure translate/scale, so no rotation or mirroring is needed.
    [[nodiscard]] bool isAxisAligned() const noexcept {
        return rotationDegrees == 0.0 && !flipHorizontal && !flipVertical;
    }

    [[nodiscard]] friend bool operator==(const Transform&, const Transform&) = default;
};

/// The coverage multiplier a clip's own edge fades contribute at `position`.
///
/// Returns 1.0 outside both ramps, rises linearly from 0.0 across `fadeIn` at the clip's leading
/// edge, and falls to 0.0 across `fadeOut` at its trailing edge. A fade longer than the clip is
/// clamped so the two ramps cannot each claim more than half of it, which keeps the result
/// monotonic instead of producing a clip that never reaches full coverage in a surprising way.
///
/// `start` and `end` are the clip's timeline bounds; `position` need not lie between them, and
/// anything outside returns 0.0 so a caller cannot accidentally brighten a clip that is not
/// present.
[[nodiscard]] double fadeCoverage(Duration start, Duration end, Duration fadeIn, Duration fadeOut,
                                  Duration position) noexcept;

}  // namespace palmier

#endif  // PALMIER_CORE_CLIPGEOMETRY_HPP
