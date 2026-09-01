// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/ScrubAudioController.hpp — Qt-free scrub-audio decision logic
// (monitoring-and-grading Requirement 3).
//
// Scrubbing is the one audio feature whose correctness is almost entirely about
// what NOT to do. Playing sound while the playhead is dragged is easy; the hard
// parts are all constraints:
//
//   * the drag must never wait for audio (Requirement 3.3, 3.5);
//   * the transport must end up exactly as it started (Requirement 3.2);
//   * nothing may reach the project or the undo history (Requirement 3.4).
//
// So this class decides and remembers, and does no I/O at all. It returns an
// INTENT — start, restart, stop, stop-and-resume — and its caller performs it
// against the Audio_Engine. Three things follow from that shape, and they are the
// reason for it:
//
//   1. **Requirement 3.4 is structural rather than tested.** This class has no
//      Project, no TimelineEngine, no undo stack and no way to reach one. It
//      cannot modify the project even incorrectly.
//   2. **Requirement 3.5 becomes a decision, not a race.** A drag emits far more
//      positions than any decoder can seek to. Rather than queue them and fall
//      behind, intermediate positions are DROPPED: `dragTo()` returns "nothing to
//      do, and I dropped one" and the caller moves on. The drag is never delayed
//      because the drag never waits on anything here.
//   3. **The timing rules are testable without sleeping.** Time arrives as an
//      argument, exactly as in AudioMeterViewModel, so a test drives simulated
//      milliseconds across the drop interval and the stop deadline.

#ifndef PALMIER_UI_SCRUBAUDIOCONTROLLER_HPP
#define PALMIER_UI_SCRUBAUDIOCONTROLLER_HPP

#include <chrono>
#include <cstdint>
#include <optional>

#include "core/Duration.hpp"

namespace palmier::ui {

/// What the caller should do to the Audio_Engine right now.
enum class ScrubAudioAction {
    /// Nothing. Either scrub audio is suppressed, no drag is in progress, or this
    /// position was dropped to keep up with the drag.
    None,
    /// Begin scrub playback at the decision's position.
    Start,
    /// Reposition scrub playback: stop and begin again at the new position.
    Restart,
    /// End scrub playback and leave the transport stopped, which is what it was
    /// before the drag began.
    Stop,
    /// End scrub playback and resume ordinary playback at the decision's position,
    /// because the transport WAS playing when the drag began (Requirement 3.2).
    StopAndResume,
};

/// One decision. `dropped` distinguishes "there was nothing to do" from "there was
/// something to do and I deliberately skipped it", which is what makes
/// Requirement 3.5's drop-rather-than-delay policy observable instead of invisible.
struct ScrubAudioDecision {
    ScrubAudioAction action = ScrubAudioAction::None;
    Duration         position{};
    bool             dropped = false;

    [[nodiscard]] bool isNoOp() const noexcept { return action == ScrubAudioAction::None; }
};

/// Monotonic counters over the controller's lifetime.
struct ScrubAudioStats {
    std::uint64_t starts{0};
    std::uint64_t restarts{0};
    std::uint64_t stops{0};
    /// Positions skipped to keep the drag responsive (Requirement 3.5).
    std::uint64_t drops{0};
    /// Drags that began while scrub audio was suppressed (Requirement 3.3). The
    /// drag still ran; only the audio did not.
    std::uint64_t suppressedDrags{0};
};

class ScrubAudioController {
public:
    using TimePoint = std::chrono::steady_clock::time_point;

    /// Minimum interval between two scrub repositions.
    ///
    /// A mouse drag produces a position per mouse-move event — far more than a
    /// decoder can seek to. This interval is what converts that flood into a rate
    /// the audio path can actually serve, and every position arriving inside it is
    /// dropped rather than queued. 60 ms is roughly 16 repositions a second: fast
    /// enough to hear a word boundary, slow enough that each one can complete.
    static constexpr std::chrono::milliseconds kMinRepositionInterval{60};

