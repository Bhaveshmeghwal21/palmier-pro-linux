// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/KeyMomentDetector_test.cpp — unit tests for the key-moment detection
// policy (task 12.1; Requirements 5.1, 5.2, 5.5). The external analysis engine is
// replaced with a scriptable mock backend so the detector's bounds enforcement,
// count cap, ordering/de-duplication, and empty/zero-duration/failure behavior is
// exercised in isolation, without any footage-analysis engine.
//
// Focus areas:
//   * 5.1 — a successful detection returns 0 to 500 ms-precision timestamps; the
//           count is capped at 500 and the budget helper reports 10 seconds.
//   * 5.2 — every returned timestamp is in [0, clipDuration]; out-of-range
//           candidates from the backend are discarded.
//   * 5.5 — an empty / zero-duration clip and a backend failure both return an
//           error and produce NO timestamps (the backend is not consulted for the
//           empty-clip case).

#include "services/KeyMomentDetector.hpp"

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/Duration.hpp"
#include "core/Error.hpp"
#include "core/Uuid.hpp"

namespace palmier::services {
namespace {

// A scriptable analysis engine: it either returns a preset candidate list
// (success) or a preset Error (failure), and records how many times it was
// invoked so tests can assert the detector never calls it on the empty-clip path.
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

KeyMomentSource clipOf(std::int64_t ms) {
    return KeyMomentSource{Uuid::generateV4(), Duration::fromMilliseconds(ms)};
}

Duration ms(std::int64_t v) { return Duration::fromMilliseconds(v); }

// ---- Requirement 5.1: count, ordering, and budget -------------------------

TEST(KeyMomentDetect, ReturnsOrderedDeduplicatedTimestamps) {
    MockBackend backend;
    // Out of order, with a duplicate.
    backend.candidates = {ms(3000), ms(1000), ms(2000), ms(1000)};
    KeyMomentDetector detector(backend);

    auto result = detector.detect(clipOf(10'000));
    ASSERT_TRUE(result.isOk());
    const auto& moments = result.value();

    ASSERT_EQ(moments.size(), 3u);
    EXPECT_EQ(moments[0].milliseconds(), 1000);
    EXPECT_EQ(moments[1].milliseconds(), 2000);
    EXPECT_EQ(moments[2].milliseconds(), 3000);
}

TEST(KeyMomentDetect, ValidClipWithNoDetectedMomentsSucceedsWithEmptyList) {
    MockBackend backend;
    backend.candidates = {};  // valid clip, but analysis found nothing
    KeyMomentDetector detector(backend);

    auto result = detector.detect(clipOf(5'000));
    ASSERT_TRUE(result.isOk());
    EXPECT_TRUE(result.value().empty());
    EXPECT_EQ(backend.calls, 1);  // the backend WAS consulted (clip has content)
}

TEST(KeyMomentDetect, CountIsCappedAtFiveHundred) {
    MockBackend backend;
    // Produce 700 distinct, in-range, whole-millisecond candidates.
    for (std::int64_t i = 1; i <= 700; ++i) {
        backend.candidates.push_back(ms(i));
    }
    KeyMomentDetector detector(backend);

    auto result = detector.detect(clipOf(1'000'000));
    ASSERT_TRUE(result.isOk());
    EXPECT_EQ(result.value().size(), KeyMomentDetector::kMaxKeyMoments);
    EXPECT_EQ(result.value().size(), 500u);
    // The earliest timestamps are retained.
    EXPECT_EQ(result.value().front().milliseconds(), 1);
    EXPECT_EQ(result.value().back().milliseconds(), 500);
}

TEST(KeyMomentDetect, RoundsCandidatesToMillisecondPrecision) {
    MockBackend backend;
    // 1_499_000 ns -> 1 ms; 1_500_000 ns -> 2 ms (round half up).
    backend.candidates = {Duration::fromNanoseconds(1'499'000),
                          Duration::fromNanoseconds(1'500'000)};
    KeyMomentDetector detector(backend);

    auto result = detector.detect(clipOf(10));
    ASSERT_TRUE(result.isOk());
    ASSERT_EQ(result.value().size(), 2u);
    EXPECT_EQ(result.value()[0].milliseconds(), 1);
    EXPECT_EQ(result.value()[1].milliseconds(), 2);
    // Each timestamp is an exact whole-millisecond Duration.
    EXPECT_EQ(result.value()[0].timestamp.nanoseconds(), 1'000'000);
    EXPECT_EQ(result.value()[1].timestamp.nanoseconds(), 2'000'000);
}

TEST(KeyMomentDetect, DetectionBudgetIsTenSeconds) {
    EXPECT_EQ(KeyMomentDetector::detectionBudget().seconds(), 10.0);
    EXPECT_EQ(KeyMomentDetector::kDetectionBudgetSeconds, 10);
}

// ---- Requirement 5.2: bounds ----------------------------------------------

TEST(KeyMomentBounds, DiscardsTimestampsOutsideClipRange) {
    MockBackend backend;
    // -1000 (before start) and 6000 (past a 5000 ms clip) must be discarded;
    // the in-range 0, 2500, and the boundary 5000 must be kept.
    backend.candidates = {ms(-1000), ms(0), ms(2500), ms(5000), ms(6000)};
    KeyMomentDetector detector(backend);

    auto result = detector.detect(clipOf(5'000));
    ASSERT_TRUE(result.isOk());
    const auto& moments = result.value();

    ASSERT_EQ(moments.size(), 3u);
    EXPECT_EQ(moments[0].milliseconds(), 0);
    EXPECT_EQ(moments[1].milliseconds(), 2500);
    EXPECT_EQ(moments[2].milliseconds(), 5000);

    for (const KeyMoment& m : moments) {
        EXPECT_GE(m.timestamp, Duration::zero());
        EXPECT_LE(m.timestamp, Duration::fromMilliseconds(5000));
    }
}

TEST(KeyMomentBounds, BoundaryTimestampsAtZeroAndDurationAreInclusive) {
    MockBackend backend;
    backend.candidates = {ms(0), ms(4'000)};
    KeyMomentDetector detector(backend);

    auto result = detector.detect(clipOf(4'000));
    ASSERT_TRUE(result.isOk());
    ASSERT_EQ(result.value().size(), 2u);
    EXPECT_EQ(result.value().front().milliseconds(), 0);
    EXPECT_EQ(result.value().back().milliseconds(), 4'000);
}

TEST(KeyMomentBounds, RoundingNearClipEndCannotEscapeRange) {
    // A sub-millisecond clip: any candidate rounds to 0 ms, which is <= duration.
    // A candidate that would round up past the end is still bounded.
    MockBackend backend;
    // clip is 400 us (~0 ms). Candidate at 600 us rounds to 1 ms which exceeds the
    // clip duration (400 us) and must be discarded.
    backend.candidates = {Duration::fromMicroseconds(600)};
    KeyMomentDetector detector(backend);

    KeyMomentSource src{Uuid::generateV4(), Duration::fromMicroseconds(400)};
    auto result = detector.detect(src);
    ASSERT_TRUE(result.isOk());
    EXPECT_TRUE(result.value().empty());
}

// ---- Requirement 5.5: empty / zero-duration clip and failure --------------

TEST(KeyMomentError, ZeroDurationClipReturnsErrorAndSkipsBackend) {
    MockBackend backend;
    backend.candidates = {ms(100)};  // would be returned if consulted
    KeyMomentDetector detector(backend);

    auto result = detector.detect(clipOf(0));
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(backend.calls, 0);  // analysis engine is never consulted (5.5)
}

TEST(KeyMomentError, NegativeDurationClipReturnsError) {
    MockBackend backend;
    KeyMomentDetector detector(backend);

    KeyMomentSource src{Uuid::generateV4(), Duration::fromMilliseconds(-1)};
    auto result = detector.detect(src);
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(backend.calls, 0);
}

TEST(KeyMomentError, BackendFailurePropagatesErrorAndProducesNoTimestamps) {
    MockBackend backend;
    backend.error = makeError(ErrorCode::Internal, "analysis engine crashed");
    KeyMomentDetector detector(backend);

    auto result = detector.detect(clipOf(10'000));
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::Internal);
    EXPECT_EQ(backend.calls, 1);  // the backend WAS consulted, and it failed
}

TEST(KeyMomentError, TimeoutFromBackendIsPropagated) {
    MockBackend backend;
    backend.error = makeError(ErrorCode::Timeout, "detection did not complete in budget");
    KeyMomentDetector detector(backend);

    auto result = detector.detect(clipOf(3'600'000));  // 60 minutes
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::Timeout);
}

}  // namespace
}  // namespace palmier::services
