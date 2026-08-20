// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/TimelinePanel.hpp — the Qt 6 timeline dock panel (task 11.3; usable-editor
// spec Requirement 1).
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
// Selection (usable-editor Requirement 1): the panel listens to the tree's own
// QItemSelectionModel and emits clipSelected()/selectionCleared() so MainWindow
// can drive InspectorViewModel's selection without this panel depending on
// InspectorViewModel itself — the panel only reports "what row is selected", it
// does not decide what selection means to any other panel. Selecting a clip row
// emits clipSelected(clipId); selecting a track row (or nothing) emits
// selectionCleared(). A model reset (any engine change) re-validates the
// selection against the fresh snapshot and emits selectionCleared() if the
// previously-selected clip is no longer present, so a clip deleted from any
// surface — the GUI, the MCP endpoint or the agent — cannot leave a stale
// selection pointed at nothing.
//
// Guarded by PALMIER_HAVE_QT, matching every other panel in this directory.

#ifndef PALMIER_UI_TIMELINEPANEL_HPP
#define PALMIER_UI_TIMELINEPANEL_HPP

#ifdef PALMIER_HAVE_QT

#include <optional>

#include <QWidget>

#include "core/Clip.hpp"
#include "core/TimelineEngine.hpp"
#include "core/Uuid.hpp"
#include "ui/GuiToolGateway.hpp"
#include "ui/PreviewController.hpp"
#include "ui/TimelineModel.hpp"

class QTreeView;
class QToolButton;
class QLabel;
class QItemSelection;
class QSlider;
class QLineEdit;

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

    /// The currently selected clip's id, or std::nullopt if a track row (or
    /// nothing) is selected. Exposed so a caller can read the current selection
    /// without waiting for the next clipSelected()/selectionCleared() signal
    /// (e.g. right after construction, before any user interaction).
    [[nodiscard]] std::optional<ClipId> selectedClipId() const;

    /// The track a placement gesture should target (usable-editor Requirement
    /// 3): the selected track row's id, OR — if a clip row is selected instead —
    /// the id of the track that clip belongs to, so selecting any row in a
    /// track's lane names that track as the placement target. std::nullopt only
    /// when nothing at all is selected.
    [[nodiscard]] std::optional<Uuid> selectedTrackId() const;

signals:
    /// A clip row became the tree's current selection.
    void clipSelected(const QString& clipId);

    /// The selection is now empty, or a track row (not a clip row) is selected.
    void selectionCleared();

    /// The placement-target track changed (any row selection change; see
    /// selectedTrackId()). Distinct from clipSelected()/selectionCleared(),
    /// which are about what the INSPECTOR shows, not about where a placement
    /// gesture would land.
    void placementTrackChanged();

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
    void onTreeSelectionChanged();
    void onModelRefreshed();
    void onScrubSliderMoved(int valueMs);
    void onTimecodeEdited();
    void onStepBackClicked();
    void onStepForwardClicked();

private:
    void buildLayout();
    // Re-derive and, if it changed, (re-)emit the selection signal for the
    // tree's CURRENT selection. Called after every user selection change and
    // after every model reset, so a clip that disappeared from under an active
    // selection is reported as cleared rather than left stale.
    void reconcileSelection();
    // Move the playhead to the frame nearest `requestedMs`, clamped to
    // [0, timeline duration] (usable-editor Requirement 4). The single seam
    // every playhead-moving gesture — the slider, the timecode field, and the
    // two step actions — funnels through, so all four honour the identical
    // snap/clamp rule.
    void movePlayheadToMs(qint64 requestedMs);
    // Format `transport_.playhead()` as HH:MM:SS.mmm, matching the label's
    // existing convention (refreshTransportState() computed this inline before;
    // it is now shared with the timecode field's display).
    [[nodiscard]] QString formatPlayheadTimecode() const;

    TimelineModel       model_;
    PreviewController&  transport_;

    QTreeView*   tree_ = nullptr;
    QToolButton* playButton_ = nullptr;
    QToolButton* pauseButton_ = nullptr;
    QToolButton* stopButton_ = nullptr;
    QToolButton* undoButton_ = nullptr;
    QToolButton* redoButton_ = nullptr;
    QLabel*      playheadLabel_ = nullptr;
    QSlider*     scrubSlider_ = nullptr;
    QLineEdit*   timecodeEdit_ = nullptr;
    QToolButton* stepBackButton_ = nullptr;
    QToolButton* stepForwardButton_ = nullptr;

    // The clip id most recently reported via clipSelected(), or std::nullopt if
    // the most recent report was selectionCleared(). Tracked so reconcileSelection()
    // only emits when the reconciled state actually differs from what was last
    // reported (a model reset that leaves the same clip selected should not
    // re-fire clipSelected() with the same id every time).
    std::optional<ClipId> lastReportedClipId_;
};

}  // namespace palmier::ui

#endif  // PALMIER_HAVE_QT

#endif  // PALMIER_UI_TIMELINEPANEL_HPP
