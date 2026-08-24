// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/MainWindow.cpp — implementation of the Qt 6 editor main window (task 11.2).
//
// Compiled only when Qt 6 is available (PALMIER_HAVE_QT). Builds the five
// QDockWidgets, the menu bar (File/Edit/Playback/Export/Help, in that order) and
// the status bar's three persistent notices, all bound to the SINGLE
// app::ApplicationComposition passed in. See MainWindow.hpp for the panel/menu
// inventory and the gateway-routing rationale (task 11.4).

#include "ui/MainWindow.hpp"

#ifdef PALMIER_HAVE_QT

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include <QAction>
#include <QDockWidget>
#include <QFileDialog>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QDesktopServices>

#include "app/ApplicationComposition.hpp"
#include "core/Duration.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/MediaManager.hpp"
#include "core/Project.hpp"
#include "core/Uuid.hpp"
#include "media/ImportValidation.hpp"
#include "services/AgentOrchestrator.hpp"
#include "services/Json.hpp"
#include "services/KeyMomentMarkers.hpp"
#include "ui/AgentChatPanel.hpp"
#include "ui/ExportDialog.hpp"
#include "ui/ProjectSettingsDialog.hpp"
#include "ui/InspectorPanel.hpp"
#include "ui/MediaBrowserPanel.hpp"
#include "ui/PreviewView.hpp"
#include "ui/TimelinePanel.hpp"

