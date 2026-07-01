// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for clip math edge cases and the undo-empty / split-miss handling
// (task 3.9).
//
// These tests complement the broader TimelineEngine / EditCommands / UndoRedoStack
// suites by focusing narrowly on two requirement clauses and on the low-level
// clip arithmetic they rest on:
//
//   * Requirement 2.10 — an undo (or redo) requested when no operation is
//     available leaves the project state unchanged and reports an indication
//     that there is nothing to undo. Modelled as a CommandResult no-op that is
//     "ok but did not change" and carries a non-empty indication message.
//
//   * Requirement 2.6  — a split requested while the playhead is not positioned
//     within any clip's boundaries leaves all clips unchanged and reports an
//     indication that no clip is available to split. Here that surfaces as a
//     failed CommandResult / Result whose message names the "nothing to split"
//     condition, with the project left byte-for-byte as it was.
//
//   * Clip math — the Clip/Duration/FrameRate arithmetic underpinning the above:
//     duration()/timelineEnd() identities, zero-length and single-tick clips,
//     one-frame clips, and the frame <-> duration round-trip at boundary values.
//
// The engine paths run without a GPU, FFmpeg, or any vendor SDK, so this target
// links only Palmier::core alongside the shared GoogleTest support.
//
// _Requirements: 2.6, 2.10_

#include "core/EditCommands.hpp"

#include <cstdint>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "core/Clip.hpp"
#include "core/CommandResult.hpp"
#include "core/Duration.hpp"
#include "core/Error.hpp"
#include "core/FrameRate.hpp"
#include "core/Project.hpp"
#include "core/TimelineEngine.hpp"
#include "core/Track.hpp"
#include "core/Uuid.hpp"

