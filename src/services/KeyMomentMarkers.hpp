// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/KeyMomentMarkers.hpp — attaches key-moment detection results to the
// timeline model as per-clip marker data the UI can display (Requirement 5:
// 5.3, 5.4). Task 12.2.
//
// The KeyMomentDetector (task 12.1) produces, for a clip, a bounded list of
// millisecond-precision timestamps — or a distinct error on failure / empty
// clip (Requirement 5.5). This component turns a *successful* detection into
// timeline-model marker data and pins down the two display-facing requirements
// while staying entirely UI-agnostic (no Qt, no view code):
//
//   * 5.3 — when detection finds one or more timestamps, a marker is recorded at
//           each detected timestamp on the clip, so the Timeline_Editor can draw
//           a visual marker at every one of them.
//   * 5.4 — when detection completes with ZERO timestamps, NO markers are added
//           and the clip's marker state carries a distinct "no key moments"
//           indication (MarkerPresence::NoKeyMoments) — a normal, successful
//           outcome, NOT an error, so the editor can surface a "no key moments
//           were found" message without treating it as a failure.
//   * 5.5 — a detection error carries through unchanged: no markers are recorded
//           and the clip's prior marker state is left untouched.
//
// Design notes
// ------------
// Markers are display annotations derived from analysis, not authoritative edit
// state, so they are deliberately kept OUT of the Project/Clip model and off the
// undo/redo path: a KeyMomentMarkerModel owns them, keyed by ClipId. This keeps
// the core data model and its EditCommand/ChangeSet machinery untouched while
// still "integrating with the Timeline Engine model": the model can attach to a
// TimelineEngine and observe its ChangeSet stream so that when a clip is removed
// from the timeline, its markers are pruned automatically and never dangle.
//
// The model exposes its own lightweight observer seam (mirroring
// TimelineEngine::observe / Subscription) so a view can refresh the affected
// clip's marker overlay when detection results arrive. Depending only on the
// domain core (Duration/Result/Uuid/ChangeSet/Subscription) and the detector,
// this header compiles and unit-tests on any platform without Qt/FFmpeg/GPU.

#ifndef PALMIER_SERVICES_KEYMOMENTMARKERS_HPP
#define PALMIER_SERVICES_KEYMOMENTMARKERS_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "core/ChangeSet.hpp"
#include "core/Clip.hpp"      // ClipId
#include "core/Duration.hpp"
#include "core/Result.hpp"
#include "core/Subscription.hpp"
#include "services/KeyMomentDetector.hpp"

namespace palmier {
class TimelineEngine;  // core/TimelineEngine.hpp — the authoritative timeline.
}  // namespace palmier

namespace palmier::services {

// ---------------------------------------------------------------------------
// KeyMomentMarker
// ---------------------------------------------------------------------------

/// A single marker placed on a clip for display at a detected key-moment
/// timestamp (Requirement 5.3). The timestamp is relative to the clip start and,
/// having come from the detector, is millisecond-precise and within
/// `[0, clipDuration]`.
struct KeyMomentMarker {
    ClipId   clipId;      ///< The clip this marker belongs to.
    Duration timestamp;   ///< Position within the clip (ms precision, in range).

    /// The marker position in whole milliseconds from the clip start.
    [[nodiscard]] std::int64_t milliseconds() const noexcept { return timestamp.milliseconds(); }

    friend bool operator==(const KeyMomentMarker& a, const KeyMomentMarker& b) {
        return a.clipId == b.clipId && a.timestamp == b.timestamp;
    }
    friend bool operator!=(const KeyMomentMarker& a, const KeyMomentMarker& b) {
        return !(a == b);
    }
};

// ---------------------------------------------------------------------------
// MarkerPresence — the display-facing outcome distinction
// ---------------------------------------------------------------------------

/// Whether a completed detection produced any markers for a clip. This is the
/// distinction Requirements 5.3 and 5.4 turn on; both are SUCCESS outcomes (a
/// detection failure is an Error, never a MarkerPresence).
enum class MarkerPresence {
    KeyMomentsFound,  ///< >= 1 timestamp detected; markers were recorded (5.3).
    NoKeyMoments,     ///< 0 timestamps detected; NO markers, show the indication (5.4).
};

// ---------------------------------------------------------------------------
// ClipMarkers — the marker state recorded for one clip
// ---------------------------------------------------------------------------

/// The marker data attached to a single clip as a result of a completed
/// detection. When `presence == NoKeyMoments` the `markers` list is empty and
/// the UI should show a "no key moments were found" indication (5.4); when
/// `presence == KeyMomentsFound` the list holds one marker per detected
/// timestamp, in ascending time order (5.3).
struct ClipMarkers {
    ClipId                       clipId;
    MarkerPresence               presence = MarkerPresence::NoKeyMoments;
    std::vector<KeyMomentMarker> markers;

    /// True iff detection found at least one key moment (Requirement 5.3).
    [[nodiscard]] bool hasKeyMoments() const noexcept {
        return presence == MarkerPresence::KeyMomentsFound;
    }
    /// True iff detection completed with zero key moments (Requirement 5.4).
    [[nodiscard]] bool noKeyMoments() const noexcept {
        return presence == MarkerPresence::NoKeyMoments;
    }
    /// Number of markers recorded (0 when noKeyMoments()).
    [[nodiscard]] std::size_t count() const noexcept { return markers.size(); }
};

// ---------------------------------------------------------------------------
// KeyMomentMarkerModel
// ---------------------------------------------------------------------------

/// Owns the timeline's key-moment marker overlay, keyed by clip. It converts a
/// detection result into display marker data (Requirements 5.3, 5.4, 5.5),
/// remembers the latest per-clip result for readers (the UI/MCP), notifies
/// observers when a clip's markers change, and — when attached to a
/// TimelineEngine — prunes markers for clips removed from the timeline.
///
/// Thread-affinity: instances are not internally synchronized; a model shared
/// across threads needs external synchronization.
class KeyMomentMarkerModel {
public:
    KeyMomentMarkerModel();
    ~KeyMomentMarkerModel();

