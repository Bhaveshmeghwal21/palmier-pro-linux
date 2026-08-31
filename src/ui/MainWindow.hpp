// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/MainWindow.hpp — the Qt 6 editor main window (application shell).
//
// Task 11.2: builds the docked shell, the menu bar and the status bar over the
// application's single ApplicationComposition (Requirement 1.1) — the same
// composed graph the MCP endpoint and the in-app agent drive. Five QDockWidgets
// are created and shown with no further user action:
//
//   * Timeline   (bottom)   — TimelinePanel over the composition's TimelineEngine
//   * Preview    (central)  — PreviewView bound to the composition's SHARED
//                             PreviewController (ui::PreviewController&), not a
//                             second, independently-constructed one
//   * Inspector  (right)    — InspectorPanel, tabbed with Agent Chat
//   * Media      (left)     — MediaBrowserPanel over the session's MediaManager
//   * Agent Chat (right)    — AgentChatPanel over the composition's AgentOrchestrator
//
// Every panel's mutating gestures are re-pointed at a single ui::GuiToolGateway
// owned by this window and bound to the composition's McpToolExecutor, so a GUI
// edit is schema-validated, atomic, logged and rolled back exactly like an MCP
// or agent-issued edit (task 11.4; Requirements 1.7, 9.4, 11.5).
//
// The window carries NO platform-decision logic (that stays in
// app::PlatformCompatibility, run by the entry point before a MainWindow is
// constructed) and performs no network activity of its own (Requirements 1.3,
// 13.3, 13.4). Constructing it does not start or stop the MCP server; the entry
// point owns that lifecycle.
//
// Guarded by PALMIER_HAVE_QT, matching every other panel in this directory.

#ifndef PALMIER_UI_MAINWINDOW_HPP
#define PALMIER_UI_MAINWINDOW_HPP

#ifdef PALMIER_HAVE_QT

#include <memory>

#include <QMainWindow>

#include "ui/AgentChatViewModel.hpp"
#include "ui/GuiToolGateway.hpp"
#include "ui/InspectorViewModel.hpp"
#include "ui/ProjectFileActions.hpp"

class QLabel;
class QDockWidget;
class QTimer;

namespace palmier::app {
class ApplicationComposition;
}  // namespace palmier::app

namespace palmier::ui {

class TimelinePanel;
class PreviewView;
class InspectorPanel;
class MediaBrowserPanel;
class AgentChatPanel;
class ExportDialog;
class ProjectSettingsDialog;

/// The editor's top-level window: the docked shell over one
/// app::ApplicationComposition (Requirement 1.1).
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    /// `composition` must outlive the window (it is owned by app::main and
    /// constructed before the window, per the entry point's launch sequence).
    explicit MainWindow(app::ApplicationComposition& composition, QWidget* parent = nullptr);
    ~MainWindow() override;

    /// The gateway every panel's mutating gestures route through (task 11.4).
    /// Exposed for tests that need to drive a gesture through the exact path
    /// the shell uses.
    [[nodiscard]] GuiToolGateway& gateway() noexcept { return gateway_; }

private slots:
    void onNew();
    void onOpen();
    void onSave();
    void onSaveAs();
    void onQuit();
    void onUndo();
    void onRedo();
    void onDeleteClip();
    void onSplitAtPlayhead();
    // Still-frame capture (usable-editor tasks.md task 14).
    void onCaptureFrame();
    // Ripple editing and gap management (task 8.3; Requirement 5).
    void onRippleDelete();
    void onRippleTrimToPlayhead();
    void onCloseGap();
    void onPlayPause();
    void onStop();
    void onGoToStart();
    void onExportVideo();
    void onCancelExport();
    void onProjectSettings();  // task 10.2; Requirement 7.2
    void onAbout();
    void onDocumentation();
    void onStatusRefreshTick();
    void onTimelineClipSelected(const QString& clipId);
    void onTimelineSelectionCleared();
    void onPlaceAtPlayhead();
    void onPlacementContextChanged();

private:
    void buildDocks();
    void buildMenus();
    void buildStatusBar();
    void refreshWindowTitle();
    void refreshUndoRedoActions();
    void refreshNotices();
    void refreshSelectionActions();
    void refreshPlacementAction();
    UiPrompts makeUiPrompts();

    app::ApplicationComposition& composition_;
    GuiToolGateway                gateway_;
    InspectorViewModel            inspectorViewModel_;
    AgentChatViewModel            agentChatViewModel_;
    ProjectFileActions            fileActions_;

    /// Liveness token for the envelope service's ready callback, which the
    /// service's worker thread fires and which must not touch this window's views
    /// after it is gone (monitoring-and-grading Requirement 2.2). Declared FIRST
    /// among these members so it is destroyed before them, and — being a member —
    /// before the QWidget base destroys the child views themselves.
    std::shared_ptr<int> envelopeCallbackToken_ = std::make_shared<int>(0);

    TimelinePanel*      timelinePanel_ = nullptr;
    PreviewView*        previewView_ = nullptr;
    InspectorPanel*     inspectorPanel_ = nullptr;
    MediaBrowserPanel*  mediaBrowserPanel_ = nullptr;
    AgentChatPanel*     agentChatPanel_ = nullptr;

    QDockWidget* timelineDock_ = nullptr;
    QDockWidget* inspectorDock_ = nullptr;
    QDockWidget* mediaDock_ = nullptr;
    QDockWidget* agentChatDock_ = nullptr;

    // Menu actions that must reflect engine/session state (enabled/disabled).
    QAction* undoAction_ = nullptr;
    QAction* redoAction_ = nullptr;
    QAction* deleteClipAction_ = nullptr;
    QAction* splitAction_ = nullptr;
    QAction* rippleDeleteAction_ = nullptr;
    QAction* rippleTrimAction_ = nullptr;
    QAction* closeGapAction_ = nullptr;
    QAction* addVideoTrackAction_ = nullptr;
    QAction* addAudioTrackAction_ = nullptr;
    QAction* addTextTrackAction_ = nullptr;  ///< usable-editor task 12; Requirement 9.
    QAction* addCaptionTrackAction_ = nullptr;  ///< usable-editor task 13; Requirement 10.
    QAction* placeAtPlayheadAction_ = nullptr;

    // Status bar: the three persistent notices (GPU, software-compositing,
    // audio) plus the export progress surface's parent dialog.
    QLabel* gpuNoticeLabel_ = nullptr;
    QLabel* softwareCompositingNoticeLabel_ = nullptr;
    QLabel* audioNoticeLabel_ = nullptr;

    std::unique_ptr<ExportDialog> exportDialog_;
    std::unique_ptr<ProjectSettingsDialog> settingsDialog_;  // task 10.2

    // Lightweight periodic refresh of undo/redo enablement and notices; the
    // timeline panel's own transport-state refresh is driven by its model's
    // change signal, but the playhead label and notices need a time-based tick
    // since they can change from playback (a different subsystem) without an
    // engine ChangeSet.
    QTimer* statusRefreshTimer_ = nullptr;
};

} // namespace palmier::ui

#endif // PALMIER_HAVE_QT

#endif // PALMIER_UI_MAINWINDOW_HPP