namespace palmier::ui {

namespace {
// A light periodic refresh for state that can change without an engine
// ChangeSet (the playhead label during playback, in particular).
constexpr int kStatusRefreshIntervalMs = 100;
}  // namespace

MainWindow::MainWindow(app::ApplicationComposition& composition, QWidget* parent)
    : QMainWindow(parent),
      composition_(composition),
      gateway_(composition.executor()),
      inspectorViewModel_(composition.timeline(), &gateway_),
      agentChatViewModel_(composition.agent()),
      fileActions_(composition.projectSession(), gateway_, makeUiPrompts()) {
    setWindowTitle(QStringLiteral("Palmier Pro"));
    setMinimumSize(1024, 640);
    resize(1440, 900);

    buildMenus();
    buildDocks();
    buildStatusBar();

    refreshWindowTitle();
    refreshUndoRedoActions();
    refreshNotices();
    refreshSelectionActions();

    statusRefreshTimer_ = new QTimer(this);
    connect(statusRefreshTimer_, &QTimer::timeout, this, &MainWindow::onStatusRefreshTick);
    statusRefreshTimer_->start(kStatusRefreshIntervalMs);
}

MainWindow::~MainWindow() = default;

// ---------------------------------------------------------------------------
// Docks (Requirement 1.2)
// ---------------------------------------------------------------------------

void MainWindow::buildDocks() {
    // Media Browser (left). Owns its own KeyMomentMarkerModel: key-moment
    // detection is not yet wired into ApplicationComposition (a later task's
    // scope), so the panel shows "not analyzed" for every clip until it is —
    // an honest gap rather than a fabricated detector.
    static services::KeyMomentMarkerModel keyMomentMarkers;  // outlives the panel
    auto validator = [](const std::filesystem::path& path) -> Result<MediaAssetRef> {
        Result<media::MediaInfo> probed = media::validateMediaImport(path);
        if (probed.isError()) {
            return err<MediaAssetRef>(probed.error());
        }
        return MediaAssetRef(Uuid::generateV4(), path.string());
    };
    mediaBrowserPanel_ =
        new MediaBrowserPanel(composition_.mediaLibrary(), keyMomentMarkers, validator, this);
    mediaBrowserPanel_->setGateway(&gateway_);
    mediaDock_ = new QDockWidget(QStringLiteral("Media Browser"), this);
    mediaDock_->setObjectName(QStringLiteral("MediaBrowserDock"));
    mediaDock_->setMinimumSize(80, 60);
    mediaDock_->setWidget(mediaBrowserPanel_);
    addDockWidget(Qt::LeftDockWidgetArea, mediaDock_);
    connect(mediaBrowserPanel_, &MediaBrowserPanel::librarySelectionChanged, this,
            &MainWindow::onPlacementContextChanged);

    // Timeline (bottom).
    timelinePanel_ = new TimelinePanel(composition_.timeline(), composition_.playbackEngine(),
                                       &gateway_, this);
    timelineDock_ = new QDockWidget(QStringLiteral("Timeline"), this);
    timelineDock_->setObjectName(QStringLiteral("TimelineDock"));
    timelineDock_->setMinimumSize(80, 60);
    timelineDock_->setWidget(timelinePanel_);
    addDockWidget(Qt::BottomDockWidgetArea, timelineDock_);
    connect(timelinePanel_, &TimelinePanel::clipSelected, this,
            &MainWindow::onTimelineClipSelected);
    connect(timelinePanel_, &TimelinePanel::selectionCleared, this,
            &MainWindow::onTimelineSelectionCleared);
    connect(timelinePanel_, &TimelinePanel::placementTrackChanged, this,
            &MainWindow::onPlacementContextChanged);

    // Preview (central) — binds to the composition's SHARED PreviewController,
    // not a second independently-constructed one (Requirement 1.1).
    previewView_ = new PreviewView(composition_.playbackEngine(), this);
    setCentralWidget(previewView_);

    // Inspector (right), tabbed with Agent Chat.
    inspectorPanel_ = new InspectorPanel(inspectorViewModel_, this);
    inspectorDock_ = new QDockWidget(QStringLiteral("Inspector"), this);
    inspectorDock_->setObjectName(QStringLiteral("InspectorDock"));
    inspectorDock_->setMinimumSize(80, 60);
    inspectorDock_->setWidget(inspectorPanel_);
    addDockWidget(Qt::RightDockWidgetArea, inspectorDock_);

    // Agent Chat (right, tabbed with Inspector).
    agentChatViewModel_.setMentionSource([this]() {
        return composition_.mediaLibrary().library();
    });
    agentChatPanel_ = new AgentChatPanel(agentChatViewModel_, this);
    agentChatDock_ = new QDockWidget(QStringLiteral("Agent Chat"), this);
    agentChatDock_->setObjectName(QStringLiteral("AgentChatDock"));
    agentChatDock_->setMinimumSize(80, 60);
    agentChatDock_->setWidget(agentChatPanel_);
    addDockWidget(Qt::RightDockWidgetArea, agentChatDock_);
    tabifyDockWidget(inspectorDock_, agentChatDock_);
    inspectorDock_->raise();

    // All five panels visible with no further user action (Requirement 1.4).
    mediaDock_->show();
    timelineDock_->show();
    inspectorDock_->show();
    agentChatDock_->show();
}

// ---------------------------------------------------------------------------
// Menus (Requirement 1.6): File, Edit, Playback, Export, Help, in that order.
// ---------------------------------------------------------------------------

void MainWindow::buildMenus() {
    QMenu* fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    QAction* newAction = fileMenu->addAction(QStringLiteral("&New"));
    newAction->setShortcut(QKeySequence::New);
    connect(newAction, &QAction::triggered, this, &MainWindow::onNew);

    QAction* openAction = fileMenu->addAction(QStringLiteral("&Open…"));
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &MainWindow::onOpen);

    QAction* saveAction = fileMenu->addAction(QStringLiteral("&Save"));
    saveAction->setShortcut(QKeySequence::Save);
    connect(saveAction, &QAction::triggered, this, &MainWindow::onSave);

    QAction* saveAsAction = fileMenu->addAction(QStringLiteral("Save &As…"));
    saveAsAction->setShortcut(QKeySequence::SaveAs);
    connect(saveAsAction, &QAction::triggered, this, &MainWindow::onSaveAs);

    fileMenu->addSeparator();
    QAction* settingsAction = fileMenu->addAction(QStringLiteral("Project &Settings…"));
    connect(settingsAction, &QAction::triggered, this, &MainWindow::onProjectSettings);

