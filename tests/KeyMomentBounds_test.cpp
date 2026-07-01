// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/KeyMomentBounds_test.cpp — focused unit tests for the key-moment
// detection policy's TIMESTAMP BOUNDS, EMPTY-RESULT handling, and EMPTY /
// ZERO-DURATION clip errors (task 12.3; Requirements 5.2, 5.4, 5.5).
//
// These cases complement (and deliberately do NOT duplicate) the broader
// KeyMomentDetector_test.cpp (task 12.1) suite. That suite already covers the
// count cap, ordering/de-duplication, backend-failure/timeout propagation, and
// the coarse in-range/out-of-range and inclusive-boundary behavior. Here we
// pin down the tight edges that task 12.3 calls out explicitly:
//
//   * 5.2 — the exact boundary: a timestamp exactly at 0 and exactly at
//           clipDuration is retained, while a timestamp just past either edge
//           (-1 ms below 0, or clipDuration + 1 ms) is rejected; and EVERY
//           returned timestamp both lies in [0, clipDuration] and is an exact
//           whole-millisecond value (millisecond precision) across a mixed batch.
//   * 5.4 — empty-result handling: a valid clip whose analysis yields no
//           in-range timestamps (either the backend returns nothing, or every
//           candidate is filtered out as out-of-range) is a SUCCESS carrying an
//           empty list — never an error.
//   * 5.5 — an empty / zero-duration clip returns an error that DESCRIBES the
//           failure, produces no timestamps, and never consults the analysis
//           engine.
//
// The external analysis engine is replaced with a scriptable mock backend, so
// the policy is exercised in isolation with no footage-analysis engine, GPU,
// FFmpeg, or other platform dependency.

#include "services/KeyMomentDetector.hpp"

#include <cstdint>
#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include "core/Duration.hpp"
#include "core/Error.hpp"
#include "core/Uuid.hpp"

namespace palmier::services {
namespace {

// A scriptable analysis engine: returns either a preset candidate list
// (success) or a preset Error (failure), and records how many times it was
// invoked so the empty/zero-duration tests can assert it is never consulted.
class MockBackend : public IKeyMomentBackend {
public:
    Result<std::vector<Duration>> analyze(const KeyMomentSource&) override {
        ++calls;
        if (error.has_value()) {
            return err<std::vector<Duration>>(*error);
        }
        return candidates;
    }

