// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/TimelinePanel.cpp — implementation of the timeline dock panel.
//
// Compiled only when Qt 6 is available (PALMIER_HAVE_QT). See TimelinePanel.hpp
// for the contract: this file only lays out widgets and forwards their signals
// to TimelineModel / PreviewController.

#include "ui/TimelinePanel.hpp"

#ifdef PALMIER_HAVE_QT

#include <algorithm>
#include <cstdint>

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QSlider>
#include <QString>
#include <QStringList>
#include <QToolButton>
#include <QVBoxLayout>

#include "core/Duration.hpp"
#include "core/FrameRate.hpp"
#include "core/Uuid.hpp"
#include "ui/TimelineGraphView.hpp"

namespace palmier::ui {

TimelinePanel::TimelinePanel(TimelineEngine& engine, PreviewController& transport,
                             GuiToolGateway* gateway, QWidget* parent)
    : QWidget(parent), model_(engine, this, gateway), transport_(transport) {
    buildLayout();

    connect(&model_, &TimelineModel::modelRefreshed, this,
            &TimelinePanel::refreshTransportState);
    connect(&model_, &TimelineModel::modelRefreshed, this,
            &TimelinePanel::onModelRefreshed);
    connect(graph_, &TimelineGraphView::clipSelected, this, &TimelinePanel::clipSelected);
    connect(graph_, &TimelineGraphView::selectionCleared, this, &TimelinePanel::selectionCleared);
    connect(graph_, &TimelineGraphView::placementTrackChanged, this,
            &TimelinePanel::onGraphSelectionChanged);
    connect(graph_, &TimelineGraphView::seekRequested, this,
            &TimelinePanel::onGraphSeekRequested);

    refreshTransportState();
}

TimelinePanel::~TimelinePanel() = default;

void TimelinePanel::buildLayout() {
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(4, 4, 4, 4);

    auto* transportBar = new QWidget(this);
    auto* transportLayout = new QHBoxLayout(transportBar);
    transportLayout->setContentsMargins(0, 0, 0, 0);

    playButton_ = new QToolButton(transportBar);
    playButton_->setText(QStringLiteral("Play"));
    connect(playButton_, &QToolButton::clicked, this, &TimelinePanel::onPlayClicked);

    pauseButton_ = new QToolButton(transportBar);
    pauseButton_->setText(QStringLiteral("Pause"));
    connect(pauseButton_, &QToolButton::clicked, this, &TimelinePanel::onPauseClicked);

    stopButton_ = new QToolButton(transportBar);
    stopButton_->setText(QStringLiteral("Stop"));
    connect(stopButton_, &QToolButton::clicked, this, &TimelinePanel::onStopClicked);

    undoButton_ = new QToolButton(transportBar);
    undoButton_->setText(QStringLiteral("Undo"));
    connect(undoButton_, &QToolButton::clicked, this, &TimelinePanel::onUndoClicked);

    redoButton_ = new QToolButton(transportBar);
    redoButton_->setText(QStringLiteral("Redo"));
    connect(redoButton_, &QToolButton::clicked, this, &TimelinePanel::onRedoClicked);

    playheadLabel_ = new QLabel(QStringLiteral("00:00:00.000"), transportBar);

    transportLayout->addWidget(playButton_);
    transportLayout->addWidget(pauseButton_);
    transportLayout->addWidget(stopButton_);
    transportLayout->addSpacing(12);
    transportLayout->addWidget(undoButton_);
    transportLayout->addWidget(redoButton_);
    transportLayout->addStretch(1);
    transportLayout->addWidget(playheadLabel_);

    // Playhead bar (usable-editor Requirement 4): a scrub slider spanning the
    // timeline, frame-step buttons, and an editable timecode field. All four
    // gestures funnel through movePlayheadToMs() so every one snaps to the
    // nearest frame and clamps to [0, timeline duration] identically.
    auto* playheadBar = new QWidget(this);
    auto* playheadLayout = new QHBoxLayout(playheadBar);
    playheadLayout->setContentsMargins(0, 0, 0, 0);

    stepBackButton_ = new QToolButton(playheadBar);
    stepBackButton_->setText(QStringLiteral("|◄"));
    stepBackButton_->setToolTip(QStringLiteral("Step back one frame"));
    connect(stepBackButton_, &QToolButton::clicked, this, &TimelinePanel::onStepBackClicked);

    scrubSlider_ = new QSlider(Qt::Horizontal, playheadBar);
    scrubSlider_->setMinimum(0);
    scrubSlider_->setMaximum(0);  // updated by refreshTransportState() as the project changes
    scrubSlider_->setTracking(true);
    connect(scrubSlider_, &QSlider::valueChanged, this, &TimelinePanel::onScrubSliderMoved);

    stepForwardButton_ = new QToolButton(playheadBar);
    stepForwardButton_->setText(QStringLiteral("►|"));
    stepForwardButton_->setToolTip(QStringLiteral("Step forward one frame"));
    connect(stepForwardButton_, &QToolButton::clicked, this,
            &TimelinePanel::onStepForwardClicked);

    timecodeEdit_ = new QLineEdit(playheadBar);
    timecodeEdit_->setText(QStringLiteral("00:00:00.000"));
    timecodeEdit_->setFixedWidth(96);
    timecodeEdit_->setToolTip(QStringLiteral("HH:MM:SS.mmm — press Enter to seek"));
    connect(timecodeEdit_, &QLineEdit::returnPressed, this, &TimelinePanel::onTimecodeEdited);

    playheadLayout->addWidget(stepBackButton_);
    playheadLayout->addWidget(scrubSlider_, /*stretch=*/1);
    playheadLayout->addWidget(stepForwardButton_);
    playheadLayout->addWidget(timecodeEdit_);

    graph_ = new TimelineGraphView(model_.viewModel(), this);

    rootLayout->addWidget(transportBar);
    rootLayout->addWidget(playheadBar);
    rootLayout->addWidget(graph_, /*stretch=*/1);
}

QString TimelinePanel::formatPlayheadTimecode() const {
    const double seconds = transport_.playhead().seconds();
    const int totalMs = static_cast<int>(seconds * 1000.0 + 0.5);
    const int ms = totalMs % 1000;
    const int totalSeconds = totalMs / 1000;
    const int s = totalSeconds % 60;
    const int m = (totalSeconds / 60) % 60;
    const int h = totalSeconds / 3600;
    return QStringLiteral("%1:%2:%3.%4")
        .arg(h, 2, 10, QChar('0'))
        .arg(m, 2, 10, QChar('0'))
        .arg(s, 2, 10, QChar('0'))
        .arg(ms, 3, 10, QChar('0'));
}

void TimelinePanel::refreshTransportState() {
    if (undoButton_ != nullptr) {
        undoButton_->setEnabled(model_.canUndo());
    }
    if (redoButton_ != nullptr) {
        redoButton_->setEnabled(model_.canRedo());
    }
    if (playheadLabel_ != nullptr) {
        playheadLabel_->setText(formatPlayheadTimecode());
    }
    if (graph_ != nullptr) {
        graph_->setPlayhead(transport_.playhead());
    }
    const qint64 playheadMs = transport_.playhead().milliseconds();
    if (scrubSlider_ != nullptr) {
        const qint64 durationMs = std::max<qint64>(model_.timelineDurationMs(), playheadMs);
        // QSignalBlocker: setting the slider's range/value from engine state
        // must not itself fire valueChanged() and re-issue a seek — this refresh
        // reflects the current playhead, it does not move it.
        QSignalBlocker blocker(scrubSlider_);
        if (scrubSlider_->maximum() != durationMs) {
            scrubSlider_->setMaximum(static_cast<int>(durationMs));
        }
        if (scrubSlider_->value() != static_cast<int>(playheadMs)) {
            scrubSlider_->setValue(static_cast<int>(playheadMs));
        }
    }
    if (timecodeEdit_ != nullptr && !timecodeEdit_->hasFocus()) {
        // Only overwrite the field when the user is not actively editing it, so
        // a periodic refresh cannot clobber an in-progress timecode entry.
        timecodeEdit_->setText(formatPlayheadTimecode());
    }
}

void TimelinePanel::onPlayClicked() {
    transport_.play();
    refreshTransportState();
}

void TimelinePanel::onPauseClicked() {
    transport_.pause();
    refreshTransportState();
}

void TimelinePanel::onStopClicked() {
    transport_.stop();
    refreshTransportState();
}

void TimelinePanel::onUndoClicked() {
    model_.undo();
}

void TimelinePanel::onRedoClicked() {
    model_.redo();
}

void TimelinePanel::movePlayheadToMs(qint64 requestedMs) {
    // Requirement 4.5: clamp out-of-range requests to the nearer bound.
    const qint64 durationMs = model_.timelineDurationMs();
    const qint64 clampedMs = std::clamp<qint64>(requestedMs, 0, std::max<qint64>(durationMs, 0));

    // Requirement 4.2: snap to the nearest frame boundary at the PROJECT's edit
    // frame rate (not PreviewController::previewFrameRate(), which is clamped
    // to >= 24 fps for display smoothness and would snap a lower-fps project's
    // edits to the wrong grid).
    const FrameRate fps = model_.timelineFps();
    Duration snapped = Duration::fromMilliseconds(clampedMs);
    if (fps.isValid()) {
        const Duration frameDuration = fps.frameDuration();
        if (frameDuration.nanoseconds() > 0) {
            const std::int64_t frameIndex =
                (Duration::fromMilliseconds(clampedMs).nanoseconds() +
                 frameDuration.nanoseconds() / 2) /
                frameDuration.nanoseconds();
            snapped = fps.durationForFrames(frameIndex);
        }
    }
    transport_.seek(snapped);
    refreshTransportState();
}

void TimelinePanel::onScrubSliderMoved(int valueMs) { movePlayheadToMs(valueMs); }

void TimelinePanel::onTimecodeEdited() {
    if (timecodeEdit_ == nullptr) {
        return;
    }
    const QString text = timecodeEdit_->text().trimmed();
    // Parse HH:MM:SS.mmm (also accepting bare seconds/M:SS forms leniently, by
    // splitting on ':' and treating the last part as SS[.mmm]).
    const QStringList parts = text.split(QChar(':'));
    double hours = 0.0;
    double minutes = 0.0;
    double seconds = 0.0;
    bool ok = true;
    if (parts.size() == 3) {
        hours = parts[0].toDouble(&ok);
        if (ok) minutes = parts[1].toDouble(&ok);
        if (ok) seconds = parts[2].toDouble(&ok);
    } else if (parts.size() == 2) {
        minutes = parts[0].toDouble(&ok);
        if (ok) seconds = parts[1].toDouble(&ok);
    } else if (parts.size() == 1) {
        seconds = parts[0].toDouble(&ok);
    } else {
        ok = false;
    }
    if (!ok) {
        // An unparseable entry restores the current playhead's display rather
        // than silently doing nothing (so the field never shows stale text the
        // user typed but that had no effect).
        refreshTransportState();
        return;
    }
    const double totalSeconds = hours * 3600.0 + minutes * 60.0 + seconds;
    movePlayheadToMs(static_cast<qint64>(totalSeconds * 1000.0 + (totalSeconds < 0 ? -0.5 : 0.5)));
    // The field still has focus after Enter (pressing Enter does not blur a
    // QLineEdit), so refreshTransportState()'s hasFocus() guard — which exists
    // to avoid clobbering an IN-PROGRESS edit — would otherwise leave whatever
    // the user literally typed on screen instead of the frame-snapped canonical
    // form movePlayheadToMs() actually seeked to (Requirement 4.4: the
    // displayed timecode must agree with the playhead to the frame). This
    // completed edit is exactly the case that guard is not meant to cover.
    timecodeEdit_->setText(formatPlayheadTimecode());
}

void TimelinePanel::onStepBackClicked() {
    const FrameRate fps = model_.timelineFps();
    const Duration interval = fps.isValid() ? fps.frameDuration() : Duration::fromMilliseconds(1);
    movePlayheadToMs((transport_.playhead() - interval).milliseconds());
}

void TimelinePanel::onStepForwardClicked() {
    const FrameRate fps = model_.timelineFps();
    const Duration interval = fps.isValid() ? fps.frameDuration() : Duration::fromMilliseconds(1);
    movePlayheadToMs((transport_.playhead() + interval).milliseconds());
}

std::optional<ClipId> TimelinePanel::selectedClipId() const {
    return graph_ != nullptr ? graph_->selectedClipId() : std::nullopt;
}

std::optional<Uuid> TimelinePanel::selectedTrackId() const {
    return graph_ != nullptr ? graph_->selectedTrackId() : std::nullopt;
}

void TimelinePanel::onGraphSelectionChanged() {
    emit placementTrackChanged();
}

void TimelinePanel::onGraphSeekRequested(qint64 ms) {
    movePlayheadToMs(ms);
}

void TimelinePanel::onModelRefreshed() {
    if (graph_ != nullptr) {
        graph_->refresh();
    }
    emit placementTrackChanged();
}

}  // namespace palmier::ui

#endif  // PALMIER_HAVE_QT
