// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/AudioPlaybackDriver.hpp — the timer that finally runs the Audio_Engine
// (monitoring-and-grading Requirement 3A.3, 3A.4).
//
// Everything decided in ui::AudioTransportSync has to be applied by somebody on a
// cadence. This is that somebody, and it is deliberately the thinnest possible
// object: a QTimer, an observation gathered from the transport and the engine, and
// a loop that applies the intent. No policy lives here — move a rule into this
// file and it stops being testable without a Qt event loop and an audio device.
//
// ## Why a GUI-thread timer is the right shape here, not a compromise
//
// The Audio_Engine documents single-thread affinity for start / pump / stop and
// spawns no threads of its own, and the sink it submits to does its own buffering.
// The existing video path already works this way — PreviewView drives
// PreviewController::pump() from a QTimer — and the engine carries dropout
// accounting (a 100 ms bound, reported through maxDeliveryGap()) precisely so that
// a host too busy to pump on time is *measured* rather than silently glitching.
// Pumping from the GUI thread therefore matches the architecture rather than
// fighting it, and it puts the pump on the same thread as the level meter that
// reads the engine's per-quantum report, which is what makes that read safe.
//
// The cadence is faster than one quantum (21⅓ ms) so that the lead is topped up
// before it drains, and the per-cycle quantum cap in AudioTransportSync is what
// bounds the work when a decoder falls behind.
//
// ## Providers, not references
//
// The transport state, the scrub state and the engine all arrive through
// std::function seams, exactly as AudioMeterWidget takes its levels. This driver
// therefore depends on neither app::ApplicationComposition nor
// ui::PreviewController, and a test can step it with `tick()` against synthesised
// state and no audio device at all.

#ifndef PALMIER_UI_AUDIOPLAYBACKDRIVER_HPP
#define PALMIER_UI_AUDIOPLAYBACKDRIVER_HPP

#ifdef PALMIER_HAVE_QT

#include <cstddef>
#include <cstdint>
#include <functional>

#include <QObject>

#include "core/Duration.hpp"
#include "ui/AudioTransportSync.hpp"

class QTimer;

namespace palmier::media {
class AudioEngine;
}

namespace palmier::ui {

/// Drives one media::AudioEngine from the transport's state.
class AudioPlaybackDriver : public QObject {
    Q_OBJECT

public:
    /// Supplies whether the transport is playing and where its playhead is.
    using PlayingProvider  = std::function<bool()>;
    using PositionProvider = std::function<Duration()>;
    /// Supplies whether a scrub gesture currently owns the engine, so this driver
    /// stands off (Requirement 3A.6). Absent, it is treated as never scrubbing.
    using ScrubbingProvider = std::function<bool()>;

    /// The polling cadence. 10 ms is comfortably under one quantum (21⅓ ms), so the
    /// lead is topped up before it can drain even if a tick is late.
    static constexpr int kCycleIntervalMs = 10;

    /// `engine` must outlive this driver.
    explicit AudioPlaybackDriver(media::AudioEngine& engine, QObject* parent = nullptr);
    ~AudioPlaybackDriver() override;

    void setProviders(PlayingProvider playing, PositionProvider position,
                      ScrubbingProvider scrubbing);

    /// Run one cycle: observe, decide, apply. Called by this driver's own timer;
    /// exposed so a test can step it deterministically instead of running an event
    /// loop. Returns the quanta actually pumped.
    std::size_t tick();

    /// Stop the engine and the timer — for shutdown, or when the shell is handing
    /// the engine to something else. Idempotent.
    void suspend();
    /// Resume polling after suspend(). Idempotent.
    void resume();
    [[nodiscard]] bool isRunning() const noexcept;

    [[nodiscard]] const AudioTransportSync& sync() const noexcept { return sync_; }

    /// Pump failures are counted rather than surfaced: a mix or sink failure during
    /// playback is the engine's own business (it reports through errors() and
    /// silences the affected clip), and there is no user action to offer mid-tick.
    [[nodiscard]] std::uint64_t pumpFailures() const noexcept { return pumpFailures_; }

private:
    media::AudioEngine& engine_;
    AudioTransportSync  sync_{};
    PlayingProvider     playing_{};
    PositionProvider    position_{};
    ScrubbingProvider   scrubbing_{};
    QTimer*             timer_ = nullptr;
    std::uint64_t       pumpFailures_ = 0;
};

} // namespace palmier::ui

#endif // PALMIER_HAVE_QT

#endif // PALMIER_UI_AUDIOPLAYBACKDRIVER_HPP
