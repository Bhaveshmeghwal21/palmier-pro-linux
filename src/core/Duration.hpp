// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/Duration.hpp — a signed, high-resolution timeline duration / position.
//
// Duration is the single time unit used across the timeline model: clip
// timelineStart, sourceIn/sourceOut, total timeline length, and playhead
// positions are all expressed as Durations (see design.md Data Models). It is
// stored internally as an integer count of nanosecond ticks, giving exact,
// associative arithmetic (no floating-point drift when summing many clips) while
// still covering ~292 years of range in an int64. Frame <-> time conversions
// live on FrameRate (see FrameRate.hpp) to keep this type dependency-free.

#ifndef PALMIER_CORE_DURATION_HPP
#define PALMIER_CORE_DURATION_HPP

#include <cstdint>
#include <compare>

namespace palmier {

class Duration {
public:
    /// One second expressed in the internal tick unit (nanoseconds).
    static constexpr std::int64_t kTicksPerSecond = 1'000'000'000;

    constexpr Duration() = default;

    // --- Named constructors ------------------------------------------------
    [[nodiscard]] static constexpr Duration zero() noexcept { return Duration{}; }

    [[nodiscard]] static constexpr Duration fromNanoseconds(std::int64_t ns) noexcept {
        return Duration{ns};
    }
    [[nodiscard]] static constexpr Duration fromMicroseconds(std::int64_t us) noexcept {
        return Duration{us * 1'000};
    }
    [[nodiscard]] static constexpr Duration fromMilliseconds(std::int64_t ms) noexcept {
        return Duration{ms * 1'000'000};
    }
    [[nodiscard]] static constexpr Duration fromSeconds(double seconds) noexcept {
        // Round to nearest tick to minimize accumulation error on conversion.
        return Duration{static_cast<std::int64_t>(
            seconds * static_cast<double>(kTicksPerSecond) + (seconds < 0 ? -0.5 : 0.5))};
    }

    // --- Accessors ---------------------------------------------------------
    [[nodiscard]] constexpr std::int64_t ticks() const noexcept { return ticks_; }
    [[nodiscard]] constexpr std::int64_t nanoseconds() const noexcept { return ticks_; }
    [[nodiscard]] constexpr std::int64_t microseconds() const noexcept { return ticks_ / 1'000; }
    [[nodiscard]] constexpr std::int64_t milliseconds() const noexcept { return ticks_ / 1'000'000; }
    [[nodiscard]] constexpr double seconds() const noexcept {
        return static_cast<double>(ticks_) / static_cast<double>(kTicksPerSecond);
    }

    [[nodiscard]] constexpr bool isZero() const noexcept { return ticks_ == 0; }
    [[nodiscard]] constexpr bool isNegative() const noexcept { return ticks_ < 0; }
    [[nodiscard]] constexpr bool isPositive() const noexcept { return ticks_ > 0; }

    [[nodiscard]] constexpr Duration abs() const noexcept {
        return Duration{ticks_ < 0 ? -ticks_ : ticks_};
    }

    // --- Arithmetic --------------------------------------------------------
    constexpr Duration& operator+=(Duration rhs) noexcept { ticks_ += rhs.ticks_; return *this; }
    constexpr Duration& operator-=(Duration rhs) noexcept { ticks_ -= rhs.ticks_; return *this; }
    constexpr Duration& operator*=(std::int64_t factor) noexcept { ticks_ *= factor; return *this; }

    [[nodiscard]] friend constexpr Duration operator+(Duration a, Duration b) noexcept {
        return Duration{a.ticks_ + b.ticks_};
    }
    [[nodiscard]] friend constexpr Duration operator-(Duration a, Duration b) noexcept {
        return Duration{a.ticks_ - b.ticks_};
    }
    [[nodiscard]] friend constexpr Duration operator-(Duration a) noexcept {
        return Duration{-a.ticks_};
    }
    [[nodiscard]] friend constexpr Duration operator*(Duration a, std::int64_t f) noexcept {
        return Duration{a.ticks_ * f};
    }
    [[nodiscard]] friend constexpr Duration operator*(std::int64_t f, Duration a) noexcept {
        return Duration{a.ticks_ * f};
    }

    // --- Comparison (spaceship yields total ordering + all relops) ---------
    [[nodiscard]] friend constexpr auto operator<=>(Duration, Duration) = default;
    [[nodiscard]] friend constexpr bool operator==(Duration, Duration) = default;

private:
    explicit constexpr Duration(std::int64_t ticks) noexcept : ticks_(ticks) {}

    std::int64_t ticks_ = 0;
};

} // namespace palmier

#endif // PALMIER_CORE_DURATION_HPP