    fileMenu->addSeparator();
    QAction* quitAction = fileMenu->addAction(QStringLiteral("&Quit"));
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, &MainWindow::onQuit);

    QMenu* editMenu = menuBar()->addMenu(QStringLiteral("&Edit"));
    undoAction_ = editMenu->addAction(QStringLiteral("&Undo"));
    undoAction_->setShortcut(QKeySequence::Undo);
    connect(undoAction_, &QAction::triggered, this, &MainWindow::onUndo);

    redoAction_ = editMenu->addAction(QStringLiteral("&Redo"));
    redoAction_->setShortcut(QKeySequence::Redo);
    connect(redoAction_, &QAction::triggered, this, &MainWindow::onRedo);

    editMenu->addSeparator();
    deleteClipAction_ = editMenu->addAction(QStringLiteral("Delete &Clip"));
    deleteClipAction_->setShortcut(QKeySequence::Delete);
    deleteClipAction_->setEnabled(false);  // no clip is selected at startup
    connect(deleteClipAction_, &QAction::triggered, this, &MainWindow::onDeleteClip);

    splitAction_ = editMenu->addAction(QStringLiteral("&Split at Playhead"));
    splitAction_->setEnabled(false);  // no clip is selected at startup
    connect(splitAction_, &QAction::triggered, this, &MainWindow::onSplitAtPlayhead);

    // Ripple editing and gap management (usable-editor task 8.3; Requirement 5.4
    // and 5.5): all three are selection-gated, exactly like the two actions above.
    rippleDeleteAction_ = editMenu->addAction(QStringLiteral("Ripple &Delete Clip"));
    rippleDeleteAction_->setEnabled(false);  // no clip is selected at startup
    connect(rippleDeleteAction_, &QAction::triggered, this, &MainWindow::onRippleDelete);

    rippleTrimAction_ = editMenu->addAction(QStringLiteral("Ripple &Trim to Playhead"));
    rippleTrimAction_->setEnabled(false);  // no clip is selected at startup
    connect(rippleTrimAction_, &QAction::triggered, this, &MainWindow::onRippleTrimToPlayhead);

    closeGapAction_ = editMenu->addAction(QStringLiteral("Close &Gap After Clip"));
    closeGapAction_->setEnabled(false);  // no clip is selected at startup
    connect(closeGapAction_, &QAction::triggered, this, &MainWindow::onCloseGap);

    editMenu->addSeparator();
    addVideoTrackAction_ = editMenu->addAction(QStringLiteral("Add &Video Track"));
    connect(addVideoTrackAction_, &QAction::triggered, this, [this]() {
        if (timelinePanel_ != nullptr) {
            (void)timelinePanel_->model().addTrack(QStringLiteral("video"));
        }
    });

    addAudioTrackAction_ = editMenu->addAction(QStringLiteral("Add &Audio Track"));
    connect(addAudioTrackAction_, &QAction::triggered, this, [this]() {
        if (timelinePanel_ != nullptr) {
            (void)timelinePanel_->model().addTrack(QStringLiteral("audio"));
        }
    });

    // Usable-editor task 12; Requirement 9: a text clip must sit on a text
    // track, so a text track needs the same GUI creation affordance the
    // video/audio lanes already have.
    addTextTrackAction_ = editMenu->addAction(QStringLiteral("Add Te&xt Track"));
    connect(addTextTrackAction_, &QAction::triggered, this, [this]() {
        if (timelinePanel_ != nullptr) {
            (void)timelinePanel_->model().addTrack(QStringLiteral("text"));
        }
    });

    // Usable-editor task 13; Requirement 10: a caption cue must sit on a
    // caption track, for the identical reason a text clip needs a text track.
    addCaptionTrackAction_ = editMenu->addAction(QStringLiteral("Add &Caption Track"));
    connect(addCaptionTrackAction_, &QAction::triggered, this, [this]() {
        if (timelinePanel_ != nullptr) {
            (void)timelinePanel_->model().addTrack(QStringLiteral("caption"));
        }
    });

    editMenu->addSeparator();
    placeAtPlayheadAction_ = editMenu->addAction(QStringLiteral("&Place at Playhead"));
    placeAtPlayheadAction_->setEnabled(false);  // no asset/track selected at startup
    connect(placeAtPlayheadAction_, &QAction::triggered, this, &MainWindow::onPlaceAtPlayhead);

    QMenu* playbackMenu = menuBar()->addMenu(QStringLiteral("&Playback"));
    QAction* playPauseAction = playbackMenu->addAction(QStringLiteral("&Play/Pause"));
    playPauseAction->setShortcut(Qt::Key_Space);
    connect(playPauseAction, &QAction::triggered, this, &MainWindow::onPlayPause);

    QAction* stopAction = playbackMenu->addAction(QStringLiteral("&Stop"));
    connect(stopAction, &QAction::triggered, this, &MainWindow::onStop);

    QAction* goToStartAction = playbackMenu->addAction(QStringLiteral("&Go to Start"));
    connect(goToStartAction, &QAction::triggered, this, &MainWindow::onGoToStart);

    QMenu* exportMenu = menuBar()->addMenu(QStringLiteral("&Export"));
    QAction* exportVideoAction = exportMenu->addAction(QStringLiteral("Export &Video…"));
    connect(exportVideoAction, &QAction::triggered, this, &MainWindow::onExportVideo);

    QAction* cancelExportAction = exportMenu->addAction(QStringLiteral("&Cancel Export"));
    connect(cancelExportAction, &QAction::triggered, this, &MainWindow::onCancelExport);

    QMenu* helpMenu = menuBar()->addMenu(QStringLiteral("&Help"));
    QAction* docsAction = helpMenu->addAction(QStringLiteral("&Documentation"));
    connect(docsAction, &QAction::triggered, this, &MainWindow::onDocumentation);

    QAction* aboutAction = helpMenu->addAction(QStringLiteral("&About"));
    connect(aboutAction, &QAction::triggered, this, &MainWindow::onAbout);
}

