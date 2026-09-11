// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/ClipGeometry.cpp — the fade arithmetic declared in the header.

#include "core/ClipGeometry.hpp"

#include <algorithm>

namespace palmier {

double fadeCoverage(Duration start, Duration end, Duration fadeIn, Duration fadeOut,
                    Duration position) noexcept {
    const std::int64_t s = start.ticks();
    const std::int64_t e = end.ticks();
    const std::int64_t p = position.ticks();
    if (e <= s) return 0.0;           // an empty clip covers nothing
    if (p < s || p >= e) return 0.0;  // outside the clip entirely

    const std::int64_t length = e - s;
    // Neither ramp may exceed half the clip, so a fade longer than the clip degrades to a
    // symmetric in-and-out rather than to a clip that never reaches full coverage.
    const std::int64_t limit = length / 2;
    const std::int64_t in = std::clamp(fadeIn.ticks(), std::int64_t{0}, limit);
    const std::int64_t out = std::clamp(fadeOut.ticks(), std::int64_t{0}, limit);

    double coverage = 1.0;
    if (in > 0 && p - s < in) {
        coverage *= static_cast<double>(p - s) / static_cast<double>(in);
    }
    if (out > 0 && e - p <= out) {
        // e - p runs from `out` down to 1 across the trailing ramp, so this reaches 1/out at the
        // clip's last tick rather than 0: the final frame is still faintly present, which is what
        // makes a fade-out look complete instead of cutting on its last frame.
        coverage *= static_cast<double>(e - p) / static_cast<double>(out);
    }
    return std::clamp(coverage, 0.0, 1.0);
}

}  // namespace palmier
