// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/TimelinePanel.cpp — implementation of the timeline dock panel.
//
// Compiled only when Qt 6 is available (PALMIER_HAVE_QT). See TimelinePanel.hpp
// for the contract: this file only lays out widgets and forwards their signals
// to TimelineModel / PreviewController.

#include "ui/TimelinePanel.hpp"

#ifdef PALMIER_HAVE_QT

#include <QHBoxLayout>
#include <QLabel>
#include <QString>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

namespace palmier::ui {

TimelinePanel::TimelinePanel(TimelineEngine& engine, PreviewController& transport,
                             GuiToolGateway* gateway, QWidget* parent)
    : QWidget(parent), model_(engine, this, gateway), transport_(transport) {
    buildLayout();

    connect(&model_, &TimelineModel::modelRefreshed, this,
            &TimelinePanel::refreshTransportState);

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

    tree_ = new QTreeView(this);
    tree_->setModel(&model_);
    tree_->setAlternatingRowColors(true);
    tree_->setUniformRowHeights(true);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);

    rootLayout->addWidget(transportBar);
    rootLayout->addWidget(tree_, /*stretch=*/1);
}

void TimelinePanel::refreshTransportState() {
    if (undoButton_ != nullptr) {
        undoButton_->setEnabled(model_.canUndo());
    }
    if (redoButton_ != nullptr) {
        redoButton_->setEnabled(model_.canRedo());
    }
    if (playheadLabel_ != nullptr) {
        const double seconds = transport_.playhead().seconds();
        const int totalMs = static_cast<int>(seconds * 1000.0 + 0.5);
        const int ms = totalMs % 1000;
        const int totalSeconds = totalMs / 1000;
        const int s = totalSeconds % 60;
        const int m = (totalSeconds / 60) % 60;
        const int h = totalSeconds / 3600;
        playheadLabel_->setText(QStringLiteral("%1:%2:%3.%4")
                                    .arg(h, 2, 10, QChar('0'))
                                    .arg(m, 2, 10, QChar('0'))
                                    .arg(s, 2, 10, QChar('0'))
                                    .arg(ms, 3, 10, QChar('0')));
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

}  // namespace palmier::ui

#endif  // PALMIER_HAVE_QT