// ---------------------------------------------------------------------------
// Status bar: the three persistent notices (Requirements 5.6, 6.7, 10.4).
// ---------------------------------------------------------------------------

void MainWindow::buildStatusBar() {
    gpuNoticeLabel_ = new QLabel(this);
    gpuNoticeLabel_->hide();
    statusBar()->addPermanentWidget(gpuNoticeLabel_);

    softwareCompositingNoticeLabel_ = new QLabel(this);
    softwareCompositingNoticeLabel_->hide();
    statusBar()->addPermanentWidget(softwareCompositingNoticeLabel_);

    audioNoticeLabel_ = new QLabel(this);
    audioNoticeLabel_->hide();
    statusBar()->addPermanentWidget(audioNoticeLabel_);

    statusBar()->showMessage(QStringLiteral("Ready"));
}

void MainWindow::refreshNotices() {
    const std::string gpuNotice = composition_.gpuUnavailableNotice();
    if (gpuNotice.empty()) {
        gpuNoticeLabel_->hide();
    } else {
        gpuNoticeLabel_->setText(QString::fromStdString(gpuNotice));
        gpuNoticeLabel_->show();
    }

    const std::string softwareNotice = composition_.softwareCompositingNotice();
    if (softwareNotice.empty()) {
        softwareCompositingNoticeLabel_->hide();
    } else {
        softwareCompositingNoticeLabel_->setText(QString::fromStdString(softwareNotice));
        softwareCompositingNoticeLabel_->show();
    }

    const std::string audioNotice = composition_.audioUnavailableNotice();
    if (audioNotice.empty()) {
        audioNoticeLabel_->hide();
    } else {
        audioNoticeLabel_->setText(QString::fromStdString(audioNotice));
        audioNoticeLabel_->show();
    }
}

void MainWindow::refreshWindowTitle() {
    const services::ProjectSession::Status status = composition_.projectSession().status();
    QString title = QStringLiteral("Palmier Pro — %1").arg(QString::fromStdString(status.name));
    if (status.modified) {
        title += QStringLiteral(" *");
    }
    setWindowTitle(title);
}

void MainWindow::refreshUndoRedoActions() {
    if (undoAction_ != nullptr) {
        undoAction_->setEnabled(timelinePanel_ != nullptr && timelinePanel_->model().canUndo());
    }
    if (redoAction_ != nullptr) {
        redoAction_->setEnabled(timelinePanel_ != nullptr && timelinePanel_->model().canRedo());
    }
}

// ---------------------------------------------------------------------------
// File actions (task 11.5)
// ---------------------------------------------------------------------------

