// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/TimelinePanel.hpp — the Qt 6 timeline dock panel (task 11.3, task 11.1;
// usable-editor spec Requirements 1, 8).
//
// A graphical TimelineGraphView (lanes per track, clip rectangles positioned
// and sized by timeline start and duration, a time ruler and a playhead
// marker — Requirement 8) over the same TimelineModel/TimelineViewModel pair
// the tree it replaced used, plus a small transport bar (play/pause/stop,
// undo/redo) and a playhead position label. All editing and playback
// DECISIONS are made elsewhere — TimelineViewModel (via TimelineModel/
// TimelineGraphView) for edits, PreviewController for transport — so this
// panel is display/wiring glue: it forwards button presses to the
// controller/model and reflects their published state (canUndo/canRedo/
// lastIndication, and the controller's playhead) back into its own widgets.
//
// The transport bar drives the SAME PreviewController the preview view's QTimer
// pumps (both are constructed once, in the composition root, and shared), so
// pressing Play here is indistinguishable from pressing Play in the preview
// panel: there is exactly one playback engine per Requirement 1.1.
//
// Selection (usable-editor Requirement 1, 8.7): the panel forwards
// TimelineGraphView's own clipSelected()/selectionCleared() signals verbatim,
// so MainWindow can drive InspectorViewModel's selection without this panel
// (or the graph view) depending on InspectorViewModel itself — the panel only
// reports "what is selected", it does not decide what selection means to any
// other panel. The graph view already re-validates its selection against the
// fresh snapshot on every engine change and reports selectionCleared() if the
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
#include "ui/AudioMeterWidget.hpp"
#include "ui/GuiToolGateway.hpp"
#include "ui/PreviewController.hpp"
#include "ui/TimelineGraphView.hpp"
#include "ui/TimelineModel.hpp"

class QToolButton;
class QLabel;
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

    /// The programme level meter in the transport bar (monitoring-and-grading
    /// Requirement 1.4). Exposed so MainWindow — which is the only class that can
    /// reach the AudioEngine — installs its data providers, keeping this panel
    /// and the meter itself ignorant of the composition root.
    [[nodiscard]] AudioMeterWidget* levelMeter() const noexcept { return levelMeter_; }

    /// The graphical timeline view (monitoring-and-grading Requirement 2.3).
    /// Exposed for the same reason as levelMeter(): MainWindow installs the audio
    /// envelope provider, so neither this panel nor the view depends on the
    /// composition root or on the envelope service.
    [[nodiscard]] TimelineGraphView* graphView() const noexcept { return graph_; }

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
    void onGraphSelectionChanged();
    void onGraphSeekRequested(qint64 ms);
    void onModelRefreshed();
    void onScrubSliderMoved(int valueMs);
    void onTimecodeEdited();
    void onStepBackClicked();
    void onStepForwardClicked();

private:
    void buildLayout();
    // Move the playhead to the frame nearest `requestedMs`, clamped to
    // [0, timeline duration] (usable-editor Requirement 4). The single seam
    // every playhead-moving gesture — the slider, the timecode field, the two
    // step actions, and now the graph view's ruler/empty-lane clicks — funnels
    // through, so all five honour the identical snap/clamp rule.
    void movePlayheadToMs(qint64 requestedMs);
    // Format `transport_.playhead()` as HH:MM:SS.mmm, matching the label's
    // existing convention (refreshTransportState() computed this inline before;
    // it is now shared with the timecode field's display).
    [[nodiscard]] QString formatPlayheadTimecode() const;

    TimelineModel       model_;
    PreviewController&  transport_;

    TimelineGraphView* graph_ = nullptr;
    QToolButton* playButton_ = nullptr;
    QToolButton* pauseButton_ = nullptr;
    QToolButton* stopButton_ = nullptr;
    QToolButton* undoButton_ = nullptr;
    QToolButton* redoButton_ = nullptr;
    QLabel*      playheadLabel_ = nullptr;
    AudioMeterWidget* levelMeter_ = nullptr;  ///< monitoring-and-grading task 1.
    QSlider*     scrubSlider_ = nullptr;
    QLineEdit*   timecodeEdit_ = nullptr;
    QToolButton* stepBackButton_ = nullptr;
    QToolButton* stepForwardButton_ = nullptr;
};

}  // namespace palmier::ui

#endif  // PALMIER_HAVE_QT

#endif  // PALMIER_UI_TIMELINEPANEL_HPP
