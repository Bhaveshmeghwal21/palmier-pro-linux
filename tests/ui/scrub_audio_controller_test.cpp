// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/ui/scrub_audio_controller_test.cpp — tests for the scrub-audio decision
// logic (monitoring-and-grading Requirement 3).
//
// Scrubbing's correctness is mostly about what must NOT happen, so most of these
// assert a negative:
//
//   * Requirement 3.3 — suppression, by setting or by missing device, must not
//     change the DRAG. Every suppressed case therefore checks that the gesture is
//     still tracked and that nothing reported an error.
//   * Requirement 3.5 — a flood of positions must be DROPPED, not queued. The drop
//     is asserted as a distinguishable outcome and as a count, because a
//     controller that silently deferred positions would look identical from the
//     outside until the audio fell behind the drag.
//   * Requirement 3.2 — the transport is owed its prior state back whether or not
//     the drag was ever audible, so resumption is checked in the suppressed case
//     too.
//
// Requirement 3.4 (no project, undo or committed-playhead change) is not tested
// here because it cannot be violated here: this class has no Project, no
// TimelineEngine and no way to reach one. It is proved by construction, and
// separately observed end to end in the shell tests.
//
// Time is an argument, so the 60 ms drop interval and the 200 ms stop deadline are
// driven with simulated milliseconds and these run in microseconds.

#include <chrono>
#include <cstdint>

#include <gtest/gtest.h>

#include "core/Duration.hpp"
#include "ui/ScrubAudioController.hpp"