UiPrompts MainWindow::makeUiPrompts() {
    UiPrompts prompts;
    prompts.confirmUnsavedChanges = [this](const std::string& projectName) {
        const QMessageBox::StandardButton chosen = QMessageBox::warning(
            this, QStringLiteral("Unsaved Changes"),
            QStringLiteral("'%1' has unsaved changes. Save before continuing?")
                .arg(QString::fromStdString(projectName)),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
            QMessageBox::Save);
        switch (chosen) {
            case QMessageBox::Save:    return UnsavedChangesChoice::Save;
            case QMessageBox::Discard: return UnsavedChangesChoice::Discard;
            default:                   return UnsavedChangesChoice::Cancel;
        }
    };
    prompts.promptSaveDestination = [this]() -> std::optional<std::string> {
        const QString path = QFileDialog::getSaveFileName(
            this, QStringLiteral("Save Project"), QString(),
            QStringLiteral("Palmier Project (*.palmier)"));
        if (path.isEmpty()) {
            return std::nullopt;
        }
        return path.toStdString();
    };
    prompts.promptOpenSource = [this]() -> std::optional<std::string> {
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Open Project"), QString(),
            QStringLiteral("Palmier Project (*.palmier)"));
        if (path.isEmpty()) {
            return std::nullopt;
        }
        return path.toStdString();
    };
    prompts.notify = [this](FileActionKind /*kind*/, const FileActionResult& result) {
        if (result.dismissed) {
            return;
        }
        statusBar()->showMessage(QString::fromStdString(result.message), 5000);
        refreshWindowTitle();
        if (!result.ok) {
            QMessageBox::warning(this, QStringLiteral("Project"),
                                 QString::fromStdString(result.message));
        }
    };
    return prompts;
}

void MainWindow::onNew() {
    const services::ProjectSession::Status status = composition_.projectSession().status();
    (void)fileActions_.newProject(status.name.empty() ? "Untitled" : status.name, 30.0, 1920,
                                  1080);
}

void MainWindow::onOpen() { (void)fileActions_.open(); }

void MainWindow::onSave() { (void)fileActions_.save(); }

void MainWindow::onSaveAs() { (void)fileActions_.saveAs(); }

void MainWindow::onQuit() { close(); }

// ---------------------------------------------------------------------------
// Edit actions
// ---------------------------------------------------------------------------

void MainWindow::onUndo() {
    if (timelinePanel_ != nullptr) {
        timelinePanel_->model().undo();
    }
    refreshUndoRedoActions();
}

void MainWindow::onRedo() {
    if (timelinePanel_ != nullptr) {
        timelinePanel_->model().redo();
    }
    refreshUndoRedoActions();
}

void MainWindow::onDeleteClip() {
    if (!inspectorViewModel_.hasSelection()) {
        return;  // disabled per refreshSelectionActions(); a shortcut race is a no-op
    }
    (void)gateway_.deleteClip(*inspectorViewModel_.selectedClipId());
}

void MainWindow::onSplitAtPlayhead() {
    if (!inspectorViewModel_.hasSelection() || previewView_ == nullptr) {
        return;  // disabled per refreshSelectionActions(); a shortcut race is a no-op
    }
    const Duration playhead = previewView_->controller().playhead();
    (void)gateway_.splitClip(*inspectorViewModel_.selectedClipId(), playhead);
}

void MainWindow::onRippleDelete() {
    if (!inspectorViewModel_.hasSelection()) {
        return;  // disabled per refreshSelectionActions(); a shortcut race is a no-op
    }
    (void)gateway_.rippleDelete(*inspectorViewModel_.selectedClipId());
}

void MainWindow::onRippleTrimToPlayhead() {
    if (!inspectorViewModel_.hasSelection() || previewView_ == nullptr) {
        return;  // disabled per refreshSelectionActions(); a shortcut race is a no-op
    }
    // The tool takes a SOURCE boundary, while the playhead is a timeline position,
    // so the offset from the clip's start is what carries over. The clip's own
    // geometry comes from the Inspector's projection of the engine snapshot.
    const std::optional<ClipInspectorView> clip = inspectorViewModel_.selectedClip();
    if (!clip) {
        return;  // the selection no longer resolves; nothing to trim
    }
    const Duration playhead = previewView_->controller().playhead();
    const Duration offset = playhead - clip->timelineStart;
    if (offset.isNegative()) {
        return;  // the playhead is before the clip; there is no out-point to set
    }
    // Trimming the out-point to the playhead is the ripple edit an editor means by
    // "trim to playhead"; the command clamps the boundary into the legal range.
    (void)gateway_.rippleTrim(clip->id, RippleTrimCommand::Edge::End,
                              clip->sourceIn + offset, clip->sourceOut);
}

void MainWindow::onCloseGap() {
    if (!inspectorViewModel_.hasSelection()) {
        return;  // disabled per refreshSelectionActions(); a shortcut race is a no-op
    }
    (void)gateway_.closeGap(*inspectorViewModel_.selectedClipId());
}

