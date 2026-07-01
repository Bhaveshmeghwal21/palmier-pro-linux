// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/FrameRate.hpp — an exact, rational frame rate (e.g. 24, 30, 60, 23.976).
//
// A Project carries a timelineFps (design.md Data Models). Broadcast/NTSC rates
// such as 23.976 and 29.97 are not exactly representable as decimals, so the rate
// is stored as an exact rational numerator/denominator (24000/1001, 30000/1001).
// This type also owns the frame <-> Duration conversions, keeping Duration itself
// free of any frame-rate dependency.

#ifndef PALMIER_CORE_FRAMERATE_HPP
#define PALMIER_CORE_FRAMERATE_HPP

#include <cstdint>
#include <numeric>

#include "core/Duration.hpp"

namespace palmier {

class FrameRate {
public:
    constexpr FrameRate() = default;

    /// Construct from an exact ratio numerator/denominator (frames per second).
    /// The ratio is reduced; a non-positive numerator or denominator yields an
    /// invalid (isValid() == false) rate rather than throwing.
    constexpr FrameRate(std::int64_t numerator, std::int64_t denominator = 1) noexcept
        : num_(numerator), den_(denominator) {
        normalize();
    }

    // --- Common presets ----------------------------------------------------
    [[nodiscard]] static constexpr FrameRate fps24() noexcept { return FrameRate{24, 1}; }
    [[nodiscard]] static constexpr FrameRate fps25() noexcept { return FrameRate{25, 1}; }
    [[nodiscard]] static constexpr FrameRate fps30() noexcept { return FrameRate{30, 1}; }
    [[nodiscard]] static constexpr FrameRate fps50() noexcept { return FrameRate{50, 1}; }
    [[nodiscard]] static constexpr FrameRate fps60() noexcept { return FrameRate{60, 1}; }
    [[nodiscard]] static constexpr FrameRate fps23_976() noexcept { return FrameRate{24000, 1001}; }
    [[nodiscard]] static constexpr FrameRate fps29_97() noexcept { return FrameRate{30000, 1001}; }
    [[nodiscard]] static constexpr FrameRate fps59_94() noexcept { return FrameRate{60000, 1001}; }

    // --- Accessors ---------------------------------------------------------
    [[nodiscard]] constexpr std::int64_t numerator() const noexcept { return num_; }
    [[nodiscard]] constexpr std::int64_t denominator() const noexcept { return den_; }
    [[nodiscard]] constexpr bool isValid() const noexcept { return num_ > 0 && den_ > 0; }
    [[nodiscard]] constexpr double toDouble() const noexcept {
        return den_ == 0 ? 0.0 : static_cast<double>(num_) / static_cast<double>(den_);
    }

    // --- Frame <-> time conversions ---------------------------------------
    /// Duration of a single frame at this rate (denominator/numerator seconds).
    [[nodiscard]] constexpr Duration frameDuration() const noexcept {
        if (!isValid()) return Duration::zero();
        return Duration::fromNanoseconds((Duration::kTicksPerSecond * den_) / num_);
    }

    /// Exact duration of `frameCount` whole frames at this rate.
    [[nodiscard]] constexpr Duration durationForFrames(std::int64_t frameCount) const noexcept {
        if (!isValid()) return Duration::zero();
        return Duration::fromNanoseconds(
            (Duration::kTicksPerSecond * den_ * frameCount) / num_);
    }

    /// Number of whole frames that fit within `d` (floored, toward zero).
    [[nodiscard]] constexpr std::int64_t framesForDuration(Duration d) const noexcept {
        if (!isValid()) return 0;
        return (d.ticks() * num_) / (Duration::kTicksPerSecond * den_);
    }

    [[nodiscard]] friend constexpr bool operator==(FrameRate a, FrameRate b) noexcept {
        return a.num_ == b.num_ && a.den_ == b.den_;
    }
    [[nodiscard]] friend constexpr bool operator!=(FrameRate a, FrameRate b) noexcept {
        return !(a == b);
    }

private:
    constexpr void normalize() noexcept {
        if (num_ <= 0 || den_ <= 0) {
            num_ = 0;
            den_ = 0; // canonical invalid form
            return;
        }
        const std::int64_t g = std::gcd(num_, den_);
        if (g > 1) {
            num_ /= g;
            den_ /= g;
        }
    }

    std::int64_t num_ = 0;
    std::int64_t den_ = 0;
};

} // namespace palmier

#endif // PALMIER_CORE_FRAMERATE_HPP
