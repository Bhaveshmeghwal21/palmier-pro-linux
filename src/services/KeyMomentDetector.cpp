// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/KeyMomentDetector.cpp — implementation of the key-moment detection
// policy (Requirement 5.1, 5.2, 5.5). See KeyMomentDetector.hpp for the contract.

#include "services/KeyMomentDetector.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace palmier::services {

namespace {

/// Round `d` to the nearest whole millisecond, returning a Duration whose value
/// is an exact multiple of one millisecond (Requirement 5.1 "millisecond
/// precision"). Ties round toward positive infinity; the result is re-bounded by
/// the caller so rounding a value near the clip's end can never escape the range.
[[nodiscard]] Duration roundToMillisecond(Duration d) noexcept {
    constexpr std::int64_t kNsPerMs = 1'000'000;
    const std::int64_t ns = d.nanoseconds();
    // Symmetric round-half-up on the nanosecond remainder.
    const std::int64_t ms =
        (ns >= 0) ? (ns + kNsPerMs / 2) / kNsPerMs
                  : -((-ns + kNsPerMs / 2) / kNsPerMs);
    return Duration::fromMilliseconds(ms);
}

} // namespace

KeyMomentDetector::KeyMomentDetector(IKeyMomentBackend& backend) : backend_(backend) {}

Result<std::vector<KeyMoment>> KeyMomentDetector::detect(const KeyMomentSource& source) {
    // 5.5: an empty or zero-duration (or negative) clip is an error, and the
    // analysis engine is never consulted. No timestamps are produced.
    if (!source.hasContent()) {
        return err<std::vector<KeyMoment>>(invalidArgument(
            "key-moment detection requested for an empty or zero-duration clip"));
    }

    // Hand off to the external analysis engine.
    Result<std::vector<Duration>> backendResult = backend_.analyze(source);

    // 5.5: a detection failure is propagated as an error and yields no timestamps.
    if (backendResult.isError()) {
        return err<std::vector<KeyMoment>>(std::move(backendResult).error());
    }

    std::vector<Duration> raw = std::move(backendResult).value();

    // 5.1 / 5.2: round each candidate to millisecond precision and keep only those
    // that fall within [0, clipDuration]. Out-of-range candidates are discarded so
    // the 5.2 guarantee holds regardless of what the backend produced.
    const Duration clipDuration = source.clipDuration;
    std::vector<Duration> bounded;
    bounded.reserve(raw.size());
    for (Duration candidate : raw) {
        const Duration ts = roundToMillisecond(candidate);
        if (ts < Duration::zero() || ts > clipDuration) {
            continue;
        }
        bounded.push_back(ts);
    }

    // Deterministic ordering + de-duplication so repeated candidates (or ones that
    // round to the same millisecond) collapse to a single marker.
    std::sort(bounded.begin(), bounded.end());
    bounded.erase(std::unique(bounded.begin(), bounded.end()), bounded.end());

    // 5.1: cap the count at kMaxKeyMoments, keeping the earliest timestamps.
    if (bounded.size() > kMaxKeyMoments) {
        bounded.resize(kMaxKeyMoments);
    }

    std::vector<KeyMoment> moments;
    moments.reserve(bounded.size());
    for (Duration ts : bounded) {
        moments.push_back(KeyMoment{ts});
    }
    return moments;
}

Duration KeyMomentDetector::detectionBudget() noexcept {
    return Duration::fromSeconds(static_cast<double>(kDetectionBudgetSeconds));
}

} // namespace palmier::services
