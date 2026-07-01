// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/KeyMomentMarkers_test.cpp — unit tests for the key-moment marker overlay
// (task 12.2; Requirements 5.3, 5.4, 5.5). Exercise the mapping from a detection
// result to timeline marker data, the "no key moments" indication, the query and
// observer surface, and the TimelineEngine integration (markers pruned when a
// clip is removed) — all in isolation via a mock IKeyMomentBackend and the real
// TimelineEngine, without any UI, FFmpeg, or GPU dependency.
//
// Focus areas:
//   * 5.3 — a detection with >= 1 timestamp records one marker per timestamp on
//           the clip (KeyMomentsFound), preserving the detector's ordering.
//   * 5.4 — a detection with zero timestamps records NO markers and a distinct
//           "no key moments" indication (NoKeyMoments), not an error.
//   * 5.5 — a detection error records nothing and leaves prior state untouched.
//   * Integration — removing a clip from an attached TimelineEngine prunes its
//           markers so they never dangle.

#include "services/KeyMomentMarkers.hpp"

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/Duration.hpp"
#include "core/EditCommands.hpp"
#include "core/Error.hpp"
#include "core/FrameRate.hpp"
#include "core/Project.hpp"
#include "core/Resolution.hpp"
#include "core/TimelineEngine.hpp"
#include "core/Track.hpp"
#include "core/Uuid.hpp"
#include "services/KeyMomentDetector.hpp"

namespace palmier::services {
namespace {

// A scriptable analysis engine: returns a preset candidate list (success) or a
// preset Error (failure).
class MockBackend : public IKeyMomentBackend {
public:
    Result<std::vector<Duration>> analyze(const KeyMomentSource&) override {
        if (error.has_value()) {
            return err<std::vector<Duration>>(*error);
        }
        return candidates;
    }

