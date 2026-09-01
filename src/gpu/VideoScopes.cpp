// SPDX-License-Identifier: GPL-3.0-or-later
//
// gpu/VideoScopes.cpp — see the header for the colour convention and why empty is a
// value rather than an error.

#include "gpu/VideoScopes.hpp"

#include <algorithm>

namespace palmier::gpu {

namespace {

/// Round a real level to a byte, clamping. Deliberately the same shape as Compositor's
/// own toByte: a scope that rounded differently from the renderer would disagree with
/// the picture at the extremes, which is where a colourist looks hardest.
std::uint8_t toLevel(double v) noexcept {
    if (v <= 0.0) return 0;
    if (v >= 255.0) return 255;
    return static_cast<std::uint8_t>(v + 0.5);
}

/// Rec.601 luma before rounding, so Cb and Cr are derived from the same value the luma
/// bin uses rather than from a re-rounded one.
double luma601(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept {
    return 0.299 * static_cast<double>(r) + 0.587 * static_cast<double>(g) +
           0.114 * static_cast<double>(b);
}

/// Whether a buffer describes a frame at all. Requirement 6.6's empty state.
bool hasFrame(const std::uint8_t* rgba, int width, int height) noexcept {
    return rgba != nullptr && width > 0 && height > 0;
}

std::uint32_t peakOf(const std::uint32_t* first, std::size_t count) noexcept {
    std::uint32_t peak = 0;
    for (std::size_t i = 0; i < count; ++i) {
        peak = std::max(peak, first[i]);
    }
    return peak;
}

}  // namespace

std::uint8_t scopeLuma(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept {
    return toLevel(luma601(r, g, b));
}

std::uint8_t scopeCb(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept {
    return toLevel(128.0 + 0.564 * (static_cast<double>(b) - luma601(r, g, b)));
}

std::uint8_t scopeCr(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept {
    return toLevel(128.0 + 0.713 * (static_cast<double>(r) - luma601(r, g, b)));
}

// ---------------------------------------------------------------------------
// Histogram
// ---------------------------------------------------------------------------

std::uint32_t Histogram::peakCount() const noexcept {
    return std::max(std::max(peakOf(red.data(), red.size()), peakOf(green.data(), green.size())),
                    std::max(peakOf(blue.data(), blue.size()), peakOf(luma.data(), luma.size())));
}

Histogram computeHistogram(const std::uint8_t* rgba, int width, int height) {
    Histogram out;
    if (!hasFrame(rgba, width, height)) {
        return out;  // empty, not black
    }
    const std::size_t pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    for (std::size_t i = 0; i < pixels; ++i) {
        const std::uint8_t r = rgba[i * 4 + 0];
        const std::uint8_t g = rgba[i * 4 + 1];
        const std::uint8_t b = rgba[i * 4 + 2];
        ++out.red[r];
        ++out.green[g];
        ++out.blue[b];
        ++out.luma[scopeLuma(r, g, b)];
    }
    out.sampleCount = pixels;
    return out;
}

// ---------------------------------------------------------------------------
// Luma waveform
// ---------------------------------------------------------------------------

std::uint32_t LumaWaveform::at(int column, int level) const noexcept {
    if (column < 0 || column >= columns || level < 0 || level >= kScopeLevels) {
        return 0;
    }
    return counts[static_cast<std::size_t>(column) * kScopeLevels + static_cast<std::size_t>(level)];
}

std::uint32_t LumaWaveform::peakCount() const noexcept {
    return counts.empty() ? 0u : peakOf(counts.data(), counts.size());
}

std::uint8_t LumaWaveform::minLuma() const noexcept {
    for (int level = 0; level < kScopeLevels; ++level) {
        for (int column = 0; column < columns; ++column) {
            if (at(column, level) > 0) return static_cast<std::uint8_t>(level);
        }
    }
    return 0;
}

std::uint8_t LumaWaveform::maxLuma() const noexcept {
    for (int level = kScopeLevels - 1; level >= 0; --level) {
        for (int column = 0; column < columns; ++column) {
            if (at(column, level) > 0) return static_cast<std::uint8_t>(level);
        }
    }
    return 0;
}

LumaWaveform computeLumaWaveform(const std::uint8_t* rgba, int width, int height, int columns) {
    LumaWaveform out;
    if (!hasFrame(rgba, width, height) || columns <= 0) {
        return out;
    }
    // Never more traces than there are pixel columns: an empty trace between populated
    // ones would read as a gap in the picture that is not there.
    out.columns = std::min(columns, width);
    out.counts.assign(static_cast<std::size_t>(out.columns) * kScopeLevels, 0u);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t i =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                 static_cast<std::size_t>(x)) * 4;
            const std::uint8_t level = scopeLuma(rgba[i + 0], rgba[i + 1], rgba[i + 2]);
            // Bucket from the pixel's own x, not from an accumulated counter: the same
            // discipline the peak-envelope bucketing needed, and for the same reason --
            // an accumulator drifts and the drift is invisible in the output.
            int column = static_cast<int>(static_cast<std::int64_t>(x) * out.columns / width);
            column = std::min(column, out.columns - 1);
            ++out.counts[static_cast<std::size_t>(column) * kScopeLevels +
                         static_cast<std::size_t>(level)];
        }
    }
    out.sampleCount = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
    return out;
}

// ---------------------------------------------------------------------------
// Vectorscope
// ---------------------------------------------------------------------------

std::uint32_t Vectorscope::at(int cb, int cr) const noexcept {
    if (bins.empty() || cb < 0 || cb >= kScopeLevels || cr < 0 || cr >= kScopeLevels) {
        return 0;
    }
    return bins[static_cast<std::size_t>(cr) * kScopeLevels + static_cast<std::size_t>(cb)];
}

std::uint32_t Vectorscope::peakCount() const noexcept {
    return bins.empty() ? 0u : peakOf(bins.data(), bins.size());
}

std::uint32_t Vectorscope::neutralCount() const noexcept { return at(128, 128); }

Vectorscope computeVectorscope(const std::uint8_t* rgba, int width, int height) {
    Vectorscope out;
    if (!hasFrame(rgba, width, height)) {
        return out;
    }
    out.bins.assign(static_cast<std::size_t>(kScopeLevels) * kScopeLevels, 0u);
    const std::size_t pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    for (std::size_t i = 0; i < pixels; ++i) {
        const std::uint8_t r = rgba[i * 4 + 0];
        const std::uint8_t g = rgba[i * 4 + 1];
        const std::uint8_t b = rgba[i * 4 + 2];
        const std::uint8_t cb = scopeCb(r, g, b);
        const std::uint8_t cr = scopeCr(r, g, b);
        ++out.bins[static_cast<std::size_t>(cr) * kScopeLevels + static_cast<std::size_t>(cb)];
    }
    out.sampleCount = pixels;
    return out;
}

}  // namespace palmier::gpu
