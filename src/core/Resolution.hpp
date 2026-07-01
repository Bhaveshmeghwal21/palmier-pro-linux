// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/Resolution.hpp — a pixel canvas size (width x height).
//
// Project.canvas is a Resolution (design.md Data Models). Its validation rule is
// canvas.width > 0 && canvas.height > 0. This type stores dimensions as unsigned
// 32-bit pixel counts and provides the common broadcast presets and derived
// aspect-ratio / pixel-count helpers.

#ifndef PALMIER_CORE_RESOLUTION_HPP
#define PALMIER_CORE_RESOLUTION_HPP

#include <cstdint>

namespace palmier {

struct Resolution {
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    constexpr Resolution() = default;
    constexpr Resolution(std::uint32_t w, std::uint32_t h) noexcept : width(w), height(h) {}

    // --- Common presets ----------------------------------------------------
    [[nodiscard]] static constexpr Resolution sd480() noexcept { return {854, 480}; }
    [[nodiscard]] static constexpr Resolution hd720() noexcept { return {1280, 720}; }
    [[nodiscard]] static constexpr Resolution hd1080() noexcept { return {1920, 1080}; }
    [[nodiscard]] static constexpr Resolution qhd1440() noexcept { return {2560, 1440}; }
    [[nodiscard]] static constexpr Resolution uhd4k() noexcept { return {3840, 2160}; }
    [[nodiscard]] static constexpr Resolution uhd8k() noexcept { return {7680, 4320}; }

    /// A canvas is valid iff both dimensions are strictly positive.
    [[nodiscard]] constexpr bool isValid() const noexcept { return width > 0 && height > 0; }

    /// Total pixel count (width * height), widened to avoid overflow.
    [[nodiscard]] constexpr std::uint64_t pixelCount() const noexcept {
        return static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
    }

    /// Width / height as a floating-point aspect ratio; 0.0 for a zero height.
    [[nodiscard]] constexpr double aspectRatio() const noexcept {
        return height == 0 ? 0.0
                           : static_cast<double>(width) / static_cast<double>(height);
    }

    [[nodiscard]] friend constexpr bool operator==(Resolution, Resolution) = default;
};

} // namespace palmier

#endif // PALMIER_CORE_RESOLUTION_HPP
