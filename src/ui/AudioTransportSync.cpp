// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/AudioTransportSync.cpp — implementation of the playback lifecycle decisions.

#include "ui/AudioTransportSync.hpp"

#include <algorithm>

namespace palmier::ui {

std::size_t AudioTransportSync::quantaFor(Duration lead) {
    const std::int64_t quantum = options_.quantumDuration.ticks();
    if (quantum <= 0) return 0;

    const std::int64_t deficit = options_.targetLead.ticks() - lead.ticks();
    if (deficit <= 0) return 0;  // already far enough ahead: Requirement 3A.4

    // Round UP, so a deficit smaller than one quantum still asks for the one
    // quantum that covers it rather than for none — otherwise a small persistent
    // shortfall would never be made up.
    const std::int64_t wanted = (deficit + quantum - 1) / quantum;
    const auto         capped = static_cast<std::size_t>(
        std::min<std::int64_t>(wanted, static_cast<std::int64_t>(options_.maxQuantaPerCycle)));
    if (static_cast<std::int64_t>(capped) < wanted) {
        ++stats_.clampedCycles;
    }
    return capped;
}

AudioTransportDecision AudioTransportSync::decide(const AudioTransportObservation& observation) {
    AudioTransportDecision decision;

    // Requirement 3A.6: a scrub gesture owns the engine outright. Standing off is
    // not an optimisation — two owners issuing start()/stop() at each other would
    // produce exactly the stutter scrubbing exists to avoid.
    if (observation.scrubbing) {
        ++stats_.deferredToScrub;
        return decision;
    }

    if (!observation.transportPlaying) {
        if (observation.engineRunning) {
            decision.action = AudioTransportAction::Stop;
            ++stats_.stops;
            return decision;
        }
        ++stats_.idleCycles;
        return decision;
    }

    // Playing from here on.
    if (!observation.engineRunning) {
        decision.action = AudioTransportAction::Start;
        decision.position = observation.transportPosition;
        // A fresh run has played nothing, so its lead is zero and this cycle asks
        // for a full lead's worth up front rather than waiting for the next tick.
        decision.quantaToPump = quantaFor(Duration::zero());
        ++stats_.starts;
        stats_.quantaRequested += decision.quantaToPump;
        return decision;
    }

    // Requirement 3A.2: a playhead that has moved somewhere this run cannot reach
    // by playing on is a seek. Measured against the PLAYED-OUT position, which is
    // what the user is hearing; the tolerance exceeds the target lead because
    // during healthy playback the transport legitimately runs ahead of the audio by
    // up to that lead (see the option's own note).
    const Duration drift = (observation.transportPosition - observation.presentationPosition).abs();
    if (drift > options_.seekTolerance) {
        decision.action = AudioTransportAction::Restart;
        decision.position = observation.transportPosition;
        decision.quantaToPump = quantaFor(Duration::zero());
        ++stats_.restarts;
        stats_.quantaRequested += decision.quantaToPump;
        return decision;
    }

    // Steady state: keep the lead topped up. `mixPosition` is documented to be at
    // or ahead of `presentationPosition`, but a negative difference is treated as
    // zero lead rather than trusted, so a surprising observation asks for audio
    // instead of silently asking for none.
    const Duration lead = observation.mixPosition - observation.presentationPosition;
    decision.quantaToPump = quantaFor(lead.isNegative() ? Duration::zero() : lead);
    stats_.quantaRequested += decision.quantaToPump;
    if (decision.quantaToPump == 0) {
        ++stats_.idleCycles;
    }
    return decision;
}

} // namespace palmier::ui
