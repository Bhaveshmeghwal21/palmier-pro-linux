// SPDX-License-Identifier: GPL-3.0-or-later
//
// gpu/VideoScopes.hpp — histogram, luma waveform and vectorscope computations
// (monitoring-and-grading task 6; Requirement 6).
//
// Requirement 6.4: pure functions of a frame buffer, independent of Qt and of any GPU
// being present, separately testable against hand-checkable inputs. They live beside
// gpu::Compositor because the buffer they read is the one it produces — Requirement 6.2
// insists a scope be computed from the SAME compositor output the Preview displays, so
// that a scope cannot disagree with the picture beside it — but nothing here touches
// Vulkan, and this header compiles in a tree with no GPU at all.
//
// COLOUR CONVENTION. Luma and chroma are Rec.601 full-range, using the same
// coefficients gpu::Compositor's saturation step already uses:
//
//     Y  = 0.299 R + 0.587 G + 0.114 B
//     Cb = 128 + 0.564 (B - Y)
//     Cr = 128 + 0.713 (R - Y)
//
// Deliberately the same as the compositor's rather than a "better" choice: a scope
// computed in a different space from the effect that moved the picture would show a
// colourist a correction they did not make. Both are clamped to [0,255], which is not a
// rounding detail but the reason a full-saturation primary lands ON the graticule edge:
// Cr for pure red is 255.45 before clamping, and a real vectorscope puts red at the
// boundary box too.
//
// EMPTY IS A VALUE, NOT AN ERROR. Requirement 6.6 wants an explicit empty state when no
// frame is available, so each result reports isEmpty() and every computation answers an
// empty result for a null or zero-sized buffer rather than failing. A scope that errored
// would leave the shell choosing between showing nothing and showing the last reading,
// and showing the last reading is precisely the stale display 6.6 forbids.

#ifndef PALMIER_GPU_VIDEOSCOPES_HPP
#define PALMIER_GPU_VIDEOSCOPES_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace palmier::gpu {

/// Number of levels an 8-bit channel can take, and therefore the exact bin count. As
/// with the tone-curve table, 256 bins is not a resolution choice but the whole domain.
inline constexpr int kScopeLevels = 256;

/// Rec.601 luma of one 8-bit RGB triple, rounded to a level in [0,255].
[[nodiscard]] std::uint8_t scopeLuma(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept;

/// Rec.601 Cb and Cr of one 8-bit RGB triple, offset to [0,255] with 128 as neutral.
[[nodiscard]] std::uint8_t scopeCb(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept;
[[nodiscard]] std::uint8_t scopeCr(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept;

// ---------------------------------------------------------------------------
// Histogram
// ---------------------------------------------------------------------------

/// Per-channel and luma level distributions over a whole frame.
struct Histogram {
    std::array<std::uint32_t, kScopeLevels> red{};
    std::array<std::uint32_t, kScopeLevels> green{};
    std::array<std::uint32_t, kScopeLevels> blue{};
    std::array<std::uint32_t, kScopeLevels> luma{};
    /// Pixels counted. Zero means no frame, which is the empty state and not a black
    /// frame — a black frame has every sample in bin 0, which is a real reading.
    std::uint64_t sampleCount = 0;

    [[nodiscard]] bool isEmpty() const noexcept { return sampleCount == 0; }
    /// The largest count in any bin of any channel, for scaling a plot. Zero only when
    /// empty, so a caller can divide by it after checking isEmpty().
    [[nodiscard]] std::uint32_t peakCount() const noexcept;
};

/// Count every pixel of a tightly packed RGBA8 buffer. Alpha is ignored: a histogram
/// measures the picture, and the compositor's output is opaque by the time it is shown.
[[nodiscard]] Histogram computeHistogram(const std::uint8_t* rgba, int width, int height);

// ---------------------------------------------------------------------------
// Luma waveform
// ---------------------------------------------------------------------------

/// Per-column luma distribution: the classic waveform monitor, one vertical trace per
/// horizontal position.
///
/// The frame's columns are BUCKETED into `columns` traces rather than one per pixel,
/// because a 3840-wide frame cannot be shown in a 200-pixel-wide panel and choosing
/// which pixels to keep is exactly how a waveform loses the overexposed streak the
/// colourist is looking for. Bucketing sums them instead, so nothing is dropped.
struct LumaWaveform {
    int columns = 0;
    /// `columns * kScopeLevels` counts, column-major.
    std::vector<std::uint32_t> counts;
    std::uint64_t sampleCount = 0;

    [[nodiscard]] bool isEmpty() const noexcept { return columns == 0 || sampleCount == 0; }
    [[nodiscard]] std::uint32_t at(int column, int level) const noexcept;
    [[nodiscard]] std::uint32_t peakCount() const noexcept;
    /// The lowest and highest luma present anywhere, for asserting range at a glance.
    /// Both zero when empty.
    [[nodiscard]] std::uint8_t minLuma() const noexcept;
    [[nodiscard]] std::uint8_t maxLuma() const noexcept;
};

[[nodiscard]] LumaWaveform computeLumaWaveform(const std::uint8_t* rgba, int width, int height,
                                              int columns);

// ---------------------------------------------------------------------------
// Vectorscope
// ---------------------------------------------------------------------------

/// A 256x256 Cb/Cr distribution: hue is the angle from the centre, saturation the
/// distance from it.
struct Vectorscope {
    /// `kScopeLevels * kScopeLevels` counts, indexed [cr * kScopeLevels + cb].
    std::vector<std::uint32_t> bins;
    std::uint64_t sampleCount = 0;

    [[nodiscard]] bool isEmpty() const noexcept { return sampleCount == 0; }
    [[nodiscard]] std::uint32_t at(int cb, int cr) const noexcept;
    [[nodiscard]] std::uint32_t peakCount() const noexcept;
    /// Count of samples at exactly neutral (128,128), which is what a greyscale frame
    /// produces and therefore the reading that proves a frame carries no colour.
    [[nodiscard]] std::uint32_t neutralCount() const noexcept;
};

[[nodiscard]] Vectorscope computeVectorscope(const std::uint8_t* rgba, int width, int height);

}  // namespace palmier::gpu

#endif  // PALMIER_GPU_VIDEOSCOPES_HPP
