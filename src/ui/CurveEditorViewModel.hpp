// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/CurveEditorViewModel.hpp — the Qt-free logic behind the Inspector's tone-curve
// control (monitoring-and-grading task 5; Requirement 5.9).
//
// Requirement 5.9 asks for a control the user edits DIRECTLY — dragging a point on the
// transfer function, not typing numbers into ten spin boxes. Direct manipulation is
// mostly arithmetic: mapping a pixel to a curve coordinate and back, deciding which
// point (if any) the pointer is over, and turning a press-drag-release into exactly one
// undoable edit. None of that needs Qt, and all of it is where the mistakes live.
//
// So it lives here, and ui::CurveEditorWidget only paints and forwards mouse events —
// the same split as ui::AudioMeterViewModel and ui::AudioMeterWidget, for the same
// reason: this file is tested on a runner with no display, no Qt and no GPU.
//
// The view model NEVER edits a project. It answers "what edit does this gesture ask
// for?" and the caller performs it, because performing it means issuing an undoable
// command through the tool surface and this type has no business knowing about either.
// That also keeps a drag from producing one history entry per mouse move: the caller
// decides that only the release commits.

#ifndef PALMIER_UI_CURVEEDITORVIEWMODEL_HPP
#define PALMIER_UI_CURVEEDITORVIEWMODEL_HPP

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/EditCommands.hpp"
#include "core/ToneCurve.hpp"

namespace palmier::ui {

/// A point in widget pixels, with y measured DOWNWARD as every raster surface does.
struct CurvePixel {
    double x = 0.0;
    double y = 0.0;
};

/// The edit a gesture asks for, or None.
///
/// Shaped to match core::EditCurvePointCommand exactly, so the caller translates rather
/// than interprets. `index` is meaningless for Add and `point` for Remove, matching the
/// command's own contract.
struct CurveEditRequest {
    enum class Kind { None, Add, Move, Remove };

    Kind        kind = Kind::None;
    std::size_t index = 0;
    CurvePoint  point{};

    [[nodiscard]] bool isNoOp() const noexcept { return kind == Kind::None; }

    /// The matching command operation. Only valid when this is not a no-op.
    [[nodiscard]] EditCurvePointCommand::Operation operation() const noexcept;
};

/// Direct manipulation of one channel's control points.
///
/// Holds a working copy of the points so a drag can be shown live without a project
/// edit per mouse move. setPoints() re-synchronises it from the project, which is what
/// makes an external change — an undo, or an edit over MCP — appear here.
class CurveEditorViewModel {
public:
    /// How close, in pixels, the pointer must be to grab an existing point.
    ///
    /// Generous on purpose: a control point is drawn a few pixels across and a user
    /// aiming at one and missing by two would otherwise ADD a point on top of it, which
    /// looks like the drag simply failed while quietly changing the curve.
    static constexpr double kGrabRadiusPx = 8.0;

    /// The channel being edited. Only this channel's points are read or written.
    void setChannel(CurveChannel channel) noexcept { channel_ = channel; }
    [[nodiscard]] CurveChannel channel() const noexcept { return channel_; }

    /// Replace the working copy from an effect's parameters.
    void setPoints(const std::map<std::string, double>& parameters);
    /// Replace the working copy directly (for tests and for a live drag).
    void setPoints(std::vector<CurvePoint> points);
    [[nodiscard]] const std::vector<CurvePoint>& points() const noexcept { return points_; }

    /// The 256-entry transfer function the working copy describes, for painting.
    ///
    /// Deliberately the SAME core::bakeToneCurve the renderer uses, so the curve drawn
    /// is the curve applied. A separate plotting routine here would be a second
    /// implementation of the interpolation, free to disagree with the image.
    [[nodiscard]] ToneCurveTable transferTable() const;

    // --- Coordinate mapping ------------------------------------------------

    /// Curve coordinate (x,y in [0,1]) to widget pixel. y is FLIPPED: a curve output of
    /// 1.0 is drawn at the top, which is how every curve control is read.
    [[nodiscard]] CurvePixel toPixel(CurvePoint point, double width, double height) const noexcept;

    /// Widget pixel back to a curve coordinate, clamped into [0,1] on both axes so a
    /// drag that leaves the widget pins the point to the edge rather than producing a
    /// coordinate the curve cannot hold.
    [[nodiscard]] CurvePoint toCurve(CurvePixel pixel, double width, double height) const noexcept;

    /// The index of the point under `pixel`, or nullopt. When several are within the
    /// grab radius the NEAREST wins, so overlapping points remain individually
    /// reachable instead of the first-added one always capturing the pointer.
    [[nodiscard]] std::optional<std::size_t> pointAt(CurvePixel pixel, double width,
                                                    double height) const noexcept;

    // --- Gestures ----------------------------------------------------------
    //
    // release() is the ONLY commit point for a press-drag-release, which is what makes
    // "one gesture, one undoable edit" (Requirement 5.7) a property of this class rather
    // than of whichever widget drives it. press() and drag() therefore never return an
    // edit: they only advance the working copy so the gesture can be drawn.

    /// Begin a gesture: grab the point under the pointer, or begin adding one there.
    ///
    /// Always a no-op request. A new point appears in the working copy immediately so it
    /// can be dragged and drawn, but it is not committed until release() -- otherwise
    /// clicking and dragging to place a point would be TWO undo entries (an add, then a
    /// move), and undoing what the user experienced as one action would take two presses.
    CurveEditRequest press(CurvePixel pixel, double width, double height);

    /// Continue a gesture, moving the grabbed point in the working copy.
    ///
    /// Always a no-op request, for the reason above: committing here would put a history
    /// entry on every mouse move.
    CurveEditRequest drag(CurvePixel pixel, double width, double height);

    /// End a gesture and commit it as exactly one edit.
    ///
    /// An Add when the gesture began on empty space -- carrying the FINAL position, so
    /// the point is created where it was released rather than where the press landed --
    /// a Move when an existing point actually moved, and None when a point was grabbed
    /// and released without moving.
    [[nodiscard]] CurveEditRequest release();

    /// Ask to remove the point under `pixel` (a right-click or double-click, decided by
    /// the widget). None when the pointer is not on a point, and never a removal that
    /// would leave the curve unable to describe itself.
    [[nodiscard]] CurveEditRequest remove(CurvePixel pixel, double width, double height);

    [[nodiscard]] bool isDragging() const noexcept { return dragging_.has_value(); }
    /// The point being dragged, for drawing it highlighted.
    [[nodiscard]] std::optional<std::size_t> draggedIndex() const noexcept { return dragging_; }

private:
    CurveChannel               channel_ = CurveChannel::Master;
    std::vector<CurvePoint>    points_;
    std::optional<std::size_t> dragging_;
    /// Where the dragged point started, so release() can tell a real drag from a click.
    CurvePoint                 dragOrigin_{};
    bool                       moved_ = false;
    /// Whether this gesture created the point it is dragging, so release() commits an Add
    /// rather than a Move. Without it, the add would have to be committed on press and
    /// the gesture would cost two undo entries.
    bool                       adding_ = false;
};

}  // namespace palmier::ui

#endif  // PALMIER_UI_CURVEEDITORVIEWMODEL_HPP
