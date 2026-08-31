// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/AudioMeterWidget.cpp — implementation of the Qt 6 level meter surface.

#include "ui/AudioMeterWidget.hpp"

#ifdef PALMIER_HAVE_QT

#include <algorithm>
#include <chrono>

#include <QColor>
#include <QPainter>
#include <QRect>
#include <QString>
#include <QTimer>

namespace palmier::ui {
namespace {

/// Bar geometry, in device-independent pixels.
constexpr int kChannelBarHeight = 6;
constexpr int kChannelSpacing = 2;
constexpr int kPreferredWidth = 120;

/// The RMS fill, the peak-hold tick, and the latched clip indication. Deliberately
/// three distinct colours rather than three shades of one, so Requirement 1.5's
/// "distinctly from any level below it" holds without relying on brightness
/// discrimination.
const QColor kTrackColour{0x20, 0x20, 0x20};
const QColor kRmsColour{0x3d, 0xa5, 0x4a};
const QColor kHoldColour{0xe8, 0xe8, 0xe8};
const QColor kClipColour{0xc0, 0x39, 0x2b};

[[nodiscard]] int scaleToWidth(float level, int width) noexcept {
    const float clamped = std::clamp(level, 0.0f, 1.0f);
    return static_cast<int>(clamped * static_cast<float>(width));
}

} // namespace

AudioMeterWidget::AudioMeterWidget(QWidget* parent) : QWidget(parent) {
    setToolTip(QStringLiteral("Programme output level (peak and RMS)"));
    setMinimumWidth(kPreferredWidth);

    timer_ = new QTimer(this);
    timer_->setInterval(kSampleIntervalMs);
    connect(timer_, &QTimer::timeout, this, &AudioMeterWidget::sample);
    timer_->start();
}

AudioMeterWidget::~AudioMeterWidget() = default;

void AudioMeterWidget::setProviders(LevelsProvider levels, PlayingProvider playing) {
    levelsProvider_ = std::move(levels);
    playingProvider_ = std::move(playing);
}

void AudioMeterWidget::sample() {
    // Absent either seam the meter reads as stopped silence rather than holding a
    // stale reading, which is the same rule Requirement 1.7 applies to a stopped
    // transport.
    const media::AudioLevels levels =
        levelsProvider_ ? levelsProvider_() : media::AudioLevels{};
    const bool playing = playingProvider_ ? playingProvider_() : false;
    sampleAt(levels, playing, std::chrono::steady_clock::now());
}

void AudioMeterWidget::sampleAt(const media::AudioLevels& levels, bool playing,
                                AudioMeterViewModel::TimePoint now) {
    model_.update(levels, playing, now);
    update();  // request a repaint; Qt coalesces these for us
}

void AudioMeterWidget::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    const auto& channels = model_.channels();
    if (channels.empty()) {
        return;  // no channels measured yet: draw nothing rather than an empty scale
    }

    const int barWidth = std::max(1, width());
    int y = 0;
    for (const MeterChannelState& state : channels) {
        const QRect track{0, y, barWidth, kChannelBarHeight};
        painter.fillRect(track, kTrackColour);

        // RMS body: what the channel actually sounds like in level terms.
        const int rmsWidth = scaleToWidth(state.rms, barWidth);
        if (rmsWidth > 0) {
            painter.fillRect(QRect{0, y, rmsWidth, kChannelBarHeight}, kRmsColour);
        }

        // Peak-hold tick: a 2 px marker at the decaying hold position, so a
        // transient that has already passed is still visible.
        const int holdX = scaleToWidth(state.hold, barWidth);
        if (holdX > 0) {
            painter.fillRect(QRect{std::min(holdX, barWidth - 2), y, 2, kChannelBarHeight},
                             kHoldColour);
        }

        // Latched clip indication: the rightmost 4 px, held for at least a second
        // after the peak that caused it (Requirement 1.5).
        if (state.clipped) {
            painter.fillRect(QRect{barWidth - 4, y, 4, kChannelBarHeight}, kClipColour);
        }

        y += kChannelBarHeight + kChannelSpacing;
    }
}

} // namespace palmier::ui

#endif // PALMIER_HAVE_QT