// ---------------------------------------------------------------------------
// Selection (usable-editor Requirement 1): the Timeline panel reports which
// row is selected; this is where "what does a clip selection mean to the rest
// of the shell" is decided — driving the Inspector and the two selection-gated
// Edit actions. TimelinePanel itself has no InspectorViewModel dependency.
// ---------------------------------------------------------------------------

void MainWindow::onTimelineClipSelected(const QString& clipId) {
    if (const std::optional<Uuid> id = Uuid::parse(clipId.toStdString())) {
        inspectorViewModel_.selectClip(*id);
    }
    refreshSelectionActions();
}

void MainWindow::onTimelineSelectionCleared() {
    inspectorViewModel_.clearSelection();
    refreshSelectionActions();
}

void MainWindow::refreshSelectionActions() {
    const bool hasSelection = inspectorViewModel_.hasSelection();
    if (deleteClipAction_ != nullptr) {
        deleteClipAction_->setEnabled(hasSelection);
    }
    if (splitAction_ != nullptr) {
        splitAction_->setEnabled(hasSelection);
    }
    // Requirement 5.4: the ripple actions are activatable exactly where a clip is
    // selected. Whether the edit is *applicable* (a gap actually follows the clip,
    // say) stays with the command, which refuses without changing the project.
    if (rippleDeleteAction_ != nullptr) {
        rippleDeleteAction_->setEnabled(hasSelection);
    }
    if (rippleTrimAction_ != nullptr) {
        rippleTrimAction_->setEnabled(hasSelection);
    }
    if (closeGapAction_ != nullptr) {
        closeGapAction_->setEnabled(hasSelection);
    }
}

// ---------------------------------------------------------------------------
// Placement (usable-editor Requirement 3): before this, GuiToolGateway::addClip
// and TimelineViewModel::addClip already existed and were tested at the
// gateway/view-model level, but no widget ever called either — there was no
// route from "an asset is in the library" to "a clip exists on the timeline"
// through the GUI at all. This is that route: take the Media Browser's
// selected library asset and the Timeline panel's selected track, and place a
// clip spanning that asset's full probed duration at the current playhead.
// ---------------------------------------------------------------------------

void MainWindow::onPlacementContextChanged() { refreshPlacementAction(); }

void MainWindow::refreshPlacementAction() {
    if (placeAtPlayheadAction_ == nullptr) {
        return;
    }
    const bool hasAsset =
        mediaBrowserPanel_ != nullptr &&
        mediaBrowserPanel_->viewModel().selectedLibraryAsset().has_value();
    const bool hasTrack =
        timelinePanel_ != nullptr && timelinePanel_->selectedTrackId().has_value();
    placeAtPlayheadAction_->setEnabled(hasAsset && hasTrack);
}

void MainWindow::onPlaceAtPlayhead() {
    if (mediaBrowserPanel_ == nullptr || timelinePanel_ == nullptr || previewView_ == nullptr) {
        return;
    }
    const std::optional<Uuid> assetId = mediaBrowserPanel_->viewModel().selectedLibraryAsset();
    const std::optional<Uuid> trackId = timelinePanel_->selectedTrackId();
    if (!assetId.has_value() || !trackId.has_value()) {
        return;  // disabled per refreshPlacementAction(); a shortcut race is a no-op
    }

    const std::optional<MediaAssetRef> asset = composition_.mediaLibrary().asset(*assetId);
    if (!asset.has_value()) {
        statusBar()->showMessage(QStringLiteral("Selected asset is no longer in the library"),
                                 5000);
        return;
    }

    // Size the placed clip to the asset's full probed duration when known
    // (assets imported through the gateway's media.import call always have
    // one cached); otherwise fall back to a nominal 1-second placeholder range
    // rather than refusing the gesture outright, since sourceOutNs > sourceInNs
    // is a hard requirement of the add_clip tool and there is no other source
    // of truth for an untracked duration at this layer.
    const Duration duration = mediaBrowserPanel_->viewModel()
                                  .assetDuration(*assetId)
                                  .value_or(Duration::fromSeconds(1.0));

    const Duration playhead = previewView_->controller().playhead();
    const Result<services::Json> result =
        gateway_.addClip(*trackId, *assetId, asset->sourcePath, std::nullopt, playhead,
                         Duration::zero(), duration, /*opacity=*/1.0, /*gain=*/1.0);
    if (result.isError()) {
        statusBar()->showMessage(
            QStringLiteral("Could not place clip: %1")
                .arg(QString::fromStdString(result.error().message())),
            8000);
    }
}

