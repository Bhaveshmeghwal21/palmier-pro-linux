// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/ui/audio_transport_sync_test.cpp — tests for the audio playback lifecycle
// (monitoring-and-grading Requirement 3A).
//
// Because ui::AudioTransportSync is a pure function of an observation, the awkward
// transitions can be stated rather than provoked: a seek during playback, a scrub
// beginning mid-play, an engine that has fallen behind, a device that vanished.
// None of these need an audio device, a timer or a real engine.
//
// Two cases here guard against specific plausible bugs rather than against generic
// misbehaviour, and they are the reason the options are named constants:
//
//   * **A steady, healthy playback cycle must not be mistaken for a seek.** During
//     normal playback the transport's position legitimately runs AHEAD of the
//     played-out audio by up to the target lead. A seek tolerance below that lead
//     would diagnose a seek every cycle and restart the engine continuously, which
//     is audible as an unbroken stutter. So the invariant seekTolerance >
//     targetLead is asserted directly.
//   * **A cycle that is far enough ahead must ask for nothing.** Otherwise the
//     driver is a busy loop that pumps whenever it is asked (Requirement 3A.4).

#include <cstddef>
#include <cstdint>

#include <gtest/gtest.h>

#include "core/Duration.hpp"
#include "ui/AudioTransportSync.hpp"

