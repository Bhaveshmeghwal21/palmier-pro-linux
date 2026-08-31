// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/AudioMeterWidget.hpp — the thin Qt 6 programme level meter surface
// (monitoring-and-grading task 1; Requirement 1.4-1.7).
//
// Deliberately minimal, exactly as ui::PreviewView is minimal over
// ui::PreviewController: every timing rule the meter obeys — the >= 1 second
// clip indication and the <= 20 dB/s peak-hold decay — lives in the Qt-free
// ui::AudioMeterViewModel this widget owns and drives. The widget only (1) runs
// a QTimer, (2) asks its injected providers for the current levels and whether
// the transport is playing, and (3) paints the resulting per-channel state.
//
// The providers are std::function seams rather than a reference to the
// composition root, so this widget depends on neither app::ApplicationComposition
// nor media::AudioEngine: the shell supplies two closures and this file stays a
// pure presentation surface. That also lets a test drive it with synthesised
// levels and no audio device at all.
//
// The whole translation unit is behind PALMIER_HAVE_QT, matching every other Qt
// panel in this directory, so the tree still configures where Qt 6 is absent.

#ifndef PALMIER_UI_AUDIOMETERWIDGET_HPP
#define PALMIER_UI_AUDIOMETERWIDGET_HPP

#ifdef PALMIER_HAVE_QT

#include <functional>

#include <QWidget>

#include "media/AudioGraph.hpp"
#include "ui/AudioMeterViewModel.hpp"

class QTimer;
class QPaintEvent;

namespace palmier::ui {

/// A compact per-channel peak/RMS meter for the programme output.
class AudioMeterWidget : public QWidget {
    Q_OBJECT

public:
    /// Supplies the most recently measured programme levels.
    using LevelsProvider = std::function<media::AudioLevels()>;
    /// Supplies whether the transport is currently playing, which decides
    /// whether the meter falls to zero (Requirement 1.7).
    using PlayingProvider = std::function<bool()>;

    explicit AudioMeterWidget(QWidget* parent = nullptr);
    ~AudioMeterWidget() override;

    /// Install the two data seams. Either may be left unset, in which case the
    /// meter reads as stopped silence rather than misreporting.
    void setProviders(LevelsProvider levels, PlayingProvider playing);

    /// Fold one sample of the providers' current values in at the current
    /// instant. Called by this widget's own timer; exposed so a test can step it
    /// deterministically instead of waiting for the timer.
    void sample();

    /// Fold in an explicit reading at an explicit instant, bypassing the
    /// providers. This is the seam the timing tests drive: it lets simulated time
    /// advance by seconds per call without sleeping.
    void sampleAt(const media::AudioLevels& levels, bool playing,
                  AudioMeterViewModel::TimePoint now);

    [[nodiscard]] const AudioMeterViewModel& viewModel() const noexcept { return model_; }

    /// The meter's polling interval. 50 ms is 20 Hz, comfortably above
    /// Requirement 1.4's "at least 10 times per second" floor.
    static constexpr int kSampleIntervalMs = 50;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    AudioMeterViewModel model_{};
    LevelsProvider      levelsProvider_{};
    PlayingProvider     playingProvider_{};
    QTimer*             timer_ = nullptr;
};

} // namespace palmier::ui

#endif // PALMIER_HAVE_QT

#endif // PALMIER_UI_AUDIOMETERWIDGET_HPP
