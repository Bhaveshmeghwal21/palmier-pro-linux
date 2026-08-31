// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/AudioMeterViewModel.cpp — implementation of the Qt-free meter logic.

#include "ui/AudioMeterViewModel.hpp"

#include <algorithm>
#include <cmath>

namespace palmier::ui {
namespace {

/// The factor a hold is multiplied by after `seconds` of decay at
/// kHoldDecayDbPerSecond. A gain in decibels is 20*log10(ratio), so the inverse
/// of a -D dB change is 10^(-D/20); over `seconds` at D dB per second that is
/// 10^(-(D*seconds)/20).
[[nodiscard]] float decayFactor(double seconds) noexcept {
    if (seconds <= 0.0) return 1.0f;
    const double decibels = kHoldDecayDbPerSecond * seconds;
    return static_cast<float>(std::pow(10.0, -decibels / 20.0));
}

/// A level below this reads as zero rather than as an ever-smaller number, so a
/// decaying hold actually reaches zero instead of approaching it forever. At
/// -100 dBFS this is far below anything representable in 16-bit audio and well
/// below what any meter can draw.
constexpr float kSilenceFloor = 1.0e-5f;

} // namespace

void AudioMeterViewModel::update(const media::AudioLevels& levels, bool playing, TimePoint now) {
    const std::size_t channelCount = levels.peak.size();

    // Resize on a channel-count change (a project switch can change it) without
    // carrying a stale latch or hold into a channel that now means something
    // different.
    if (channels_.size() != channelCount) {
        channels_.assign(channelCount, MeterChannelState{});
        clipUntil_.assign(channelCount, std::nullopt);
    }

    // Requirement 1.6: the decay depends only on elapsed real time, so a caller
    // that repaints twice as often does not decay twice as fast. Before the first
    // update there is no elapsed time and therefore no decay.
    const float decay = lastUpdate_.has_value()
                            ? decayFactor(std::chrono::duration<double>(now - *lastUpdate_).count())
                            : 1.0f;
    lastUpdate_ = now;

    for (std::size_t channel = 0; channel < channelCount; ++channel) {
        // Requirement 1.7: a stopped transport reads as silence whatever the last
        // measured quantum held, so the meter falls rather than freezing.
        const float peak = playing ? levels.peak[channel] : 0.0f;
        const float rms =
            playing && channel < levels.rms.size() ? levels.rms[channel] : 0.0f;

        MeterChannelState& state = channels_[channel];
        state.peak = peak;
        state.rms = rms;

        // The hold decays from its previous value and is then raised — never
        // lowered — by the incoming peak.
        float hold = state.hold * decay;
        if (hold < kSilenceFloor) hold = 0.0f;
        state.hold = std::max(hold, peak);

        // Requirement 1.5: latch at or above full scale and hold the latch for at
        // least kClipLatchDuration from the most recent such peak. A later
        // full-scale peak extends the latch rather than restarting a shorter one.
        if (peak >= 1.0f) {
            clipUntil_[channel] = now + kClipLatchDuration;
        }
        const std::optional<TimePoint>& until = clipUntil_[channel];
        if (until.has_value() && now >= *until) {
            clipUntil_[channel] = std::nullopt;
        }
        state.clipped = clipUntil_[channel].has_value();
    }
}

bool AudioMeterViewModel::anyClipped() const noexcept {
    return std::any_of(channels_.begin(), channels_.end(),
                       [](const MeterChannelState& state) { return state.clipped; });
}

void AudioMeterViewModel::reset() noexcept {
    channels_.clear();
    clipUntil_.clear();
    lastUpdate_.reset();
}

} // namespace palmier::ui