namespace palmier::ui {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

[[nodiscard]] Clock::time_point origin() {
    static const Clock::time_point t0 = Clock::now();
    return t0;
}

[[nodiscard]] Duration at(std::int64_t ms) { return Duration::fromMilliseconds(ms); }

// ---------------------------------------------------------------------------
// The ordinary gesture
// ---------------------------------------------------------------------------

TEST(ScrubAudioController, StartsUnsuppressedAndIdle) {
    const ScrubAudioController controller;
    EXPECT_TRUE(controller.isEnabled());
    EXPECT_TRUE(controller.isOutputAvailable());
    EXPECT_FALSE(controller.isSuppressed());
    EXPECT_FALSE(controller.isDragging());
    EXPECT_FALSE(controller.isScrubbing());
    EXPECT_FALSE(controller.lastPosition().has_value());
}

TEST(ScrubAudioController, ADragStartsScrubAudioAtThePressPosition) {
    ScrubAudioController controller;
    const ScrubAudioDecision begin =
        controller.beginDrag(at(1'000), /*transportWasPlaying=*/false, origin());

    EXPECT_EQ(begin.action, ScrubAudioAction::Start);
    EXPECT_EQ(begin.position, at(1'000));
    EXPECT_FALSE(begin.dropped);
    EXPECT_TRUE(controller.isDragging());
    EXPECT_TRUE(controller.isScrubbing());
    EXPECT_EQ(controller.stats().starts, 1u);
}

TEST(ScrubAudioController, MovingFarEnoughApartRepositionsTheAudio) {
    ScrubAudioController controller;
    (void)controller.beginDrag(at(0), false, origin());

    const ScrubAudioDecision moved =
        controller.dragTo(at(500), origin() + ScrubAudioController::kMinRepositionInterval);
    EXPECT_EQ(moved.action, ScrubAudioAction::Restart);
    EXPECT_EQ(moved.position, at(500));
    EXPECT_FALSE(moved.dropped);
    EXPECT_EQ(controller.stats().restarts, 1u);
    EXPECT_EQ(controller.stats().drops, 0u);
}

TEST(ScrubAudioController, ReleasingAStoppedTransportLeavesItStoppedAtTheLastPosition) {
    ScrubAudioController controller;
    (void)controller.beginDrag(at(0), /*transportWasPlaying=*/false, origin());
    (void)controller.dragTo(at(2'500), origin() + 100ms);

    const ScrubAudioDecision end = controller.endDrag(origin() + 120ms);
    EXPECT_EQ(end.action, ScrubAudioAction::Stop);
    EXPECT_EQ(end.position, at(2'500));
    EXPECT_FALSE(controller.isDragging());
    EXPECT_FALSE(controller.isScrubbing());
}

TEST(ScrubAudioController, ReleasingAPlayingTransportResumesPlaybackAtTheLastPosition) {
    // Requirement 3.2's other half: the transport is owed the state it had.
    ScrubAudioController controller;
    (void)controller.beginDrag(at(0), /*transportWasPlaying=*/true, origin());
    EXPECT_TRUE(controller.transportWasPlaying());
    (void)controller.dragTo(at(4'000), origin() + 100ms);

    const ScrubAudioDecision end = controller.endDrag(origin() + 110ms);
    EXPECT_EQ(end.action, ScrubAudioAction::StopAndResume);
    EXPECT_EQ(end.position, at(4'000));
    // And the flag is consumed, so a second gesture does not inherit it.
    EXPECT_FALSE(controller.transportWasPlaying());
}

// ---------------------------------------------------------------------------
// Requirement 3.5 — drop, never delay
// ---------------------------------------------------------------------------

TEST(ScrubAudioController, APositionArrivingTooSoonIsDroppedRatherThanDeferred) {
    ScrubAudioController controller;
    (void)controller.beginDrag(at(0), false, origin());

    const ScrubAudioDecision tooSoon = controller.dragTo(at(10), origin() + 1ms);
    EXPECT_EQ(tooSoon.action, ScrubAudioAction::None);
    EXPECT_TRUE(tooSoon.dropped) << "a drop must be distinguishable from having nothing to do";
    EXPECT_EQ(tooSoon.position, at(10)) << "the position is still reported for the caller's use";
    EXPECT_EQ(controller.stats().drops, 1u);
    EXPECT_EQ(controller.stats().restarts, 0u);
}

TEST(ScrubAudioController, AFastDragRepositionsAtABoundedRateAndDropsTheRest) {
    // A mouse drag produces a position per move event; the audio path cannot serve
    // them all. 100 positions over one simulated second must yield repositions at
    // the interval and drops for everything between — never a growing backlog.
    ScrubAudioController controller;
    (void)controller.beginDrag(at(0), false, origin());

    for (int step = 1; step <= 100; ++step) {
        const auto when = origin() + std::chrono::milliseconds{10 * step};
        (void)controller.dragTo(at(10 * step), when);
    }

    const ScrubAudioStats stats = controller.stats();
    // Repositions land at 60, 120, ... 960 ms: sixteen of them.
    EXPECT_EQ(stats.restarts, 16u);
    EXPECT_EQ(stats.drops, 84u);
    EXPECT_EQ(stats.restarts + stats.drops, 100u) << "every position was accounted for";
    EXPECT_EQ(stats.starts, 1u);
}

TEST(ScrubAudioController, TheDropIntervalIsWellInsideTheStopDeadline) {
    // If repositions were rarer than the stop deadline, a drag could end before its
    // first reposition and the feature would be inaudible in short gestures.
    EXPECT_LT(ScrubAudioController::kMinRepositionInterval,
              ScrubAudioController::kStopDeadline);
    EXPECT_EQ(ScrubAudioController::kStopDeadline, 200ms) << "Requirement 3.2's bound";
}

// ---------------------------------------------------------------------------
// Requirement 3.3 — suppression must not change the drag
// ---------------------------------------------------------------------------

TEST(ScrubAudioController, TheSettingSuppressesTheAudioButNotTheGesture) {
    ScrubAudioController controller;
    EXPECT_TRUE(controller.setEnabled(false).isNoOp()) << "nothing was running to stop";
    EXPECT_TRUE(controller.isSuppressed());

    const ScrubAudioDecision begin = controller.beginDrag(at(1'000), false, origin());
    EXPECT_EQ(begin.action, ScrubAudioAction::None);
    EXPECT_FALSE(begin.dropped) << "suppressed is not the same as dropped";

    // The drag itself is entirely unaffected — which is the requirement.
    EXPECT_TRUE(controller.isDragging());
    EXPECT_FALSE(controller.isScrubbing());
    EXPECT_EQ(controller.stats().suppressedDrags, 1u);
    EXPECT_EQ(controller.stats().starts, 0u);

    // Moving still tracks the position and still costs nothing.
    const ScrubAudioDecision moved = controller.dragTo(at(2'000), origin() + 500ms);
    EXPECT_EQ(moved.action, ScrubAudioAction::None);
    EXPECT_FALSE(moved.dropped);
    EXPECT_EQ(controller.lastPosition(), at(2'000));
}

TEST(ScrubAudioController, AMissingOutputDeviceSuppressesWithoutASettingChangeOrAnError) {
    ScrubAudioController controller;
    EXPECT_TRUE(controller.setOutputAvailable(false).isNoOp());
    EXPECT_TRUE(controller.isSuppressed());
    EXPECT_TRUE(controller.isEnabled()) << "the user's setting is untouched by a missing device";

    const ScrubAudioDecision begin = controller.beginDrag(at(0), false, origin());
    EXPECT_EQ(begin.action, ScrubAudioAction::None);
    EXPECT_TRUE(controller.isDragging());
    EXPECT_EQ(controller.stats().suppressedDrags, 1u);
}

TEST(ScrubAudioController, SwitchingTheSettingOffMidDragSilencesImmediately) {
    // Not at the next mouse move: the user asked for silence, so sound must stop
    // now even if the pointer never moves again.
    ScrubAudioController controller;
    ASSERT_EQ(controller.beginDrag(at(0), false, origin()).action, ScrubAudioAction::Start);
    ASSERT_TRUE(controller.isScrubbing());

    const ScrubAudioDecision off = controller.setEnabled(false);
    EXPECT_EQ(off.action, ScrubAudioAction::Stop);
    EXPECT_FALSE(controller.isScrubbing());
    EXPECT_TRUE(controller.isDragging()) << "the gesture continues; only the audio stopped";
}

TEST(ScrubAudioController, LosingTheOutputDeviceMidDragSilencesImmediately) {
    ScrubAudioController controller;
    ASSERT_EQ(controller.beginDrag(at(0), false, origin()).action, ScrubAudioAction::Start);

    const ScrubAudioDecision lost = controller.setOutputAvailable(false);
    EXPECT_EQ(lost.action, ScrubAudioAction::Stop);
    EXPECT_FALSE(controller.isScrubbing());
    EXPECT_TRUE(controller.isDragging());
}

TEST(ScrubAudioController, ReEnablingMidDragStartsAtAPositionTheUserHasActuallyReached) {
    // Starting at the stale press position would play audio from somewhere the
    // pointer left long ago, so the start waits for the next real move.
    ScrubAudioController controller;
    (void)controller.setEnabled(false);
    (void)controller.beginDrag(at(0), false, origin());

    const ScrubAudioDecision on = controller.setEnabled(true);
    EXPECT_TRUE(on.isNoOp()) << "no audio starts from a stale position";
    EXPECT_FALSE(controller.isScrubbing());

    const ScrubAudioDecision moved = controller.dragTo(at(5'000), origin() + 300ms);
    EXPECT_EQ(moved.action, ScrubAudioAction::Start) << "Start, not Restart: nothing was playing";
    EXPECT_EQ(moved.position, at(5'000));
    EXPECT_TRUE(controller.isScrubbing());
    EXPECT_EQ(controller.stats().starts, 1u);
    EXPECT_EQ(controller.stats().restarts, 0u);
}

TEST(ScrubAudioController, ASuppressedDragStillOwesTheTransportItsPriorState) {
    // Requirement 3.2 is about the transport, not about what was audible: a drag
    // that made no sound must still resume playback if it interrupted playback.
    ScrubAudioController controller;
    (void)controller.setOutputAvailable(false);
    (void)controller.beginDrag(at(0), /*transportWasPlaying=*/true, origin());
    (void)controller.dragTo(at(7'000), origin() + 200ms);

    const ScrubAudioDecision end = controller.endDrag(origin() + 210ms);
    EXPECT_EQ(end.action, ScrubAudioAction::StopAndResume);
    EXPECT_EQ(end.position, at(7'000)) << "at the position actually reached";
}

TEST(ScrubAudioController, SettingAValueItAlreadyHasChangesNothing) {
    ScrubAudioController controller;
    (void)controller.beginDrag(at(0), false, origin());
    ASSERT_TRUE(controller.isScrubbing());

    EXPECT_TRUE(controller.setEnabled(true).isNoOp());
    EXPECT_TRUE(controller.setOutputAvailable(true).isNoOp());
    EXPECT_TRUE(controller.isScrubbing()) << "a redundant setter must not stop the audio";
    EXPECT_EQ(controller.stats().stops, 0u);
}

// ---------------------------------------------------------------------------
// Degenerate orderings
// ---------------------------------------------------------------------------

TEST(ScrubAudioController, MovingOrReleasingWithoutADragDoesNothing) {
    ScrubAudioController controller;
    EXPECT_TRUE(controller.dragTo(at(1'000), origin()).isNoOp());
    EXPECT_FALSE(controller.dragTo(at(1'000), origin()).dropped);
    EXPECT_TRUE(controller.endDrag(origin()).isNoOp());
    EXPECT_FALSE(controller.isDragging());
    EXPECT_EQ(controller.stats().starts, 0u);
    EXPECT_EQ(controller.stats().drops, 0u);
}

TEST(ScrubAudioController, ReleasingTwiceStopsOnlyOnce) {
    ScrubAudioController controller;
    (void)controller.beginDrag(at(0), false, origin());
    ASSERT_EQ(controller.endDrag(origin() + 10ms).action, ScrubAudioAction::Stop);
    EXPECT_TRUE(controller.endDrag(origin() + 20ms).isNoOp());
    EXPECT_EQ(controller.stats().stops, 1u);
}

TEST(ScrubAudioController, ACancelledDragStopsWithoutResuming) {
    // A cancelled gesture has no position to resume at, so it must not resume even
    // though the transport was playing.
    ScrubAudioController controller;
    (void)controller.beginDrag(at(0), /*transportWasPlaying=*/true, origin());
    ASSERT_TRUE(controller.isScrubbing());

    const ScrubAudioDecision cancelled = controller.cancelDrag();
    EXPECT_EQ(cancelled.action, ScrubAudioAction::Stop);
    EXPECT_NE(cancelled.action, ScrubAudioAction::StopAndResume);
    EXPECT_FALSE(controller.isDragging());
    EXPECT_FALSE(controller.isScrubbing());
    EXPECT_FALSE(controller.transportWasPlaying());
}

TEST(ScrubAudioController, ASecondDragBehavesLikeTheFirst) {
    ScrubAudioController controller;
    (void)controller.beginDrag(at(0), false, origin());
    (void)controller.endDrag(origin() + 100ms);

    const ScrubAudioDecision again = controller.beginDrag(at(9'000), true, origin() + 200ms);
    EXPECT_EQ(again.action, ScrubAudioAction::Start);
    EXPECT_EQ(again.position, at(9'000));
    EXPECT_TRUE(controller.transportWasPlaying());
    EXPECT_EQ(controller.stats().starts, 2u);

    // The reposition clock restarted with the new drag, so an immediate move drops
    // rather than repositioning off the previous gesture's timestamp.
    EXPECT_TRUE(controller.dragTo(at(9'010), origin() + 205ms).dropped);
}

} // namespace
} // namespace palmier::ui
