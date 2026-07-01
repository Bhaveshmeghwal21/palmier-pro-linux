// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/MainWindow.cpp — implementation of the Qt 6 editor main window.
//
// Compiled only when Qt 6 is available (PALMIER_HAVE_QT). The window sets up the
// editor chrome (title, menu bar, and a central placeholder that later tasks
// replace with the timeline/preview/inspector/media/agent panels). It performs
// no network access and no heavyweight work, so it can be shown well within the
// 15-second launch budget (Requirement 1.3).

#include "ui/MainWindow.hpp"

#ifdef PALMIER_HAVE_QT

#include <QAction>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QStatusBar>

namespace palmier::ui {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("Palmier Pro"));
    resize(1280, 720);

    buildMenus();
    buildCentralPlaceholder();

    statusBar()->showMessage(QStringLiteral("Ready"));
}

MainWindow::~MainWindow() = default;

void MainWindow::buildMenus() {
    QMenu* fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    QAction* quitAction = fileMenu->addAction(QStringLiteral("&Quit"));
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);

    // Edit menu placeholder — undo/redo are wired to the TimelineEngine by the
    // timeline-view task (19.2).
    menuBar()->addMenu(QStringLiteral("&Edit"));
}

void MainWindow::buildCentralPlaceholder() {
    placeholder_ = new QLabel(
        QStringLiteral("Palmier Pro editor\n(timeline, preview, inspector, media, "
                       "and agent panels load here)"),
        this);
    placeholder_->setAlignment(Qt::AlignCenter);
    setCentralWidget(placeholder_);
}

} // namespace palmier::ui

#endif // PALMIER_HAVE_QT