    KeyMomentMarkerModel(const KeyMomentMarkerModel&) = delete;
    KeyMomentMarkerModel& operator=(const KeyMomentMarkerModel&) = delete;
    KeyMomentMarkerModel(KeyMomentMarkerModel&&) = delete;
    KeyMomentMarkerModel& operator=(KeyMomentMarkerModel&&) = delete;

    // --- Pure classification (no state; the testable policy core) ----------

    /// Turn a detection result for `clipId` into clip marker data, WITHOUT
    /// touching the model's stored state or notifying observers.
    ///   * detection is an error  -> the error is propagated unchanged; no
    ///                               markers are produced (Requirement 5.5).
    ///   * detection is empty      -> ClipMarkers{NoKeyMoments, {}} (Requirement 5.4).
    ///   * detection has moments   -> ClipMarkers{KeyMomentsFound, one marker per
    ///                               timestamp, in ascending order} (Requirement 5.3).
    [[nodiscard]] static Result<ClipMarkers> classify(
        ClipId clipId, const Result<std::vector<KeyMoment>>& detection);

    // --- Record a detection result into the model --------------------------

    /// Classify `detection` for `clipId` (see classify) and, on success, store
    /// the resulting ClipMarkers as the clip's current marker state and notify
    /// observers. On a detection error the model is left unchanged and the error
    /// is returned (Requirement 5.5). Returns the recorded ClipMarkers on success.
    [[nodiscard]] Result<ClipMarkers> record(
        ClipId clipId, const Result<std::vector<KeyMoment>>& detection);

    /// Run `detector` on `source`, then record the result for `source.clipId`
    /// (see record). This is the one-call entry point the editor uses to detect
    /// key moments for a clip and update its timeline markers.
    [[nodiscard]] Result<ClipMarkers> detectAndRecord(KeyMomentDetector& detector,
                                                      const KeyMomentSource& source);

    // --- Query -------------------------------------------------------------

    /// The current marker state for `clipId`, or std::nullopt if no detection has
    /// been recorded for it yet.
    [[nodiscard]] std::optional<ClipMarkers> markersFor(ClipId clipId) const;

    /// The presence outcome for `clipId`, or std::nullopt if none recorded yet.
    [[nodiscard]] std::optional<MarkerPresence> presenceFor(ClipId clipId) const;

    /// True iff a detection has been recorded for `clipId` and it found >= 1
    /// key moment.
    [[nodiscard]] bool hasKeyMoments(ClipId clipId) const;

    /// Number of markers currently recorded for `clipId` (0 if none recorded or
    /// the clip has no key moments).
    [[nodiscard]] std::size_t markerCount(ClipId clipId) const;

    /// Number of clips that currently carry a recorded marker state.
    [[nodiscard]] std::size_t trackedClipCount() const noexcept { return byClip_.size(); }

    // --- Mutation ----------------------------------------------------------

    /// Forget any recorded marker state for `clipId`. Returns true if state was
    /// present and removed. Does not notify observers (a removal is not a
    /// detection result).
    bool clear(ClipId clipId);

    /// Forget all recorded marker state.
    void clearAll() noexcept;

    /// Drop marker state for every clip listed in `change.removedClips`. Called
    /// automatically for changes emitted by an attached TimelineEngine; exposed
    /// so the behaviour can be exercised directly.
    void pruneRemovedClips(const ChangeSet& change);

    // --- Timeline integration ---------------------------------------------

    /// Observe `engine` so that whenever a clip is removed from the timeline its
    /// markers are pruned from this model (they would otherwise dangle). The
    /// registration lives for as long as this model, or until detachAll(). Safe
    /// to call for multiple engines.
    void attachTimeline(TimelineEngine& engine);

    /// Drop all timeline attachments made via attachTimeline.
    void detachAll() noexcept;

    // --- Change notification ----------------------------------------------

    /// Register `callback` to receive the ClipMarkers whenever a clip's marker
    /// state changes as a result of a recorded detection (both KeyMomentsFound
    /// and NoKeyMoments outcomes fire; detection errors and prunes do not).
    /// Returns an RAII Subscription; destroying it unregisters the callback. A
    /// null callback yields an inactive Subscription.
    [[nodiscard]] Subscription observe(std::function<void(const ClipMarkers&)> callback);

private:
    // Observer callbacks live in a shared registry so a Subscription can
    // unregister via a weak reference that stays valid even if the model is
    // destroyed first (mirrors TimelineEngine's observer machinery).
    struct ObserverRegistry {
        std::unordered_map<std::uint64_t, std::function<void(const ClipMarkers&)>> callbacks;
        std::uint64_t nextId = 1;
    };

    // NB: intentionally NOT named `emit` — that is a Qt keyword-macro (from
    // <QObject>). This header is pulled into Qt translation units (via
    // ui/MediaBrowserViewModel.hpp -> ui/MediaBrowserPanel.hpp), so a member
    // named `emit` would be mangled by the macro and fail to compile there.
    void notifyObservers(const ClipMarkers& markers) const;

    std::unordered_map<ClipId, ClipMarkers> byClip_;
    std::vector<Subscription>               timelineSubscriptions_;
    std::shared_ptr<ObserverRegistry>       observers_;
};

}  // namespace palmier::services

#endif  // PALMIER_SERVICES_KEYMOMENTMARKERS_HPP