namespace palmier {
namespace {

// --- Fixtures --------------------------------------------------------------

constexpr Duration ms(std::int64_t v) { return Duration::fromMilliseconds(v); }
constexpr Duration ns(std::int64_t v) { return Duration::fromNanoseconds(v); }

Clip makeClip(ClipId id, Duration timelineStart, Duration sourceIn, Duration sourceOut) {
    Clip clip;
    clip.id = id;
    clip.assetRef = MediaAssetRef(Uuid::generateV4(), "mem://asset");
    clip.timelineStart = timelineStart;
    clip.sourceIn = sourceIn;
    clip.sourceOut = sourceOut;
    return clip;
}

// A project with a single empty video track, returning the track id.
Project makeProjectWithOneTrack(Uuid& trackIdOut) {
    Project project;
    project.id = Uuid::generateV4();
    project.name = "test";
    project.timelineFps = FrameRate::fps24();
    Track track;
    track.id = Uuid::generateV4();
    track.kind = TrackKind::Video;
    trackIdOut = track.id;
    project.tracks.push_back(std::move(track));
    return project;
}

// A compact, order-preserving fingerprint of a track's clips: (id, start, end)
// per clip. Two projects with equal fingerprints are indistinguishable to the
// timeline model, so this is the "left unchanged" oracle for Requirements 2.6/2.10.
struct ClipFingerprint {
    ClipId   id;
    Duration start;
    Duration end;
    friend bool operator==(const ClipFingerprint&, const ClipFingerprint&) = default;
};

std::vector<ClipFingerprint> fingerprint(const Project& project) {
    std::vector<ClipFingerprint> out;
    for (const auto& track : project.tracks) {
        for (const auto& clip : track.clips) {
            out.push_back({clip.id, clip.timelineStart, clip.timelineEnd()});
        }
    }
    return out;
}

// ===========================================================================
// Clip math edge cases
// ===========================================================================

TEST(ClipMath, DurationIsSourceOutMinusSourceIn) {
    const Clip clip = makeClip(Uuid::generateV4(), ms(1000), ms(200), ms(900));
    EXPECT_EQ(clip.duration(), ms(700));
}

TEST(ClipMath, TimelineEndIsStartPlusDuration) {
    const Clip clip = makeClip(Uuid::generateV4(), ms(1000), ms(200), ms(900));
    EXPECT_EQ(clip.timelineEnd(), ms(1700));
    EXPECT_EQ(clip.timelineEnd(), clip.timelineStart + clip.duration());
}

TEST(ClipMath, ZeroLengthClipHasZeroDuration) {
    // A clip whose in- and out-points coincide has no duration. (The engine
    // rejects such clips via its positive-duration invariant; the arithmetic
    // itself is still well defined.)
    const Clip clip = makeClip(Uuid::generateV4(), ms(0), ms(500), ms(500));
    EXPECT_TRUE(clip.duration().isZero());
    EXPECT_EQ(clip.timelineEnd(), clip.timelineStart);
}

TEST(ClipMath, SingleTickClipHasOneTickDuration) {
    // The smallest positive clip: a one-nanosecond source range.
    const Clip clip = makeClip(Uuid::generateV4(), ns(0), ns(0), ns(1));
    EXPECT_EQ(clip.duration(), ns(1));
    EXPECT_TRUE(clip.duration().isPositive());
}

TEST(ClipMath, OneFrameClipDurationEqualsFrameDuration) {
    const FrameRate fps = FrameRate::fps24();
    const Duration frame = fps.frameDuration();
    const Clip clip = makeClip(Uuid::generateV4(), ms(0), Duration::zero(), frame);
    EXPECT_EQ(clip.duration(), frame);
    // A one-frame clip and "the duration of one frame" are the same quantity.
    EXPECT_EQ(clip.duration(), fps.durationForFrames(1));
    EXPECT_TRUE(clip.duration().isPositive());
}

TEST(ClipMath, FrameDurationRoundTripsExactlyForEvenlyDivisibleRates) {
    // For rates whose frame period divides the nanosecond tick evenly (25 and 50
    // fps), duration -> frames -> duration round-trips exactly at the small-count
    // boundaries the split/trim math relies on.
    for (const FrameRate fps : {FrameRate::fps25(), FrameRate::fps50()}) {
        for (std::int64_t n : {0, 1, 2, 3, 10}) {
            const Duration d = fps.durationForFrames(n);
            EXPECT_EQ(fps.framesForDuration(d), n)
                << "fps=" << fps.numerator() << "/" << fps.denominator()
                << " frames=" << n;
        }
    }
}

TEST(ClipMath, FramesForDurationIsAFloorForNonDivisibleRates) {
    // 24 / 30 / 23.976 fps are not exactly representable in integer nanoseconds,
    // so framesForDuration floors: it never reports MORE whole frames than fit
    // within the (rounded-down) frame duration. This documents the by-design
    // lossiness rather than assuming an exact round-trip.
    for (const FrameRate fps : {FrameRate::fps24(), FrameRate::fps30(),
                                FrameRate::fps23_976()}) {
        for (std::int64_t n : {0, 1, 2, 3, 10}) {
            const Duration d = fps.durationForFrames(n);
            EXPECT_LE(fps.framesForDuration(d), n)
                << "fps=" << fps.numerator() << "/" << fps.denominator()
                << " frames=" << n;
        }
    }
}

TEST(ClipMath, DurationSummationEqualsScalarMultiple) {
    // Duration is integer-tick, so repeated addition of the same value is exact
    // and associative (no floating-point drift): summing a unit N times equals
    // N * unit. (This is a property of Duration arithmetic itself, independent of
    // the lossy frame<->duration conversion above.)
    const Duration unit = FrameRate::fps30().frameDuration();
    Duration sum = Duration::zero();
    constexpr std::int64_t kCount = 90;
    for (std::int64_t i = 0; i < kCount; ++i) {
        sum += unit;
    }
    EXPECT_EQ(sum, unit * kCount);
}

// ===========================================================================
// Undo/redo with empty history (Requirement 2.10)
// ===========================================================================

TEST(EmptyHistory, UndoOnFreshEngineIsNoOpWithIndicationAndLeavesProjectUnchanged) {
    Uuid trackId;
    TimelineEngine engine(makeProjectWithOneTrack(trackId));
    const auto before = fingerprint(engine.snapshot());

    const auto result = engine.undo();

    EXPECT_TRUE(result.isNoOp());
    EXPECT_FALSE(result.changed());
    EXPECT_FALSE(result.isError());
    EXPECT_FALSE(result.message().empty());  // an indication is surfaced
    EXPECT_FALSE(engine.canUndo());
    EXPECT_EQ(fingerprint(engine.snapshot()), before);
}

TEST(EmptyHistory, RedoOnFreshEngineIsNoOpWithIndicationAndLeavesProjectUnchanged) {
    Uuid trackId;
    TimelineEngine engine(makeProjectWithOneTrack(trackId));
    const auto before = fingerprint(engine.snapshot());

    const auto result = engine.redo();

    EXPECT_TRUE(result.isNoOp());
    EXPECT_FALSE(result.message().empty());
    EXPECT_FALSE(engine.canRedo());
    EXPECT_EQ(fingerprint(engine.snapshot()), before);
}

TEST(EmptyHistory, UndoPastBeginningIsNoOp) {
    Uuid trackId;
    TimelineEngine engine(makeProjectWithOneTrack(trackId));
    const ClipId id = Uuid::generateV4();
    ASSERT_TRUE(engine.apply(std::make_unique<AddClipCommand>(
                                 trackId, makeClip(id, ms(0), ms(0), ms(1000))))
                    .changed());

    // First undo reverts the single edit; the second has nothing left to do.
    ASSERT_TRUE(engine.undo().changed());
    const auto beyond = engine.undo();
    EXPECT_TRUE(beyond.isNoOp());
    EXPECT_FALSE(beyond.message().empty());
    EXPECT_FALSE(engine.canUndo());
    EXPECT_TRUE(engine.snapshot().tracks[0].clips.empty());
}

TEST(EmptyHistory, RedoPastEndIsNoOp) {
    Uuid trackId;
    TimelineEngine engine(makeProjectWithOneTrack(trackId));
    const ClipId id = Uuid::generateV4();
    ASSERT_TRUE(engine.apply(std::make_unique<AddClipCommand>(
                                 trackId, makeClip(id, ms(0), ms(0), ms(1000))))
                    .changed());
    ASSERT_TRUE(engine.undo().changed());

    // First redo re-applies; the second has nothing left to redo.
    ASSERT_TRUE(engine.redo().changed());
    const auto beyond = engine.redo();
    EXPECT_TRUE(beyond.isNoOp());
    EXPECT_FALSE(beyond.message().empty());
    EXPECT_FALSE(engine.canRedo());
    EXPECT_TRUE(engine.clip(id).has_value());
}

// ===========================================================================
// Split when the playhead misses all clips (Requirement 2.6)
// ===========================================================================

TEST(SplitMiss, PlayheadInGapBetweenClipsLeavesAllClipsUnchanged) {
    // Two clips with a gap: A[0,500) and B[1000,1500). A playhead anywhere in the
    // gap (750) lies within NO clip, so a split targeting either clip must fail
    // and leave the whole timeline unchanged.
    Uuid trackId;
    Project project = makeProjectWithOneTrack(trackId);
    const ClipId a = Uuid::generateV4();
    const ClipId b = Uuid::generateV4();
    project.tracks[0].clips = {makeClip(a, ms(0), ms(0), ms(500)),
                               makeClip(b, ms(1000), ms(0), ms(500))};
    TimelineEngine engine(std::move(project));
    const auto before = fingerprint(engine.snapshot());

    const auto onA = engine.apply(std::make_unique<SplitClipCommand>(a, ms(750)));
    EXPECT_TRUE(onA.isError());
    EXPECT_EQ(onA.error().code(), ErrorCode::FailedPrecondition);
    EXPECT_FALSE(onA.error().message().empty());  // "nothing to split" indication

    const auto onB = engine.apply(std::make_unique<SplitClipCommand>(b, ms(750)));
    EXPECT_TRUE(onB.isError());
    EXPECT_EQ(onB.error().code(), ErrorCode::FailedPrecondition);

    // Nothing was committed and no undo entry was recorded for the misses.
    EXPECT_EQ(fingerprint(engine.snapshot()), before);
    EXPECT_FALSE(engine.canUndo());
}

TEST(SplitMiss, PlayheadAtExactBoundariesIsNotASplit) {
    // The playhead must fall STRICTLY inside the clip. At the leading and
    // trailing edges there is nothing to split.
    Uuid trackId;
    Project project = makeProjectWithOneTrack(trackId);
    const ClipId a = Uuid::generateV4();
    project.tracks[0].clips = {makeClip(a, ms(1000), ms(0), ms(1000))};  // [1000,2000)
    const auto before = fingerprint(project);

    SplitClipCommand atStart(a, ms(1000));
    EXPECT_TRUE(atStart.apply(project).isError());
    SplitClipCommand atEnd(a, ms(2000));
    EXPECT_TRUE(atEnd.apply(project).isError());

    EXPECT_EQ(fingerprint(project), before);
    ASSERT_EQ(project.tracks[0].clips.size(), 1u);
    EXPECT_EQ(project.tracks[0].clips[0].duration(), ms(1000));
}

TEST(SplitMiss, PlayheadBeforeAndAfterTheOnlyClipLeavesItUnchanged) {
    Uuid trackId;
    Project project = makeProjectWithOneTrack(trackId);
    const ClipId a = Uuid::generateV4();
    project.tracks[0].clips = {makeClip(a, ms(1000), ms(0), ms(1000))};  // [1000,2000)
    const auto priorFingerprint = fingerprint(project);

    SplitClipCommand beforeClip(a, ms(200));   // well before the clip
    SplitClipCommand afterClip(a, ms(9000));   // well after the clip
    EXPECT_TRUE(beforeClip.apply(project).isError());
    EXPECT_TRUE(afterClip.apply(project).isError());

    EXPECT_EQ(fingerprint(project), priorFingerprint);
}

TEST(SplitMiss, SplitOnNonexistentClipFailsWithNotFound) {
    Uuid trackId;
    Project project = makeProjectWithOneTrack(trackId);
    project.tracks[0].clips = {makeClip(Uuid::generateV4(), ms(0), ms(0), ms(1000))};
    const auto before = fingerprint(project);

    SplitClipCommand cmd(Uuid::generateV4(), ms(500));  // id not on any track
    const auto result = cmd.apply(project);
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::NotFound);
    EXPECT_EQ(fingerprint(project), before);
}

// Positive control tying clip math to the split boundary: an interior split at
// the exact midpoint of a two-tick clip yields two single-tick halves whose
// combined duration and source range equal the original (Requirement 2.5),
// contrasting the miss cases above.
TEST(SplitMiss, InteriorSplitOfSmallestClipYieldsEqualHalves) {
    Uuid trackId;
    Project project = makeProjectWithOneTrack(trackId);
    const ClipId a = Uuid::generateV4();
    project.tracks[0].clips = {makeClip(a, ns(0), ns(0), ns(2))};  // 2-tick clip

    SplitClipCommand cmd(a, ns(1));  // strictly interior
    ASSERT_TRUE(cmd.apply(project).isOk());

    const auto& clips = project.tracks[0].clips;
    ASSERT_EQ(clips.size(), 2u);
    EXPECT_EQ(clips[0].duration(), ns(1));
    EXPECT_EQ(clips[1].duration(), ns(1));
    EXPECT_EQ(clips[1].timelineStart, clips[0].timelineEnd());  // contiguous
    EXPECT_EQ(clips[0].duration() + clips[1].duration(), ns(2));
    EXPECT_EQ(clips[0].sourceOut, clips[1].sourceIn);
}

}  // namespace
}  // namespace palmier
