// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/AudioPlaybackDriver.cpp — implementation of the audio playback timer.

#include "ui/AudioPlaybackDriver.hpp"

#ifdef PALMIER_HAVE_QT

#include <utility>

#include <QTimer>

#include "media/AudioEngine.hpp"

namespace palmier::ui {

AudioPlaybackDriver::AudioPlaybackDriver(media::AudioEngine& engine, QObject* parent)
    : QObject(parent), engine_(engine) {
    timer_ = new QTimer(this);
    timer_->setInterval(kCycleIntervalMs);
    connect(timer_, &QTimer::timeout, this, [this] { (void)tick(); });
    timer_->start();
}

AudioPlaybackDriver::~AudioPlaybackDriver() {
    // Stop the engine before this driver disappears, so a run does not outlive the
    // only object that would ever pump it again.
    if (engine_.running()) engine_.stop();
}

void AudioPlaybackDriver::setProviders(PlayingProvider playing, PositionProvider position,
                                       ScrubbingProvider scrubbing) {
    playing_ = std::move(playing);
    position_ = std::move(position);
    scrubbing_ = std::move(scrubbing);
}

bool AudioPlaybackDriver::isRunning() const noexcept {
    return timer_ != nullptr && timer_->isActive();
}

void AudioPlaybackDriver::suspend() {
    if (timer_ != nullptr) timer_->stop();
    if (engine_.running()) engine_.stop();
}

void AudioPlaybackDriver::resume() {
    if (timer_ != nullptr && !timer_->isActive()) timer_->start();
}

std::size_t AudioPlaybackDriver::tick() {
    AudioTransportObservation observation;
    // Absent providers read as a stopped transport, which makes an unwired driver
    // inert rather than wrong.
    observation.transportPlaying = playing_ ? playing_() : false;
    observation.transportPosition = position_ ? position_() : Duration::zero();
    observation.scrubbing = scrubbing_ ? scrubbing_() : false;
    observation.engineRunning = engine_.running();
    observation.mixPosition = engine_.mixPosition();
    observation.presentationPosition = engine_.presentationPosition();

    const AudioTransportDecision decision = sync_.decide(observation);

    switch (decision.action) {
        case AudioTransportAction::Stop:
            engine_.stop();
            return 0;
        case AudioTransportAction::Restart:
            engine_.stop();
            [[fallthrough]];
        case AudioTransportAction::Start: {
            // Requirement 3A.5: a sink that cannot be opened is NOT an error — the
            // engine installs a null sink, keeps its clock running and raises a
            // notice. So a failed start is genuinely exceptional (an invalid
            // position, or already running) and there is nothing to pump.
            if (engine_.start(decision.position).isError()) {
                ++pumpFailures_;
                return 0;
            }
            break;
        }
        case AudioTransportAction::None:
            break;
    }

    if (!engine_.running()) return 0;

    std::size_t pumped = 0;
    for (std::size_t quantum = 0; quantum < decision.quantaToPump; ++quantum) {
        if (engine_.pump().isError()) {
            // Counted, not surfaced: the engine reports decode failures through
            // errors() and silences the affected clip, and there is no user action
            // to offer from inside a timer tick. Stop pumping this cycle rather
            // than hammering a failing sink.
            ++pumpFailures_;
            break;
        }
        ++pumped;
    }
    return pumped;
}

} // namespace palmier::ui

#endif // PALMIER_HAVE_QT
