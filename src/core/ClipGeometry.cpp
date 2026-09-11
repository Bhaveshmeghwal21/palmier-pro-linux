// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/ClipGeometry.cpp — the fade arithmetic declared in the header.

#include "core/ClipGeometry.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace palmier {

std::vector<Transform> layoutTransforms(LayoutKind kind, std::size_t count) {
    std::vector<Transform> out;
    if (count == 0) return out;
    out.resize(count);

    switch (kind) {
        case LayoutKind::SideBySide: {
            // Equal columns across the middle. One clip is left untouched rather than shrunk to a
            // full-width column, because "side by side" with a single participant means nothing
            // and filling the frame is the only sensible reading.
            if (count == 1) return out;
            const double scale = 1.0 / static_cast<double>(count);
            for (std::size_t i = 0; i < count; ++i) {
                out[i].scale = scale;
                out[i].centerX = (static_cast<double>(i) + 0.5) * scale;
                out[i].centerY = 0.5;
            }
            return out;
        }

        case LayoutKind::PictureInPicture: {
            // The first clip is the programme and keeps the frame; the rest are insets at a
            // quarter size, walking the corners clockwise from the top right. A fifth clip would
            // land back on the first corner, so the corners repeat rather than shrinking further:
            // an unreadably small inset is worse than an overlapping one, and the caller can place
            // extras itself.
            constexpr double kInset = 0.25;
            constexpr double kMargin = 0.03;
            const double offset = kInset / 2.0 + kMargin;
            const double corners[4][2] = {{1.0 - offset, offset},
                                          {1.0 - offset, 1.0 - offset},
                                          {offset, 1.0 - offset},
                                          {offset, offset}};
            for (std::size_t i = 1; i < count; ++i) {
                const auto* corner = corners[(i - 1) % 4];
                out[i].scale = kInset;
                out[i].centerX = corner[0];
                out[i].centerY = corner[1];
            }
            return out;
        }

        case LayoutKind::Grid: {
            if (count == 1) return out;
            // The squarest grid that holds them all: columns = ceil(sqrt(count)).
            auto columns =
                static_cast<std::size_t>(std::ceil(std::sqrt(static_cast<double>(count))));
            if (columns == 0) columns = 1;
            const std::size_t rows = (count + columns - 1) / columns;
            const double cellW = 1.0 / static_cast<double>(columns);
            const double cellH = 1.0 / static_cast<double>(rows);
            // One uniform scale for every cell so no clip is larger than another; the smaller of
            // the two cell dimensions is what fits.
            const double scale = std::min(cellW, cellH);
            for (std::size_t i = 0; i < count; ++i) {
                const std::size_t column = i % columns;
                const std::size_t row = i / columns;
                out[i].scale = scale;
                out[i].centerX = (static_cast<double>(column) + 0.5) * cellW;
                out[i].centerY = (static_cast<double>(row) + 0.5) * cellH;
            }
            return out;
        }
    }
    return out;
}

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