    /// Requirement 3.2's bound: scrub audio must stop within 200 ms of the drag
    /// ending. Exposed so the caller and the tests share one number, and so a
    /// caller that defers its stop can check itself against it.
    static constexpr std::chrono::milliseconds kStopDeadline{200};

    ScrubAudioController() = default;

    // --- Suppression (Requirement 3.3) --------------------------------------

    /// The user-visible setting. Returns the action this requires NOW: switching it
    /// off part-way through a drag has to stop audio already playing rather than
    /// wait for the next mouse move, which would leave sound running after the user
    /// asked for silence.
    [[nodiscard]] ScrubAudioDecision setEnabled(bool enabled);

    /// Whether a real output device is available. False suppresses scrub audio
    /// automatically, with no setting change and no error — the same "silent, not
    /// failed" rule the Audio_Engine already applies to a missing device.
    [[nodiscard]] ScrubAudioDecision setOutputAvailable(bool available);

    [[nodiscard]] bool isEnabled() const noexcept { return enabled_; }
    [[nodiscard]] bool isOutputAvailable() const noexcept { return outputAvailable_; }

    /// True when scrub audio will not sound, for either reason. A suppressed
    /// controller still tracks the drag: suppression must not change how the drag
    /// itself behaves (Requirement 3.3's "without blocking or slowing the drag").
    [[nodiscard]] bool isSuppressed() const noexcept { return !enabled_ || !outputAvailable_; }

    // --- The drag -----------------------------------------------------------

    /// The user began dragging the playhead at `position`.
    ///
    /// `transportWasPlaying` is recorded now and returned to the caller at
    /// `endDrag()`, which is how Requirement 3.2's "leave the transport in the same
    /// state it was in before the drag" is honoured without the caller having to
    /// remember anything across the gesture.
    [[nodiscard]] ScrubAudioDecision beginDrag(Duration position, bool transportWasPlaying,
                                               TimePoint now);

    /// The drag moved to `position`.
    ///
    /// Returns Restart when enough time has passed since the last reposition, and
    /// otherwise a dropped no-op. Never blocks, never queues, and never reports an
    /// error: a dropped position costs the user a little audio resolution, whereas
    /// a delayed drag costs them the gesture (Requirement 3.5).
    [[nodiscard]] ScrubAudioDecision dragTo(Duration position, TimePoint now);

    /// The drag ended. Returns Stop, or StopAndResume when the transport was
    /// playing beforehand, at the last dragged position.
    [[nodiscard]] ScrubAudioDecision endDrag(TimePoint now);

    /// Abandon the gesture without resuming anything — for a cancelled drag or a
    /// window losing focus mid-gesture. Stops scrub audio if it is running and
    /// deliberately does NOT resume playback, because a cancelled gesture has no
    /// position to resume at.
    [[nodiscard]] ScrubAudioDecision cancelDrag();

    // --- Observation --------------------------------------------------------

    [[nodiscard]] bool isDragging() const noexcept { return dragging_; }
    /// True while the caller has been told to start and not yet told to stop.
    [[nodiscard]] bool isScrubbing() const noexcept { return scrubbing_; }
    [[nodiscard]] bool transportWasPlaying() const noexcept { return transportWasPlaying_; }
    /// The most recent dragged position, absent before any drag.
    [[nodiscard]] std::optional<Duration> lastPosition() const noexcept { return lastPosition_; }
    [[nodiscard]] ScrubAudioStats stats() const noexcept { return stats_; }

private:
    /// Build a stop decision for whatever is currently running, and clear the
    /// scrubbing flag. Returns a no-op when nothing is running, so a caller may ask
    /// twice without stopping something twice.
    [[nodiscard]] ScrubAudioDecision stopIfScrubbing();

    bool                     enabled_ = true;
    bool                     outputAvailable_ = true;
    bool                     dragging_ = false;
    bool                     scrubbing_ = false;
    bool                     transportWasPlaying_ = false;
    std::optional<Duration>  lastPosition_{};
    std::optional<TimePoint> lastRepositionAt_{};
    ScrubAudioStats          stats_{};
};

} // namespace palmier::ui

#endif // PALMIER_UI_SCRUBAUDIOCONTROLLER_HPP