    std::vector<Duration> candidates;
    std::optional<Error>  error;
};

Duration ms(std::int64_t v) { return Duration::fromMilliseconds(v); }

KeyMomentSource clipOf(ClipId id, std::int64_t durationMs) {
    return KeyMomentSource{id, Duration::fromMilliseconds(durationMs)};
}

// ---- Requirement 5.3: markers at each detected timestamp ------------------

TEST(KeyMomentMarkers, RecordsOneMarkerPerDetectedTimestamp) {
    const ClipId clip = Uuid::generateV4();
    MockBackend backend;
    backend.candidates = {ms(1000), ms(2000), ms(3000)};
    KeyMomentDetector detector(backend);
    KeyMomentMarkerModel model;

    auto result = model.detectAndRecord(detector, clipOf(clip, 10'000));
    ASSERT_TRUE(result.isOk());
    const ClipMarkers& markers = result.value();

    EXPECT_TRUE(markers.hasKeyMoments());
    EXPECT_EQ(markers.presence, MarkerPresence::KeyMomentsFound);
    ASSERT_EQ(markers.count(), 3u);
    EXPECT_EQ(markers.markers[0].milliseconds(), 1000);
    EXPECT_EQ(markers.markers[1].milliseconds(), 2000);
    EXPECT_EQ(markers.markers[2].milliseconds(), 3000);
    for (const KeyMomentMarker& m : markers.markers) {
        EXPECT_EQ(m.clipId, clip);
    }

    // Recorded state is queryable.
    EXPECT_TRUE(model.hasKeyMoments(clip));
    EXPECT_EQ(model.markerCount(clip), 3u);
    ASSERT_TRUE(model.presenceFor(clip).has_value());
    EXPECT_EQ(*model.presenceFor(clip), MarkerPresence::KeyMomentsFound);
}

TEST(KeyMomentMarkers, MarkersMirrorDetectorTimestampsExactly) {
    const ClipId clip = Uuid::generateV4();
    const std::vector<KeyMoment> detected = {KeyMoment{ms(500)}, KeyMoment{ms(4200)}};
    KeyMomentMarkerModel model;

    auto result = KeyMomentMarkerModel::classify(clip, Result<std::vector<KeyMoment>>(detected));
    ASSERT_TRUE(result.isOk());
    ASSERT_EQ(result.value().count(), 2u);
    EXPECT_EQ(result.value().markers[0].timestamp, ms(500));
    EXPECT_EQ(result.value().markers[1].timestamp, ms(4200));
}

// ---- Requirement 5.4: zero detected -> no markers + indication ------------

TEST(KeyMomentMarkers, ZeroTimestampsRecordsNoMarkersWithIndication) {
    const ClipId clip = Uuid::generateV4();
    MockBackend backend;
    backend.candidates = {};  // valid clip, analysis found nothing
    KeyMomentDetector detector(backend);
    KeyMomentMarkerModel model;

    auto result = model.detectAndRecord(detector, clipOf(clip, 5'000));
    ASSERT_TRUE(result.isOk());  // NOT an error — a normal "no key moments" outcome
    const ClipMarkers& markers = result.value();

    EXPECT_TRUE(markers.noKeyMoments());
    EXPECT_FALSE(markers.hasKeyMoments());
    EXPECT_EQ(markers.presence, MarkerPresence::NoKeyMoments);
    EXPECT_EQ(markers.count(), 0u);
    EXPECT_TRUE(markers.markers.empty());

    // The clip is tracked with a NoKeyMoments indication and zero markers.
    ASSERT_TRUE(model.presenceFor(clip).has_value());
    EXPECT_EQ(*model.presenceFor(clip), MarkerPresence::NoKeyMoments);
    EXPECT_FALSE(model.hasKeyMoments(clip));
    EXPECT_EQ(model.markerCount(clip), 0u);
}

// ---- Requirement 5.5: detection error records nothing ---------------------

TEST(KeyMomentMarkers, DetectionErrorRecordsNothingAndPropagates) {
    const ClipId clip = Uuid::generateV4();
    MockBackend backend;
    backend.error = makeError(ErrorCode::Internal, "analysis engine crashed");
    KeyMomentDetector detector(backend);
    KeyMomentMarkerModel model;

    auto result = model.detectAndRecord(detector, clipOf(clip, 10'000));
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::Internal);

    // Nothing was recorded for the clip.
    EXPECT_FALSE(model.markersFor(clip).has_value());
    EXPECT_FALSE(model.presenceFor(clip).has_value());
    EXPECT_EQ(model.trackedClipCount(), 0u);
}

TEST(KeyMomentMarkers, DetectionErrorLeavesPriorStateUntouched) {
    const ClipId clip = Uuid::generateV4();
    MockBackend backend;
    backend.candidates = {ms(1000), ms(2000)};
    KeyMomentDetector detector(backend);
    KeyMomentMarkerModel model;

    ASSERT_TRUE(model.detectAndRecord(detector, clipOf(clip, 10'000)).isOk());
    ASSERT_EQ(model.markerCount(clip), 2u);

    // A subsequent failing detection must not clobber the recorded markers.
    backend.error = makeError(ErrorCode::Timeout, "detection did not complete in budget");
    auto result = model.detectAndRecord(detector, clipOf(clip, 10'000));
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(model.markerCount(clip), 2u);
    EXPECT_TRUE(model.hasKeyMoments(clip));
}

TEST(KeyMomentMarkers, EmptyOrZeroDurationClipErrorRecordsNothing) {
    const ClipId clip = Uuid::generateV4();
    MockBackend backend;
    backend.candidates = {ms(100)};
    KeyMomentDetector detector(backend);
    KeyMomentMarkerModel model;

    auto result = model.detectAndRecord(detector, clipOf(clip, 0));
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    EXPECT_FALSE(model.markersFor(clip).has_value());
}

// ---- Query surface --------------------------------------------------------

TEST(KeyMomentMarkers, UnknownClipHasNoRecordedState) {
    KeyMomentMarkerModel model;
    const ClipId clip = Uuid::generateV4();
    EXPECT_FALSE(model.markersFor(clip).has_value());
    EXPECT_FALSE(model.presenceFor(clip).has_value());
    EXPECT_FALSE(model.hasKeyMoments(clip));
    EXPECT_EQ(model.markerCount(clip), 0u);
    EXPECT_EQ(model.trackedClipCount(), 0u);
}

TEST(KeyMomentMarkers, ReDetectionReplacesPriorMarkers) {
    const ClipId clip = Uuid::generateV4();
    MockBackend backend;
    backend.candidates = {ms(1000), ms(2000), ms(3000)};
    KeyMomentDetector detector(backend);
    KeyMomentMarkerModel model;

    ASSERT_TRUE(model.detectAndRecord(detector, clipOf(clip, 10'000)).isOk());
    ASSERT_EQ(model.markerCount(clip), 3u);

    // A second detection with fewer moments replaces (not appends to) the state.
    backend.candidates = {ms(500)};
    ASSERT_TRUE(model.detectAndRecord(detector, clipOf(clip, 10'000)).isOk());
    EXPECT_EQ(model.markerCount(clip), 1u);
    EXPECT_EQ(model.markersFor(clip)->markers.front().milliseconds(), 500);
}

TEST(KeyMomentMarkers, ClearForgetsRecordedState) {
    const ClipId clip = Uuid::generateV4();
    MockBackend backend;
    backend.candidates = {ms(1000)};
    KeyMomentDetector detector(backend);
    KeyMomentMarkerModel model;

    ASSERT_TRUE(model.detectAndRecord(detector, clipOf(clip, 10'000)).isOk());
    EXPECT_TRUE(model.clear(clip));
    EXPECT_FALSE(model.markersFor(clip).has_value());
    EXPECT_FALSE(model.clear(clip));  // already gone
}

// ---- Observer notification ------------------------------------------------

TEST(KeyMomentMarkers, ObserverNotifiedOnRecordedDetection) {
    const ClipId clip = Uuid::generateV4();
    MockBackend backend;
    backend.candidates = {ms(1000), ms(2000)};
    KeyMomentDetector detector(backend);
    KeyMomentMarkerModel model;

    std::vector<ClipMarkers> seen;
    auto sub = model.observe([&seen](const ClipMarkers& m) { seen.push_back(m); });

    ASSERT_TRUE(model.detectAndRecord(detector, clipOf(clip, 10'000)).isOk());
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_TRUE(seen.front().hasKeyMoments());
    EXPECT_EQ(seen.front().count(), 2u);

    // A "no key moments" outcome also notifies (the UI needs to show the indication).
    backend.candidates = {};
    ASSERT_TRUE(model.detectAndRecord(detector, clipOf(clip, 10'000)).isOk());
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_TRUE(seen.back().noKeyMoments());
}

TEST(KeyMomentMarkers, ObserverNotNotifiedOnDetectionError) {
    const ClipId clip = Uuid::generateV4();
    MockBackend backend;
    backend.error = makeError(ErrorCode::Internal, "boom");
    KeyMomentDetector detector(backend);
    KeyMomentMarkerModel model;

    int calls = 0;
    auto sub = model.observe([&calls](const ClipMarkers&) { ++calls; });
    ASSERT_TRUE(model.detectAndRecord(detector, clipOf(clip, 10'000)).isError());
    EXPECT_EQ(calls, 0);
}

TEST(KeyMomentMarkers, ResetSubscriptionStopsNotifications) {
    const ClipId clip = Uuid::generateV4();
    MockBackend backend;
    backend.candidates = {ms(1000)};
    KeyMomentDetector detector(backend);
    KeyMomentMarkerModel model;

    int calls = 0;
    auto sub = model.observe([&calls](const ClipMarkers&) { ++calls; });
    ASSERT_TRUE(model.detectAndRecord(detector, clipOf(clip, 10'000)).isOk());
    EXPECT_EQ(calls, 1);

    sub.reset();
    ASSERT_TRUE(model.detectAndRecord(detector, clipOf(clip, 10'000)).isOk());
    EXPECT_EQ(calls, 1);  // no further notification after unsubscribe
}

// ---- Timeline integration: prune markers for removed clips ----------------

// Build a minimal valid project with a single video track (no clips) and return
// the track id.
Project projectWithEmptyTrack(Uuid& trackIdOut) {
    Project project;
    project.id = Uuid::generateV4();
    project.name = "test";
    project.timelineFps = FrameRate{30, 1};
    project.canvas = Resolution{1920, 1080};

    Track track;
    track.id = Uuid::generateV4();
    track.kind = TrackKind::Video;
    trackIdOut = track.id;
    project.tracks.push_back(track);
    return project;
}

Clip makeClip(ClipId id, std::int64_t startMs, std::int64_t durationMs) {
    Clip clip;
    clip.id = id;
    clip.timelineStart = Duration::fromMilliseconds(startMs);
    clip.sourceIn = Duration::zero();
    clip.sourceOut = Duration::fromMilliseconds(durationMs);
    return clip;
}

TEST(KeyMomentMarkers, RemovingClipFromAttachedTimelinePrunesMarkers) {
    Uuid trackId;
    TimelineEngine engine(projectWithEmptyTrack(trackId));

    const ClipId clip = Uuid::generateV4();
    ASSERT_TRUE(engine.apply(std::make_unique<AddClipCommand>(trackId, makeClip(clip, 0, 5'000)))
                    .isOk());

    // Record markers for the clip, then attach the model to the engine.
    MockBackend backend;
    backend.candidates = {ms(1000), ms(2000)};
    KeyMomentDetector detector(backend);
    KeyMomentMarkerModel model;
    model.attachTimeline(engine);

    ASSERT_TRUE(model.detectAndRecord(detector, clipOf(clip, 5'000)).isOk());
    ASSERT_EQ(model.markerCount(clip), 2u);

    // Deleting the clip must prune its markers automatically.
    ASSERT_TRUE(engine.apply(std::make_unique<DeleteClipCommand>(clip)).isOk());
    EXPECT_FALSE(model.markersFor(clip).has_value());
    EXPECT_EQ(model.trackedClipCount(), 0u);
}

TEST(KeyMomentMarkers, PruneRemovedClipsDropsOnlyRemovedEntries) {
    const ClipId kept = Uuid::generateV4();
    const ClipId gone = Uuid::generateV4();
    KeyMomentMarkerModel model;

    ASSERT_TRUE(model.record(kept, Result<std::vector<KeyMoment>>(
                                       std::vector<KeyMoment>{KeyMoment{ms(1)}}))
                    .isOk());
    ASSERT_TRUE(model.record(gone, Result<std::vector<KeyMoment>>(
                                       std::vector<KeyMoment>{KeyMoment{ms(2)}}))
                    .isOk());

    ChangeSet change;
    change.removedClips = {gone};
    model.pruneRemovedClips(change);

    EXPECT_TRUE(model.markersFor(kept).has_value());
    EXPECT_FALSE(model.markersFor(gone).has_value());
}

TEST(KeyMomentMarkers, DetachStopsPruning) {
    Uuid trackId;
    TimelineEngine engine(projectWithEmptyTrack(trackId));

    const ClipId clip = Uuid::generateV4();
    ASSERT_TRUE(engine.apply(std::make_unique<AddClipCommand>(trackId, makeClip(clip, 0, 5'000)))
                    .isOk());

    MockBackend backend;
    backend.candidates = {ms(1000)};
    KeyMomentDetector detector(backend);
    KeyMomentMarkerModel model;
    model.attachTimeline(engine);
    ASSERT_TRUE(model.detectAndRecord(detector, clipOf(clip, 5'000)).isOk());

    model.detachAll();
    ASSERT_TRUE(engine.apply(std::make_unique<DeleteClipCommand>(clip)).isOk());
    // With no attachment, the removal does not prune the (now stale) markers.
    EXPECT_TRUE(model.markersFor(clip).has_value());
}

}  // namespace
}  // namespace palmier::services
