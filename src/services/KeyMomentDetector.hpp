// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/KeyMomentDetector.hpp — key-moment detection producing bounded,
// millisecond-precision timestamps for a clip (Requirement 5: 5.1, 5.2, 5.5).
//
// The KeyMoment_Detector (design.md Glossary / component list) "analyzes footage
// to identify candidate key moments". The actual analysis engine — a scene/shot
// detector, an ML saliency model, or a hosted service — is an external concern.
// This component owns the *editor-side* policy that the requirements pin down and
// keeps it independent of any particular analysis engine:
//
//   * 5.1 — a successful detection on a valid clip returns a list of 0 to 500
//           timestamps, each expressed to MILLISECOND precision, produced within
//           the time budget (10 seconds for clips up to 60 minutes).
//   * 5.2 — every returned timestamp is >= 0 and <= the analyzed clip duration.
//   * 5.5 — if detection fails, or the requested clip is empty or has zero
//           duration, an error indication is returned and NO timestamps are
//           produced.
//
// (Requirements 5.3 and 5.4 — displaying markers / a "no key moments" indication
// on the timeline — are the Timeline model's concern and are wired up by task
// 12.2; a completed detection carrying zero timestamps is a valid success here.)
//
// The analysis engine is abstracted behind IKeyMomentBackend so the bounds
// enforcement (5.2), the count cap (5.1), and the empty/zero-duration and failure
// handling (5.5) are fully unit-testable with a mock backend and no external
// dependency. This header depends only on the domain core (Result/Error, Duration,
// Uuid/ClipId), so it compiles and tests on any platform.

#ifndef PALMIER_SERVICES_KEYMOMENTDETECTOR_HPP
#define PALMIER_SERVICES_KEYMOMENTDETECTOR_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/Clip.hpp"      // ClipId
#include "core/Duration.hpp"
#include "core/Result.hpp"

namespace palmier::services {

// ---------------------------------------------------------------------------
// KeyMoment
// ---------------------------------------------------------------------------

/// A single detected key moment: a position within the analyzed clip, expressed
/// to millisecond precision and constrained to `[0, clipDuration]` (Requirements
/// 5.1, 5.2). The timestamp is measured relative to the start of the clip.
struct KeyMoment {
    Duration timestamp;  ///< Position within the clip, ms precision, in [0, clipDuration].

    /// The timestamp in whole milliseconds from the clip start.
    [[nodiscard]] std::int64_t milliseconds() const noexcept { return timestamp.milliseconds(); }

    friend bool operator==(const KeyMoment& a, const KeyMoment& b) {
        return a.timestamp == b.timestamp;
    }
    friend bool operator!=(const KeyMoment& a, const KeyMoment& b) { return !(a == b); }
};

// ---------------------------------------------------------------------------
// Clip description
// ---------------------------------------------------------------------------

/// Describes the clip to analyze. `clipDuration` is the analyzed clip's total
/// length; a clip that is empty or has zero (or negative) duration triggers the
/// error path (Requirement 5.5). `clipId` is carried through for callers that
/// correlate results back to a clip (e.g. the timeline-marker integration in
/// task 12.2); the detection policy itself does not depend on it.
struct KeyMomentSource {
    ClipId   clipId;        ///< Identity of the clip being analyzed (optional to the policy).
    Duration clipDuration;  ///< Total length of the clip; must be positive.

    /// True iff the clip carries analyzable content, i.e. a strictly positive
    /// duration (Requirement 5.5 treats empty / zero-duration clips as errors).
    [[nodiscard]] bool hasContent() const noexcept { return clipDuration.isPositive(); }
};

// ---------------------------------------------------------------------------
// Analysis engine seam
// ---------------------------------------------------------------------------

/// The external footage-analysis engine, abstracted so the detector's bounds,
/// count-cap, and failure policy are testable with a mock. The detector only
/// invokes the backend when the clip has analyzable content, so implementations
/// may assume `source.hasContent() == true`.
///
/// Returning an Error models a detection failure (Requirement 5.5). Returning a
/// vector of timestamps models success; the detector is responsible for rounding
/// each timestamp to millisecond precision, discarding any that fall outside
/// `[0, clipDuration]`, ordering and de-duplicating them, and capping the count —
/// the backend need not pre-sort, pre-bound, or pre-cap its candidates.
class IKeyMomentBackend {
public:
    virtual ~IKeyMomentBackend() = default;

    /// Analyze the supplied clip into raw candidate timestamps (relative to the
    /// clip start). The candidates may be unordered, out of range, or exceed the
    /// count cap; the detector normalizes them.
    [[nodiscard]] virtual Result<std::vector<Duration>> analyze(
        const KeyMomentSource& source) = 0;
};

// ---------------------------------------------------------------------------
// KeyMomentDetector
// ---------------------------------------------------------------------------

/// Owns the editor-side key-moment detection policy. The `backend` reference must
/// outlive the detector.
class KeyMomentDetector {
public:
    /// Maximum number of timestamps a successful detection may return
    /// (Requirement 5.1).
    static constexpr std::size_t kMaxKeyMoments = 500;

    /// Wall-clock budget for a detection request: 10 seconds for clips up to 60
    /// minutes (Requirement 5.1).
    static constexpr std::int64_t kDetectionBudgetSeconds = 10;

    /// Longest clip duration the budget above is guaranteed for (60 minutes).
    static constexpr std::int64_t kMaxSupportedClipMinutes = 60;

    explicit KeyMomentDetector(IKeyMomentBackend& backend);

    /// Detect key moments in `source`.
    ///
    /// Behavior by case:
    ///   * Empty / zero-duration clip (source.hasContent() == false): returns an
    ///     Error and does NOT invoke the backend or produce timestamps
    ///     (Requirement 5.5).
    ///   * Backend fails: the Error is propagated and no timestamps are produced
    ///     (Requirement 5.5).
    ///   * Backend succeeds: each candidate is rounded to millisecond precision,
    ///     any timestamp outside `[0, clipDuration]` is discarded (Requirement
    ///     5.2), the survivors are sorted ascending and de-duplicated, and the
    ///     result is capped to the earliest `kMaxKeyMoments` timestamps
    ///     (Requirement 5.1). A valid clip with no detected moments yields an
    ///     empty list (a successful zero-timestamp result, not an error).
    [[nodiscard]] Result<std::vector<KeyMoment>> detect(const KeyMomentSource& source);

    /// The wall-clock budget for a detection request (Requirement 5.1): a fixed
    /// 10 seconds, independent of clip length within the supported 60-minute range.
    [[nodiscard]] static Duration detectionBudget() noexcept;

private:
    IKeyMomentBackend& backend_;
};

} // namespace palmier::services

#endif // PALMIER_SERVICES_KEYMOMENTDETECTOR_HPP