    std::vector<Duration> candidates;  // returned on success
    std::optional<Error>  error;       // when set, returned instead
    int                   calls = 0;
};

Duration ms(std::int64_t v) { return Duration::fromMilliseconds(v); }

KeyMomentSource clipOf(std::int64_t durationMs) {
    return KeyMomentSource{Uuid::generateV4(), Duration::fromMilliseconds(durationMs)};
}

// ---- Requirement 5.2: tight timestamp bounds ------------------------------

// Exactly at 0 and exactly at the clip duration are RETAINED (inclusive edges),
// while exactly one millisecond below 0 or one millisecond past the duration is
// REJECTED. This isolates the closed-interval boundary [0, clipDuration].
TEST(KeyMomentBoundsEdge, InclusiveEdgesKeptJustPastEdgesRejected) {
    constexpr std::int64_t kDurMs = 8'000;
    MockBackend backend;
    backend.candidates = {
        ms(-1),            // just below 0            -> rejected
        ms(0),             // exactly 0               -> kept
        ms(kDurMs),        // exactly clipDuration    -> kept
        ms(kDurMs + 1),    // one ms past duration    -> rejected
    };
    KeyMomentDetector detector(backend);

    auto result = detector.detect(clipOf(kDurMs));
    ASSERT_TRUE(result.isOk());
    const auto& moments = result.value();

    ASSERT_EQ(moments.size(), 2u);
    EXPECT_EQ(moments.front().milliseconds(), 0);
    EXPECT_EQ(moments.back().milliseconds(), kDurMs);
}

// Across a deliberately messy batch (negative, out-of-range, and sub-millisecond
// candidates mixed with valid ones) EVERY surviving timestamp must satisfy the
// two 5.2 guarantees simultaneously: it lies in [0, clipDuration] AND it is an
// exact whole-millisecond value (millisecond precision).
TEST(KeyMomentBoundsEdge, EveryReturnedTimestampIsInRangeAndMillisecondPrecise) {
    constexpr std::int64_t kDurMs = 5'000;
    MockBackend backend;
    backend.candidates = {
        ms(-2'000),                          // out of range (below)
        Duration::fromNanoseconds(1'250'000),  // 1.25 ms -> rounds to 1 ms (in range)
        ms(2'500),                           // in range
        Duration::fromMicroseconds(4'999'400), // 4999.4 ms -> rounds to 4999 ms (in range)
        ms(9'000),                           // out of range (above)
    };
    KeyMomentDetector detector(backend);

    auto result = detector.detect(clipOf(kDurMs));
    ASSERT_TRUE(result.isOk());
    const auto& moments = result.value();

    ASSERT_FALSE(moments.empty());
    const Duration clipDuration = ms(kDurMs);
    for (const KeyMoment& m : moments) {
        // 5.2: within the closed interval [0, clipDuration].
        EXPECT_GE(m.timestamp, Duration::zero());
        EXPECT_LE(m.timestamp, clipDuration);
        // Millisecond precision: the timestamp is an exact multiple of 1 ms.
        EXPECT_EQ(m.timestamp.nanoseconds() % 1'000'000, 0);
        EXPECT_EQ(m.timestamp, ms(m.milliseconds()));
    }
}

// ---- Requirement 5.4: empty-result handling -------------------------------

// When the analysis engine finds nothing, the detection SUCCEEDS with an empty
// list (the timeline shows a "no key moments" indication, not an error). This
// pins the empty-result path where the backend itself returns nothing.
TEST(KeyMomentEmptyResult, BackendReturnsNothingYieldsSuccessfulEmptyList) {
    MockBackend backend;
    backend.candidates = {};  // valid clip, analysis found nothing
    KeyMomentDetector detector(backend);

    auto result = detector.detect(clipOf(30'000));
    ASSERT_TRUE(result.isOk());     // NOT an error
    EXPECT_TRUE(result.value().empty());
    EXPECT_EQ(backend.calls, 1);    // the clip has content, so the engine ran
}

// A distinct empty-result path: the backend returns candidates, but every one is
// out of range and therefore filtered out. The outcome is still a SUCCESS with
// an empty list (5.4) — filtering out-of-range timestamps never turns a valid
// detection into a failure.
TEST(KeyMomentEmptyResult, AllCandidatesOutOfRangeYieldsSuccessfulEmptyList) {
    MockBackend backend;
    backend.candidates = {ms(-100), ms(-1), ms(6'001), ms(50'000)};  // all outside [0, 6000]
    KeyMomentDetector detector(backend);

    auto result = detector.detect(clipOf(6'000));
    ASSERT_TRUE(result.isOk());     // filtering everything out is not an error
    EXPECT_TRUE(result.value().empty());
    EXPECT_EQ(backend.calls, 1);
}

// ---- Requirement 5.5: empty / zero-duration clip errors -------------------

// A zero-duration ("empty") clip returns an InvalidArgument error whose message
// DESCRIBES the failure (5.5: "return an error indication describing the
// failure"), produces no timestamps, and never consults the analysis engine —
// even when the mock is scripted to return candidates.
TEST(KeyMomentEmptyClipError, ZeroDurationClipReturnsDescriptiveErrorAndSkipsBackend) {
    MockBackend backend;
    backend.candidates = {ms(100), ms(200)};  // would be returned if consulted
    KeyMomentDetector detector(backend);

    auto result = detector.detect(clipOf(0));
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    EXPECT_FALSE(result.error().message().empty());  // describes the failure
    EXPECT_EQ(backend.calls, 0);                     // engine never consulted
}

// A negative-duration clip is likewise treated as having no analyzable content
// (5.5): error, no timestamps, no backend call. Uses a sub-millisecond negative
// value to confirm the guard keys off "not positive", not off whole ms.
TEST(KeyMomentEmptyClipError, SubMillisecondNegativeDurationClipReturnsError) {
    MockBackend backend;
    backend.candidates = {ms(100)};
    KeyMomentDetector detector(backend);

    KeyMomentSource src{Uuid::generateV4(), Duration::fromNanoseconds(-1)};
    ASSERT_FALSE(src.hasContent());
    auto result = detector.detect(src);
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(backend.calls, 0);
}

// A clip with a tiny but strictly positive (sub-millisecond) duration DOES have
// content, so it is analyzed rather than rejected: the boundary between the 5.5
// error path and a valid detection is "duration > 0", not "duration >= 1 ms".
// A candidate at 0 is in range for such a clip; a candidate that rounds past the
// tiny duration is filtered (empty result, still success).
TEST(KeyMomentEmptyClipError, TinyPositiveDurationClipIsAnalyzedNotRejected) {
    MockBackend backend;
    backend.candidates = {Duration::zero()};  // 0 <= 400us, so it is kept
    KeyMomentDetector detector(backend);

    KeyMomentSource src{Uuid::generateV4(), Duration::fromMicroseconds(400)};
    ASSERT_TRUE(src.hasContent());
    auto result = detector.detect(src);
    ASSERT_TRUE(result.isOk());          // positive duration -> analyzed, not an error
    ASSERT_EQ(result.value().size(), 1u);
    EXPECT_EQ(result.value().front().milliseconds(), 0);
    EXPECT_EQ(backend.calls, 1);         // the engine WAS consulted
}

}  // namespace
}  // namespace palmier::services
