// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/TimelinePanel.hpp — the Qt 6 timeline dock panel (task 11.3).
//
// A QTreeView over TimelineModel (tracks as top-level rows, clips as their
// children) plus a small transport bar (play/pause/stop, undo/redo) and a
// playhead position label. All editing and playback DECISIONS are made
// elsewhere — TimelineViewModel (via TimelineModel) for edits, PreviewController
// for transport — so this panel is display/wiring glue: it forwards button
// presses to the controller/model and reflects their published state (canUndo/
// canRedo/lastIndication, and the controller's playhead) back into its own
// widgets.
//
// The transport bar drives the SAME PreviewController the preview view's QTimer
// pumps (both are constructed once, in the composition root, and shared), so
// pressing Play here is indistinguishable from pressing Play in the preview
// panel: there is exactly one playback engine per Requirement 1.1.
//
// Guarded by PALMIER_HAVE_QT, matching every other panel in this directory.

#ifndef PALMIER_UI_TIMELINEPANEL_HPP
#define PALMIER_UI_TIMELINEPANEL_HPP

#ifdef PALMIER_HAVE_QT

#include <QWidget>

#include "core/TimelineEngine.hpp"
#include "ui/GuiToolGateway.hpp"
#include "ui/PreviewController.hpp"
#include "ui/TimelineModel.hpp"

class QTreeView;
class QToolButton;
class QLabel;

namespace palmier::ui {

/// The timeline dock panel: a tree view over the project's tracks/clips plus a
/// transport bar wired to the shared PreviewController.
class TimelinePanel : public QWidget {
    Q_OBJECT

public:
    /// `engine` and `transport` must outlive the panel. `gateway`, when
    /// non-null, must also outlive the panel; passed through to the
    /// TimelineModel so every gesture issued from this view routes through the
    /// shared tool surface (task 11.4).
    TimelinePanel(TimelineEngine& engine, PreviewController& transport,
                 GuiToolGateway* gateway = nullptr, QWidget* parent = nullptr);
    ~TimelinePanel() override;

    /// The underlying Qt model, for MainWindow to wire Edit-menu actions
    /// (Undo/Redo/Delete Clip/Split at Playhead) against.
    [[nodiscard]] TimelineModel& model() noexcept { return model_; }
    [[nodiscard]] const TimelineModel& model() const noexcept { return model_; }

public slots:
    /// Refresh the transport-state-derived widgets (undo/redo enablement, the
    /// playhead label). Safe to call at any time; called automatically after
    /// every model change and on a light periodic refresh from the preview
    /// panel's timer via MainWindow.
    void refreshTransportState();

private slots:
    void onPlayClicked();
    void onPauseClicked();
    void onStopClicked();
    void onUndoClicked();
    void onRedoClicked();

private:
    void buildLayout();

    TimelineModel       model_;
    PreviewController&  transport_;

    QTreeView*   tree_ = nullptr;
    QToolButton* playButton_ = nullptr;
    QToolButton* pauseButton_ = nullptr;
    QToolButton* stopButton_ = nullptr;
    QToolButton* undoButton_ = nullptr;
    QToolButton* redoButton_ = nullptr;
    QLabel*      playheadLabel_ = nullptr;
};

}  // namespace palmier::ui

#endif  // PALMIER_HAVE_QT

#endif  // PALMIER_UI_TIMELINEPANEL_HPP
