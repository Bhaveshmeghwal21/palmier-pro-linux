// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/AudioMeterViewModel.hpp — Qt-free programme level meter logic.
//
// monitoring-and-grading Requirement 1.4-1.7. This is the meter's BEHAVIOUR —
// peak-hold decay, the at-or-above-full-scale latch, and falling to zero when
// the transport stops — separated from any painting, exactly as
// ui::PreviewController is separated from ui::PreviewView and
// ui::MediaBrowserViewModel from ui::MediaBrowserPanel.
//
// The separation is what makes the two timing rules testable at all. Both are
// stated in real time:
//
//   * a full-scale peak must stay indicated for at least 1 second
//     (Requirement 1.5), so a single over-scale sample cannot slip between two
//     repaints;
//   * the peak-hold must decay no faster than 20 dB per second
//     (Requirement 1.6).
//
// Time therefore arrives as an argument rather than being read from a clock
// inside update(), so a test can drive a hundred simulated seconds without
// sleeping and without a display.

#ifndef PALMIER_UI_AUDIOMETERVIEWMODEL_HPP
#define PALMIER_UI_AUDIOMETERVIEWMODEL_HPP

#include <chrono>
#include <cstddef>
#include <optional>
#include <vector>

#include "media/AudioGraph.hpp"

namespace palmier::ui {

/// What a meter draws for one channel. All three levels are normalised, 1.0 being
/// full scale.
struct MeterChannelState {
    /// The most recent quantum's peak for this channel.
    float peak = 0.0f;
    /// The most recent quantum's RMS for this channel.
    float rms = 0.0f;
    /// The decaying peak-hold: the highest recent peak, falling at no more than
    /// kHoldDecayDbPerSecond (Requirement 1.6).
    float hold = 0.0f;
    /// True while this channel's at-or-above-full-scale indication is latched
    /// (Requirement 1.5).
    bool clipped = false;
};

/// The meter's timing rules, named so the tests assert against the same numbers
/// the implementation uses rather than against transcribed copies.
///
/// Requirement 1.6 is an upper bound ("no faster than 20 dB per second"), so
/// decaying at exactly 20 dB/s satisfies it. 20 dB is a factor of ten, which is
/// why one second of decay multiplies the hold by 0.1.
inline constexpr double kHoldDecayDbPerSecond = 20.0;

/// Requirement 1.5 is a lower bound ("at least 1 second"), so the latch is held
/// for exactly one second from the last full-scale peak.
inline constexpr std::chrono::milliseconds kClipLatchDuration{1000};

/// Peak-hold decay and clip latching over a sequence of measured quanta.
///
/// Not an owner of anything and not a source of time: update() is told both the
/// levels and the instant they belong to. Two updates with the same instant
/// therefore decay nothing, which keeps a repaint-driven caller from decaying the
/// hold faster than real time merely by repainting more often.
class AudioMeterViewModel {
public:
    using TimePoint = std::chrono::steady_clock::time_point;

    AudioMeterViewModel() = default;

    /// Fold one measured quantum in at `now`.
    ///
    /// `playing` false means the transport is stopped, in which case the incoming
    /// levels are treated as silence regardless of what was measured: a stopped
    /// transport must fall to zero rather than freeze at its last reading
    /// (Requirement 1.7). The hold still decays from wherever it was, so the
    /// fall is visible rather than instantaneous.
    void update(const media::AudioLevels& levels, bool playing, TimePoint now);

    /// One entry per channel seen in the most recent update.
    [[nodiscard]] const std::vector<MeterChannelState>& channels() const noexcept {
        return channels_;
    }

    [[nodiscard]] std::size_t channelCount() const noexcept { return channels_.size(); }

    /// True while ANY channel's clip indication is latched — what a single
    /// summary indicator lights on.
    [[nodiscard]] bool anyClipped() const noexcept;

    /// Every level back to zero and every latch cleared, as on a project switch.
    void reset() noexcept;

private:
    std::vector<MeterChannelState> channels_{};
    /// The instant of the previous update, absent before the first one. The gap
    /// between it and `now` is the only thing the decay depends on.
    std::optional<TimePoint>       lastUpdate_{};
    /// Per channel, when the current clip latch expires. Absent when unlatched.
    std::vector<std::optional<TimePoint>> clipUntil_{};
};

} // namespace palmier::ui

#endif // PALMIER_UI_AUDIOMETERVIEWMODEL_HPP
