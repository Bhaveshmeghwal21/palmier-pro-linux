// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/TimelineGraphView.hpp — the graphical (non-tree) timeline view (task 11.1;
// usable-editor Requirement 8).
//
// Replaces the QTreeView TimelinePanel used to host: each track is a horizontal
// lane, each clip is a rectangle whose x-position and width correspond to its
// timelineStart and duration (Requirement 8.1), a time ruler runs along the top
// and a playhead marker is drawn at the current position (Requirement 8.2).
//
// This widget reads geometry through TimelineViewModel's typed API
// (trackAt/clipAt/locate) rather than through QAbstractItemModel roles, since a
// custom paintEvent wants direct numeric access on every repaint rather than a
// QVariant round-trip per cell. It shares the SAME TimelineViewModel instance
// TimelineModel already owns (via TimelineModel::viewModel()), so there is one
// adapter, one engine subscription, and one notion of "the current project" —
// this view and the tree it replaces never had two independent projections to
// keep in sync, and that stays true here.
//
// Every mutation (move, trim) is issued through TimelineViewModel's own gesture
// methods, which is the identical EditCommand path the MCP endpoint and the
// in-app agent use, and which already classifies a rejected drop as
// GestureIndication::InvalidDrop with the clip retained (Requirement 8.5) — this
// widget does not need to duplicate that logic, only to visually revert the
// dragged rectangle to its last-committed position when the gesture is rejected
// (the model's cached snapshot IS that position, since a rejected gesture never
// mutates it).
//
// Interaction (Requirements 8.3-8.6):
//   * Click on the ruler or an empty part of a lane -> seekRequested(ms), which
//     TimelinePanel connects to the SAME movePlayheadToMs() the scrub slider and
//     timecode field already use, so every playhead-moving gesture in the panel
//     snaps to the project's edit frame rate identically (Requirement 4.2).
//   * Click on a clip rectangle -> selects it (clipSelected()/selectionCleared(),
//     matching TimelinePanel's existing signal contract) and repaints it
//     highlighted (Requirement 8.7).
//   * Drag a clip rectangle's interior horizontally -> a live-follows-the-mouse
//     preview during the drag, then TimelineViewModel::moveClip() on release.
//   * Drag within kEdgeGrabPx of a clip's left/right edge -> trims that edge
//     through TimelineViewModel::trimClip() on release.
//   * Mouse wheel with Ctrl held -> zoom, keeping the playhead's pixel position
//     fixed across the zoom change (Requirement 8.4) rather than the view's
//     left edge, so zooming while the playhead is off-screen still brings it
//     back into view instead of zooming around a point the user cannot see.

#ifndef PALMIER_UI_TIMELINEGRAPHVIEW_HPP
#define PALMIER_UI_TIMELINEGRAPHVIEW_HPP

#ifdef PALMIER_HAVE_QT

#include <cstddef>
#include <optional>

#include <QPoint>
#include <QWidget>

#include "core/Clip.hpp"
#include "core/Duration.hpp"
#include "core/Uuid.hpp"

class QMouseEvent;
class QPaintEvent;
class QResizeEvent;
class QWheelEvent;

namespace palmier::ui {

class TimelineViewModel;

class TimelineGraphView : public QWidget {
    Q_OBJECT

public:
    /// `viewModel` must outlive this widget.
    explicit TimelineGraphView(TimelineViewModel& viewModel, QWidget* parent = nullptr);
    ~TimelineGraphView() override;

    /// The clip id of the current selection, or std::nullopt if a track lane
    /// (or nothing) is selected — mirrors TimelinePanel::selectedClipId()'s
    /// existing contract exactly, so TimelinePanel's own accessor can simply
    /// forward here instead of querying a QItemSelectionModel.
    [[nodiscard]] std::optional<ClipId> selectedClipId() const noexcept { return selectedClip_; }

    /// The track a placement gesture should target: the selected lane's track,
    /// or — if a clip is selected instead — the track that clip belongs to.
    /// std::nullopt only when nothing at all is selected. Mirrors
    /// TimelinePanel::selectedTrackId()'s existing contract.
    [[nodiscard]] std::optional<Uuid> selectedTrackId() const noexcept { return selectedTrack_; }

    /// Select a track's lane directly (e.g. from a freshly-added track, so the
    /// new lane becomes the placement target the way selecting its tree row
    /// used to). Emits the same selection signals a mouse click on that lane
    /// would, if the selection actually changes.
    void selectTrack(Uuid trackId);

    /// Clear the selection programmatically, as if empty space were clicked.
    void clearSelection();

    /// Re-derive rendering (and, if the previously-selected clip disappeared,
    /// the reported selection) from the current project snapshot. Called after
    /// every engine change; safe to call at any time.
    void refresh();