namespace palmier::ui {
namespace {

[[nodiscard]] Duration ms(std::int64_t value) { return Duration::fromMilliseconds(value); }

/// A healthy playing observation: engine running, at `lead` ahead of playout.
[[nodiscard]] AudioTransportObservation playing(Duration playedOut, Duration lead) {
    AudioTransportObservation observation;
    observation.transportPlaying = true;
    observation.engineRunning = true;
    observation.presentationPosition = playedOut;
    observation.mixPosition = playedOut + lead;
    // The transport tracks what has been played out, which is what the master clock
    // makes it do once audio is running.
    observation.transportPosition = playedOut;
    return observation;
}

// ---------------------------------------------------------------------------
// Starting and stopping (Requirement 3A.1)
// ---------------------------------------------------------------------------

TEST(AudioTransportSync, AStoppedTransportWithAStoppedEngineIsCompletelyIdle) {
    AudioTransportSync sync;
    const AudioTransportDecision decision = sync.decide(AudioTransportObservation{});

    EXPECT_TRUE(decision.isIdle());
    EXPECT_EQ(decision.action, AudioTransportAction::None);
    EXPECT_EQ(decision.quantaToPump, 0u);
    EXPECT_EQ(sync.stats().idleCycles, 1u);
    EXPECT_EQ(sync.stats().starts, 0u);
}

TEST(AudioTransportSync, EnteringPlayStartsTheEngineAtTheTransportPositionAndFillsTheLead) {
    AudioTransportSync        sync;
    AudioTransportObservation observation;
    observation.transportPlaying = true;
    observation.transportPosition = ms(5'000);
    observation.engineRunning = false;

    const AudioTransportDecision decision = sync.decide(observation);
    EXPECT_EQ(decision.action, AudioTransportAction::Start);
    EXPECT_EQ(decision.position, ms(5'000));
    // A fresh run has played nothing, so this cycle asks for a full lead's worth
    // rather than waiting for the next tick to begin filling.
    EXPECT_GT(decision.quantaToPump, 0u);
    EXPECT_LE(decision.quantaToPump, sync.options().maxQuantaPerCycle);
    EXPECT_EQ(sync.stats().starts, 1u);
}

TEST(AudioTransportSync, LeavingPlayStopsTheEngineAndAsksForNoAudio) {
    AudioTransportSync        sync;
    AudioTransportObservation observation = playing(ms(2'000), ms(80));
    observation.transportPlaying = false;

    const AudioTransportDecision decision = sync.decide(observation);
    EXPECT_EQ(decision.action, AudioTransportAction::Stop);
    EXPECT_EQ(decision.quantaToPump, 0u) << "a stopping cycle must not also pump";
    EXPECT_EQ(sync.stats().stops, 1u);
}

TEST(AudioTransportSync, AnAlreadyStoppedEngineIsNotStoppedAgain) {
    AudioTransportSync        sync;
    AudioTransportObservation observation;
    observation.transportPlaying = false;
    observation.engineRunning = false;

    EXPECT_TRUE(sync.decide(observation).isIdle());
    EXPECT_EQ(sync.stats().stops, 0u);
}

// ---------------------------------------------------------------------------
// Seeking (Requirement 3A.2)
// ---------------------------------------------------------------------------

TEST(AudioTransportSync, APlayheadJumpWhilePlayingRestartsTheEngineAtTheNewPosition) {
    AudioTransportSync        sync;
    AudioTransportObservation observation = playing(ms(2'000), ms(40));
    observation.transportPosition = ms(30'000);  // the user jumped forward

    const AudioTransportDecision decision = sync.decide(observation);
    EXPECT_EQ(decision.action, AudioTransportAction::Restart);
    EXPECT_EQ(decision.position, ms(30'000));
    EXPECT_GT(decision.quantaToPump, 0u) << "the new run starts with an empty lead";
    EXPECT_EQ(sync.stats().restarts, 1u);
}

TEST(AudioTransportSync, ABackwardJumpAlsoRestarts) {
    AudioTransportSync        sync;
    AudioTransportObservation observation = playing(ms(30'000), ms(40));
    observation.transportPosition = ms(1'000);

    const AudioTransportDecision decision = sync.decide(observation);
    EXPECT_EQ(decision.action, AudioTransportAction::Restart);
    EXPECT_EQ(decision.position, ms(1'000));
}

TEST(AudioTransportSync, HealthyPlaybackIsNeverMistakenForASeek) {
    // The bug this guards: during normal playback the transport's position runs
    // ahead of the played-out audio by up to the target lead. If the seek tolerance
    // were below that, every cycle would look like a seek and the engine would
    // restart continuously — an unbroken stutter.
    AudioTransportSync sync;
    ASSERT_GT(sync.options().seekTolerance, sync.options().targetLead)
        << "the tolerance must exceed the lead, or steady playback restarts forever";

    // A transport a full lead ahead of the audio, which is the healthy case.
    AudioTransportObservation observation = playing(ms(2'000), sync.options().targetLead);
    observation.transportPosition = ms(2'000) + sync.options().targetLead;

    const AudioTransportDecision decision = sync.decide(observation);
    EXPECT_NE(decision.action, AudioTransportAction::Restart);
    EXPECT_EQ(decision.action, AudioTransportAction::None);
    EXPECT_EQ(sync.stats().restarts, 0u);
}

TEST(AudioTransportSync, ManyConsecutiveHealthyCyclesProduceNoRestartsAtAll) {
    // The same guard as a sequence rather than a single observation, because a
    // continuous restart is only obviously wrong when seen repeating.
    AudioTransportSync sync;
    for (int cycle = 0; cycle < 200; ++cycle) {
        AudioTransportObservation observation =
            playing(ms(10 * cycle), sync.options().targetLead);
        observation.transportPosition = ms(10 * cycle) + ms(40);
        (void)sync.decide(observation);
    }
    EXPECT_EQ(sync.stats().restarts, 0u);
    EXPECT_EQ(sync.stats().starts, 0u);
    EXPECT_EQ(sync.stats().stops, 0u);
}

// ---------------------------------------------------------------------------
// Lead keeping (Requirement 3A.3, 3A.4)
// ---------------------------------------------------------------------------

TEST(AudioTransportSync, ACycleThatIsFarEnoughAheadAsksForNothing) {
    // Requirement 3A.4: not a busy loop.
    AudioTransportSync           sync;
    const AudioTransportDecision decision =
        sync.decide(playing(ms(2'000), sync.options().targetLead));

    EXPECT_EQ(decision.quantaToPump, 0u);
    EXPECT_TRUE(decision.isIdle());
    EXPECT_EQ(sync.stats().idleCycles, 1u);
}

TEST(AudioTransportSync, AnEvenLargerLeadStillAsksForNothing) {
    AudioTransportSync sync;
    EXPECT_EQ(sync.decide(playing(ms(2'000), ms(10'000))).quantaToPump, 0u);
}

TEST(AudioTransportSync, AnEmptyLeadAsksForAFullLeadsWorth) {
    AudioTransportSync           sync;
    const AudioTransportDecision decision = sync.decide(playing(ms(2'000), Duration::zero()));

    EXPECT_EQ(decision.action, AudioTransportAction::None) << "no lifecycle change, just audio";
    EXPECT_GT(decision.quantaToPump, 0u);
    // Four quanta of ~21.3 ms cover the 80 ms lead.
    EXPECT_EQ(decision.quantaToPump, 4u);
}

TEST(AudioTransportSync, ASmallShortfallStillAsksForOneQuantumRatherThanNone) {
    // Rounding down would leave a persistent shortfall that is never made up.
    AudioTransportSync sync;
    const Duration     nearlyFull = sync.options().targetLead - ms(1);

    const AudioTransportDecision decision = sync.decide(playing(ms(2'000), nearlyFull));
    EXPECT_EQ(decision.quantaToPump, 1u);
}

TEST(AudioTransportSync, ThePumpRequestIsBoundedPerCycle) {
    // Requirement 3A.3: a decoder having a bad time must cost a bounded amount of
    // UI-thread work, not an unbounded catch-up burst. The default options cannot
    // clamp (a full lead is exactly the cap), so the bound is exercised with a lead
    // deliberately larger than the cap can cover in one cycle.
    AudioTransportSyncOptions options;
    options.targetLead = ms(500);
    options.maxQuantaPerCycle = 2;
    AudioTransportSync sync(options);

    const AudioTransportDecision decision = sync.decide(playing(ms(2'000), Duration::zero()));
    EXPECT_EQ(decision.quantaToPump, 2u);
    EXPECT_EQ(sync.stats().clampedCycles, 1u) << "the clamp must be observable, not silent";
}

TEST(AudioTransportSync, ANegativeLeadIsTreatedAsEmptyRatherThanTrusted) {
    // mixPosition is documented to be at or ahead of presentationPosition. A
    // surprising observation should ask for audio, not silently ask for none.
    AudioTransportSync        sync;
    AudioTransportObservation observation = playing(ms(2'000), Duration::zero());
    observation.mixPosition = ms(1'900);  // behind the playout: should not happen

    const AudioTransportDecision decision = sync.decide(observation);
    EXPECT_GT(decision.quantaToPump, 0u);
}

TEST(AudioTransportSync, ADegenerateQuantumAsksForNothingRatherThanDividingByZero) {
    AudioTransportSyncOptions options;
    options.quantumDuration = Duration::zero();
    AudioTransportSync sync(options);

    const AudioTransportDecision decision = sync.decide(playing(ms(0), Duration::zero()));
    EXPECT_EQ(decision.quantaToPump, 0u);
}

// ---------------------------------------------------------------------------
// Standing off for scrub (Requirement 3A.6)
// ---------------------------------------------------------------------------

TEST(AudioTransportSync, ScrubbingSuppressesEveryDecisionIncludingStartAndStop) {
    // Requirement 3A.6. Two owners issuing start()/stop() at each other would
    // produce exactly the stutter scrubbing exists to avoid, so standing off is not
    // an optimisation.
    AudioTransportSync sync;

    // Would otherwise start.
    AudioTransportObservation wouldStart;
    wouldStart.transportPlaying = true;
    wouldStart.transportPosition = ms(1'000);
    wouldStart.engineRunning = false;
    wouldStart.scrubbing = true;
    EXPECT_TRUE(sync.decide(wouldStart).isIdle());

    // Would otherwise stop.
    AudioTransportObservation wouldStop = playing(ms(2'000), ms(80));
    wouldStop.transportPlaying = false;
    wouldStop.scrubbing = true;
    EXPECT_TRUE(sync.decide(wouldStop).isIdle());

    // Would otherwise pump.
    AudioTransportObservation wouldPump = playing(ms(2'000), Duration::zero());
    wouldPump.scrubbing = true;
    EXPECT_TRUE(sync.decide(wouldPump).isIdle());

    EXPECT_EQ(sync.stats().deferredToScrub, 3u);
    EXPECT_EQ(sync.stats().starts, 0u);
    EXPECT_EQ(sync.stats().stops, 0u);
    EXPECT_EQ(sync.stats().quantaRequested, 0u);
}

TEST(AudioTransportSync, ReleasingAScrubRestoresNormalControlOnTheNextCycle) {
    AudioTransportSync        sync;
    AudioTransportObservation observation = playing(ms(2'000), Duration::zero());

    observation.scrubbing = true;
    ASSERT_TRUE(sync.decide(observation).isIdle());

    // The drag ended. Scrub left the engine somewhere else, so the transport's
    // position no longer matches what was played out — which reads as a seek and
    // restarts, putting playback back where the user left the playhead.
    observation.scrubbing = false;
    observation.transportPosition = ms(45'000);
    const AudioTransportDecision resumed = sync.decide(observation);
    EXPECT_EQ(resumed.action, AudioTransportAction::Restart);
    EXPECT_EQ(resumed.position, ms(45'000));
}

// ---------------------------------------------------------------------------
// The missing-device case (Requirement 3A.5) and the option invariants
// ---------------------------------------------------------------------------

TEST(AudioTransportSync, TheDecisionDoesNotDependOnWhetherADeviceExists) {
    // Requirement 3A.5: there is deliberately no "device available" input here. The
    // engine's own null-sink path suppresses audio and still runs the clock, so the
    // wiring must start it regardless — refusing to start would stop the transport
    // working on a host with no sound card.
    AudioTransportSync        sync;
    AudioTransportObservation observation;
    observation.transportPlaying = true;
    observation.transportPosition = ms(0);
    observation.engineRunning = false;

    EXPECT_EQ(sync.decide(observation).action, AudioTransportAction::Start);
}

TEST(AudioTransportSync, TheDefaultOptionsAreConsistentWithTheEnginesOwnGeometry) {
    const AudioTransportSyncOptions options;

    // One quantum is 1024 frames at 48 kHz.
    EXPECT_NEAR(options.quantumDuration.seconds(), 1024.0 / 48'000.0, 1.0e-6);
    // The lead sits under the engine's 100 ms dropout bound, so a fully drained
    // lead is still reported as a dropout rather than hidden by an over-generous
    // buffer.
    EXPECT_LT(options.targetLead, Duration::fromMilliseconds(100));
    // And the cap can refill the whole lead in one cycle.
    EXPECT_GE(static_cast<double>(options.maxQuantaPerCycle) * options.quantumDuration.seconds(),
              options.targetLead.seconds());
}

TEST(AudioTransportSync, AWholePlaybackSequenceStartsPumpsAndStopsExactlyOnce) {
    AudioTransportSync sync;

    AudioTransportObservation observation;
    observation.transportPlaying = true;
    observation.transportPosition = ms(0);
    observation.engineRunning = false;
    ASSERT_EQ(sync.decide(observation).action, AudioTransportAction::Start);

    // Now running, and steadily topped up.
    observation.engineRunning = true;
    for (int cycle = 1; cycle <= 50; ++cycle) {
        const Duration playedOut = ms(10 * cycle);
        observation.presentationPosition = playedOut;
        observation.transportPosition = playedOut;
        // A lead that drains by 10 ms each cycle and is refilled by the decision.
        observation.mixPosition = playedOut + ms(60);
        (void)sync.decide(observation);
    }

    observation.transportPlaying = false;
    ASSERT_EQ(sync.decide(observation).action, AudioTransportAction::Stop);

    const AudioTransportSyncStats stats = sync.stats();
    EXPECT_EQ(stats.starts, 1u);
    EXPECT_EQ(stats.stops, 1u);
    EXPECT_EQ(stats.restarts, 0u);
    EXPECT_GT(stats.quantaRequested, 0u);
}

} // namespace
} // namespace palmier::ui
