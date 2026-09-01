// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/ScrubAudioController.cpp — implementation of the scrub-audio decisions.

#include "ui/ScrubAudioController.hpp"

namespace palmier::ui {

ScrubAudioDecision ScrubAudioController::stopIfScrubbing() {
    ScrubAudioDecision decision;
    if (!scrubbing_) return decision;  // asking twice must not stop twice
    scrubbing_ = false;
    decision.action = ScrubAudioAction::Stop;
    decision.position = lastPosition_.value_or(Duration::zero());
    ++stats_.stops;
    return decision;
}

ScrubAudioDecision ScrubAudioController::setEnabled(bool enabled) {
    if (enabled == enabled_) return ScrubAudioDecision{};
    enabled_ = enabled;
    if (!enabled_) {
        // Switched off part-way through a drag: silence now, not at the next mouse
        // move. The drag itself is untouched — dragging_ stays true, so the gesture
        // continues normally and only the audio stops.
        return stopIfScrubbing();
    }
    // Switched on mid-drag: do not start here. The next dragTo() will, at a
    // position the user has actually reached, rather than at a stale one.
    return ScrubAudioDecision{};
}

ScrubAudioDecision ScrubAudioController::setOutputAvailable(bool available) {
    if (available == outputAvailable_) return ScrubAudioDecision{};
    outputAvailable_ = available;
    if (!outputAvailable_) return stopIfScrubbing();
    return ScrubAudioDecision{};
}

ScrubAudioDecision ScrubAudioController::beginDrag(Duration position, bool transportWasPlaying,
                                                   TimePoint now) {
    dragging_ = true;
    transportWasPlaying_ = transportWasPlaying;
    lastPosition_ = position;

    ScrubAudioDecision decision;
    decision.position = position;

    if (isSuppressed()) {
        // The drag is tracked either way (Requirement 3.3: suppression must not
        // change the gesture), but no audio starts and nothing fails.
        ++stats_.suppressedDrags;
        return decision;
    }

    lastRepositionAt_ = now;
    scrubbing_ = true;
    decision.action = ScrubAudioAction::Start;
    ++stats_.starts;
    return decision;
}

ScrubAudioDecision ScrubAudioController::dragTo(Duration position, TimePoint now) {
    if (!dragging_) return ScrubAudioDecision{};

    // Recorded before any suppression or drop decision, so endDrag() resumes at
    // the position the user actually reached even if no audio was ever played
    // there. Requirement 3.2 is about the transport, not about what was audible.
    lastPosition_ = position;

    ScrubAudioDecision decision;
    decision.position = position;
    if (isSuppressed()) return decision;

    // Requirement 3.5. A position arriving inside the interval is dropped, not
    // deferred: there is no queue to fall behind on and the caller returns
    // immediately, so the drag stays responsive however slow the decoder is.
    if (lastRepositionAt_.has_value() && (now - *lastRepositionAt_) < kMinRepositionInterval) {
        decision.dropped = true;
        ++stats_.drops;
        return decision;
    }

    lastRepositionAt_ = now;
    // A drag that began while suppressed, then had the setting switched on, starts
    // here rather than repositioning something that was never playing.
    decision.action = scrubbing_ ? ScrubAudioAction::Restart : ScrubAudioAction::Start;
    if (scrubbing_) {
        ++stats_.restarts;
    } else {
        scrubbing_ = true;
        ++stats_.starts;
    }
    return decision;
}

ScrubAudioDecision ScrubAudioController::endDrag(TimePoint /*now*/) {
    ScrubAudioDecision decision;
    if (!dragging_) return decision;

    const bool resume = transportWasPlaying_;
    const Duration position = lastPosition_.value_or(Duration::zero());

    dragging_ = false;
    lastRepositionAt_.reset();
    if (scrubbing_) {
        scrubbing_ = false;
        ++stats_.stops;
    }

    decision.position = position;
    // Requirement 3.2: the transport returns to the state it was in beforehand.
    // Resuming is reported even when scrub audio never sounded — the transport's
    // state is owed back regardless of whether the drag was audible, which is why
    // this is not conditional on `scrubbing_`.
    decision.action = resume ? ScrubAudioAction::StopAndResume : ScrubAudioAction::Stop;
    transportWasPlaying_ = false;
    return decision;
}

ScrubAudioDecision ScrubAudioController::cancelDrag() {
    ScrubAudioDecision decision = stopIfScrubbing();
    dragging_ = false;
    transportWasPlaying_ = false;
    lastRepositionAt_.reset();
    return decision;
}

} // namespace palmier::ui