// ---------------------------------------------------------------------------
// Playback actions
// ---------------------------------------------------------------------------

void MainWindow::onPlayPause() {
    if (previewView_ == nullptr) {
        return;
    }
    if (previewView_->controller().isPlaying()) {
        previewView_->pause();
    } else {
        previewView_->play();
    }
}

void MainWindow::onStop() {
    if (previewView_ != nullptr) {
        previewView_->stop();
    }
}

void MainWindow::onGoToStart() {
    if (previewView_ != nullptr) {
        previewView_->seekSeconds(0.0);
    }
}

// ---------------------------------------------------------------------------
// Export actions (task 11.6)
// ---------------------------------------------------------------------------

void MainWindow::onExportVideo() {
    if (exportDialog_ == nullptr) {
        const services::ProjectSession::Status status = composition_.projectSession().status();
        (void)status;  // resolution/fps come straight from the live snapshot below
        const Project snapshot = composition_.timeline().snapshot();
        exportDialog_ = std::make_unique<ExportDialog>(composition_.exportCoordinator(),
                                                       snapshot.canvas, snapshot.timelineFps,
                                                       this);
        connect(exportDialog_.get(), &ExportDialog::exportFinished, this,
                [this](bool succeeded, const QString& message) {
                    statusBar()->showMessage(message, succeeded ? 5000 : 8000);
                });
    }
    exportDialog_->show();
    exportDialog_->raise();
    exportDialog_->activateWindow();
}

void MainWindow::onCancelExport() { composition_.exportCoordinator().cancel(); }

void MainWindow::onProjectSettings() {
    // A fresh dialog every time (unlike exportDialog_, which persists so a
    // running export keeps being polled): there is no in-flight state here to
    // preserve across closes, and a fresh snapshot is exactly Requirement 7.2's
    // "reads the current values" — reopening always shows what is true now,
    // including a settings change made from the MCP endpoint or the agent while
    // the dialog was closed.
    const Project snapshot = composition_.timeline().snapshot();
    settingsDialog_ = std::make_unique<ProjectSettingsDialog>(
        gateway_, snapshot.timelineFps, snapshot.canvas, snapshot.colorSpace, this);
    settingsDialog_->setAttribute(Qt::WA_DeleteOnClose, false);
    settingsDialog_->show();
    settingsDialog_->raise();
    settingsDialog_->activateWindow();
}

// ---------------------------------------------------------------------------
// Help actions
// ---------------------------------------------------------------------------

void MainWindow::onDocumentation() {
    QDesktopServices::openUrl(
        QUrl(QStringLiteral("https://github.com/palmier-io/palmier-pro")));
}

void MainWindow::onAbout() {
    QMessageBox::about(
        this, QStringLiteral("About Palmier Pro"),
        QStringLiteral("Palmier Pro — an AI-native, multi-track video editor.\n\n"
                       "GPLv3-licensed timeline editor and MCP server; the "
                       "generative-AI capability is an optional, account-gated "
                       "closed service."));
}

// ---------------------------------------------------------------------------
// Periodic refresh
// ---------------------------------------------------------------------------

void MainWindow::onStatusRefreshTick() {
    if (timelinePanel_ != nullptr) {
        timelinePanel_->refreshTransportState();
    }
    refreshUndoRedoActions();
    refreshNotices();
    refreshSelectionActions();
    if (addVideoTrackAction_ != nullptr && addAudioTrackAction_ != nullptr &&
        addTextTrackAction_ != nullptr && addCaptionTrackAction_ != nullptr &&
        timelinePanel_ != nullptr) {
        const bool canAddTrack = timelinePanel_->model().canAddTrack();
        addVideoTrackAction_->setEnabled(canAddTrack);
        addAudioTrackAction_->setEnabled(canAddTrack);
        addTextTrackAction_->setEnabled(canAddTrack);
        addCaptionTrackAction_->setEnabled(canAddTrack);
    }
}

} // namespace palmier::ui

#endif // PALMIER_HAVE_QT