    /// Set the playhead position to paint the marker at (Requirement 8.2).
    /// TimelinePanel owns the actual playhead (PreviewController does), so it
    /// calls this from the same refreshTransportState() that already updates
    /// every other playhead-derived widget, keeping this view's marker and the
    /// scrub slider/timecode field in agreement to the same source of truth.
    void setPlayhead(Duration position);

signals:
    /// A clip rectangle was clicked/selected.
    void clipSelected(const QString& clipId);
    /// The selection became empty, or a lane (not a clip) was selected.
    void selectionCleared();
    /// The placement-target track changed (any selection change).
    void placementTrackChanged();
    /// A click on the ruler or an empty lane asked to move the playhead to this
    /// timeline position (before snapping/clamping, which TimelinePanel's
    /// movePlayheadToMs() already applies identically for every other
    /// playhead-moving gesture in the panel).
    void seekRequested(qint64 ms);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    // Grants the test suite direct access to the protected event handlers
    // above. QCoreApplication::sendEvent() reliably reaches
    // mousePressEvent()/mouseReleaseEvent() (both are simple, singular "point"
    // events), but a hand-constructed QEvent::MouseMove has proven unreliable
    // to deliver this way under Qt6's QSinglePointEvent internals in a
    // synthetic (no real platform grab) test — this friendship lets the test
    // call mouseMoveEvent() directly instead of depending on that delivery
    // path, without widening the class's own public surface for it.
    friend class TimelineGraphViewFriendAccess;
    // What a press on a clip rectangle is about to do, decided by where inside
    // the rectangle the press landed.
    enum class DragKind { None, Move, TrimStart, TrimEnd };

    struct DragState {
        // Captured at press time, so every intermediate paint frame — and a
        // cancelled/rejected drag — has an exact reference to compute the live
        // delta from and to repaint from once the drag ends.
        DragKind kind = DragKind::None;
        ClipId   clipId;
        Duration originalStart;
        Duration originalSourceIn;
        Duration originalSourceOut;
        int      pressX = 0;
        Duration liveDelta;  ///< The in-progress delta; meaningful only while dragging.
    };

    // --- Coordinate mapping --------------------------------------------------
    [[nodiscard]] int xForDuration(Duration d) const noexcept;
    [[nodiscard]] Duration durationForX(int x) const noexcept;
    [[nodiscard]] int laneTop(std::size_t trackRow) const noexcept;
    [[nodiscard]] std::optional<std::size_t> laneAtY(int y) const noexcept;

    // --- Hit testing ----------------------------------------------------------
    struct ClipHit {
        std::size_t trackRow = 0;
        std::size_t clipColumn = 0;
        DragKind    zone = DragKind::None;  ///< Move (body) or TrimStart/TrimEnd (near an edge).
    };
    [[nodiscard]] std::optional<ClipHit> hitTestClip(QPoint pos) const;

    // --- Selection ------------------------------------------------------------
    void setSelection(std::optional<ClipId> clipId, std::optional<Uuid> trackId);

    // --- Zoom -------------------------------------------------------------------
    void zoomBy(double factor, int pivotX);

    TimelineViewModel& viewModel_;

    double   pixelsPerSecond_ = 60.0;  ///< horizontal zoom; clamped to a sane range
    Duration scrollOffset_;             ///< timeline position at the widget's left edge
    Duration playhead_;                 ///< last position setPlayhead() was told (paint only)

    std::optional<ClipId> selectedClip_;
    std::optional<Uuid>   selectedTrack_;

    std::optional<DragState> drag_;

    static constexpr int kRulerHeight = 24;
    static constexpr int kLaneHeight = 56;
    static constexpr int kLaneSpacing = 4;
    static constexpr int kEdgeGrabPx = 6;
    static constexpr double kMinPixelsPerSecond = 1.0;
    static constexpr double kMaxPixelsPerSecond = 2000.0;
};

/// Test-only access to TimelineGraphView's protected event handlers (see the
/// friend declaration above). Deliberately declared here rather than in the
/// test file, since only a class actually named as a friend can reach across
/// the access boundary — the test itself does not need to derive from
/// QWidget or duplicate any Qt event-handling code, only to forward a call.
/// All three handlers are reached directly rather than through
/// QCoreApplication::sendEvent(), for one uniform, deterministic delivery path
/// across the whole press/move/release sequence a drag test drives.
class TimelineGraphViewFriendAccess {
public:
    static void sendMousePress(TimelineGraphView* view, QMouseEvent* event) {
        view->mousePressEvent(event);
    }
    static void sendMouseMove(TimelineGraphView* view, QMouseEvent* event) {
        view->mouseMoveEvent(event);
    }
    static void sendMouseRelease(TimelineGraphView* view, QMouseEvent* event) {
        view->mouseReleaseEvent(event);
    }
};

}  // namespace palmier::ui

#endif  // PALMIER_HAVE_QT

#endif  // PALMIER_UI_TIMELINEGRAPHVIEW_HPP
