// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/AudioTransportSync.hpp — Qt-free audio playback lifecycle decisions
// (monitoring-and-grading Requirement 3A).
//
// The Audio_Engine is complete and thoroughly tested, and until now it was never
// run: nothing in production ever called start() or pump(), so `running()` was
// permanently false, the master clock was never consulted, and no audio was
// audible. This is the component that decides when to run it.
//
// ## What it decides, and why that is all it does
//
// Given an observation of the transport and the engine, it returns an intent —
// start, stop, restart, or "pump N quanta" — which the caller applies. It holds no
// engine, opens no device, owns no timer and reads no clock. Requirement 3A.7 asks
// for exactly this, for the same reason Requirement 3 asks it of
// ScrubAudioController: a lifecycle expressed as a pure function of an observation
// can be tested exhaustively with no audio device, no timer and no real engine,
// including the transitions that are awkward to provoke in a live system (a seek
// during playback, a device disappearing, a scrub starting mid-play).
//
// ## Pumping is a lead-keeping problem, not a rate
//
// The engine mixes and submits one QUANTUM per pump() — 1024 frames at 48 kHz,
// about 21.3 ms. A caller that pumped once per timer tick would couple audio
// continuity to timer jitter: a late tick starves the sink. So instead of a rate,
// this computes the LEAD the engine still has (mix cursor minus played-out
// position) and asks for however many quanta restore the target lead. Two
// properties follow, and both are Requirement 3A.4 and 3A.3 respectively:
//
//   * a cycle that finds the engine already far enough ahead asks for ZERO quanta,
//     so this is never a busy loop;
//   * the request is capped per cycle, so a decoder having a bad time costs a
//     bounded amount of UI-thread work rather than an unbounded catch-up burst.

#ifndef PALMIER_UI_AUDIOTRANSPORTSYNC_HPP
#define PALMIER_UI_AUDIOTRANSPORTSYNC_HPP

#include <cstddef>
#include <cstdint>

#include "core/Duration.hpp"

namespace palmier::ui {

/// What the caller should do to the Audio_Engine before pumping it.
enum class AudioTransportAction {
    /// Leave the engine as it is (it may still need pumping — see `quantaToPump`).
    None,
    /// Start the engine at the decision's position.
    Start,
    /// Stop the engine.
    Stop,
    /// Stop and start again at the decision's position, because the playhead moved
    /// somewhere the current run cannot reach by playing on.
    Restart,
};

/// One cycle's decision. `action` and `quantaToPump` are independent: an ordinary
/// steady-state cycle is `None` plus a positive pump count, and a `Stop` is always
/// accompanied by zero.
struct AudioTransportDecision {
    AudioTransportAction action = AudioTransportAction::None;
    Duration             position{};
    std::size_t          quantaToPump = 0;

    [[nodiscard]] bool isIdle() const noexcept {
        return action == AudioTransportAction::None && quantaToPump == 0;
    }
};

/// Everything the decision depends on, gathered by the caller in one place so the
/// decision cannot read anything else — and so a test can state a situation
/// exactly rather than construct one.
struct AudioTransportObservation {
    /// The transport's own state and position (from ui::PreviewController).
    bool     transportPlaying = false;
    Duration transportPosition{};
    /// The engine's state (from media::AudioEngine).
    bool     engineRunning = false;
    /// The next position to be mixed. Ahead of, or equal to, `presentation`.
    Duration mixPosition{};
    /// The position actually played out — the engine's master clock.
    Duration presentationPosition{};
    /// True while a scrub gesture owns the engine (Requirement 3). The playback
    /// wiring must then stand off completely rather than fight it for the engine.
    bool     scrubbing = false;
};

/// Tunables, all with defaults derived from the engine's real geometry.
struct AudioTransportSyncOptions {
    /// One pump()'s worth of audio: kDefaultQuantumFrames (1024) at
    /// kOutputSampleRate (48 kHz) is 21⅓ ms. Named rather than hard-coded at the
    /// call site so a project-rate-derived quantum can be substituted.
    Duration quantumDuration = Duration::fromNanoseconds(1024LL * Duration::kTicksPerSecond / 48'000);

    /// How far ahead of the played-out position the mix cursor should be kept.
    ///
    /// Must exceed the driving timer's interval by a comfortable margin, because
    /// this lead is the entire budget for a late tick. 80 ms is under the engine's
    /// own 100 ms dropout bound — so a lead that has fully drained is still
    /// reported as a dropout rather than being hidden by an over-generous buffer —
    /// and is roughly four quanta.
    Duration targetLead = Duration::fromMilliseconds(80);

    /// Maximum quanta one cycle may ask for (Requirement 3A.3). Four is enough to
    /// refill the whole lead from empty in a single cycle, and bounds the work so a
    /// slow decoder cannot stall the UI thread with an unbounded catch-up burst.
    std::size_t maxQuantaPerCycle = 4;

    /// How far the transport's position may differ from what has been played out
    /// before it counts as a SEEK rather than as ordinary drift.
    ///
    /// This must be larger than the target lead, because during healthy playback
    /// the transport's position legitimately runs ahead of the played-out audio by
    /// up to that lead. A tolerance below it would diagnose a seek on every cycle
    /// and restart the engine continuously — which is audible as a stutter and is
    /// the reason this is a named, separately-tested constant.
    Duration seekTolerance = Duration::fromMilliseconds(250);
};

/// Monotonic counters over the object's lifetime.
struct AudioTransportSyncStats {
    std::uint64_t starts{0};
    std::uint64_t stops{0};
    std::uint64_t restarts{0};
    std::uint64_t quantaRequested{0};
    /// Cycles that asked for nothing at all (Requirement 3A.4's "no busy loop").
    std::uint64_t idleCycles{0};
    /// Cycles skipped because a scrub gesture owned the engine (Requirement 3A.6).
    std::uint64_t deferredToScrub{0};
    /// Cycles whose pump request was clamped by maxQuantaPerCycle.
    std::uint64_t clampedCycles{0};
};

class AudioTransportSync {
public:
    using Options = AudioTransportSyncOptions;
    using Stats   = AudioTransportSyncStats;

    AudioTransportSync() = default;
    explicit AudioTransportSync(Options options) : options_(options) {}

    /// Decide what this cycle should do. Pure with respect to the engine: it reads
    /// nothing and changes nothing, so a caller may discard the decision.
    [[nodiscard]] AudioTransportDecision decide(const AudioTransportObservation& observation);

    [[nodiscard]] const Options& options() const noexcept { return options_; }
    [[nodiscard]] Stats stats() const noexcept { return stats_; }

private:
    /// Quanta needed to restore the target lead, clamped to the per-cycle cap.
    /// Counts a clamp when it bites, so the cap's effect is observable.
    [[nodiscard]] std::size_t quantaFor(Duration lead);

    Options options_{};
    Stats   stats_{};
};

} // namespace palmier::ui

#endif // PALMIER_UI_AUDIOTRANSPORTSYNC_HPP
