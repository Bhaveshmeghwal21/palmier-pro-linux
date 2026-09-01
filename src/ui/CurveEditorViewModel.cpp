// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/CurveEditorViewModel.cpp — see the header for why this logic is Qt-free.

#include "ui/CurveEditorViewModel.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace palmier::ui {

namespace {

/// Clamp into the unit range. Curve coordinates are normalised on both axes, so a
/// pointer outside the widget pins to the edge instead of describing an unreachable
/// point.
double clamp01(double v) noexcept {
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

}  // namespace

EditCurvePointCommand::Operation CurveEditRequest::operation() const noexcept {
    switch (kind) {
        case Kind::Add:    return EditCurvePointCommand::Operation::Add;
        case Kind::Move:   return EditCurvePointCommand::Operation::Move;
        case Kind::Remove: return EditCurvePointCommand::Operation::Remove;
        case Kind::None:   break;
    }
    return EditCurvePointCommand::Operation::Add;
}

void CurveEditorViewModel::setPoints(const std::map<std::string, double>& parameters) {
    setPoints(curvePoints(parameters, channel_));
}

void CurveEditorViewModel::setPoints(std::vector<CurvePoint> points) {
    points_ = std::move(points);
    // An external change invalidates any gesture in progress: the index being dragged
    // may no longer mean the same point, or may no longer exist. Dropping the gesture is
    // the only safe answer — continuing it would move whichever point now holds that
    // index, which is a wrong edit that looks like a successful one.
    dragging_.reset();
    moved_ = false;
}

ToneCurveTable CurveEditorViewModel::transferTable() const {
    return bakeToneCurve(points_);
}

CurvePixel CurveEditorViewModel::toPixel(CurvePoint point, double width,
                                        double height) const noexcept {
    return CurvePixel{point.x * width, (1.0 - point.y) * height};
}

CurvePoint CurveEditorViewModel::toCurve(CurvePixel pixel, double width,
                                        double height) const noexcept {
    // A zero-sized widget has no meaningful mapping. Answering (0,0) rather than
    // dividing keeps a transient layout state from producing NaN coordinates that would
    // then be written into the project.
    if (width <= 0.0 || height <= 0.0) {
        return CurvePoint{0.0, 0.0};
    }
    return CurvePoint{clamp01(pixel.x / width), clamp01(1.0 - pixel.y / height)};
}

std::optional<std::size_t> CurveEditorViewModel::pointAt(CurvePixel pixel, double width,
                                                        double height) const noexcept {
    std::optional<std::size_t> best;
    double bestDistanceSq = kGrabRadiusPx * kGrabRadiusPx;
    for (std::size_t i = 0; i < points_.size(); ++i) {
        const CurvePixel at = toPixel(points_[i], width, height);
        const double dx = at.x - pixel.x;
        const double dy = at.y - pixel.y;
        const double distanceSq = dx * dx + dy * dy;
        // <= so a point exactly on the radius is grabbable, and strictly-nearer wins so
        // the result does not depend on insertion order.
        if (distanceSq <= bestDistanceSq) {
            bestDistanceSq = distanceSq;
            best = i;
        }
    }
    return best;
}

CurveEditRequest CurveEditorViewModel::press(CurvePixel pixel, double width, double height) {
    const CurvePoint where = toCurve(pixel, width, height);
    if (const std::optional<std::size_t> hit = pointAt(pixel, width, height)) {
        dragging_ = *hit;
        dragOrigin_ = points_[*hit];
        moved_ = false;
        // Grabbing an existing point is not itself an edit. Reporting a Move here would
        // put a history entry on every click, including one that changes nothing.
        return CurveEditRequest{};
    }
    // A new point appended: the working copy gains it immediately so the following moves
    // have something to drag, and its index is the one the command will also give it.
    points_.push_back(where);
    dragging_ = points_.size() - 1;
    dragOrigin_ = where;
    moved_ = false;
    return CurveEditRequest{CurveEditRequest::Kind::Add, dragging_.value(), where};
}

CurveEditRequest CurveEditorViewModel::drag(CurvePixel pixel, double width, double height) {
    if (!dragging_) {
        return CurveEditRequest{};
    }
    const CurvePoint where = toCurve(pixel, width, height);
    points_[*dragging_] = where;
    moved_ = true;
    return CurveEditRequest{CurveEditRequest::Kind::Move, *dragging_, where};
}

CurveEditRequest CurveEditorViewModel::release() {
    if (!dragging_) {
        return CurveEditRequest{};
    }
    const std::size_t index = *dragging_;
    const CurvePoint  where = points_[index];
    const bool        moved = moved_;
    dragging_.reset();
    moved_ = false;
    if (!moved) {
        // A press with no movement. The point is already where it was, so committing a
        // Move would add an undo entry that undoes to the same state — indistinguishable
        // from a broken undo when the user tries it.
        return CurveEditRequest{};
    }
    return CurveEditRequest{CurveEditRequest::Kind::Move, index, where};
}

CurveEditRequest CurveEditorViewModel::remove(CurvePixel pixel, double width, double height) {
    const std::optional<std::size_t> hit = pointAt(pixel, width, height);
    if (!hit) {
        return CurveEditRequest{};
    }
    // Removing is always allowed, including the last point: an empty curve is the
    // identity (Requirement 5.5), not an invalid state, so there is nothing to protect
    // the user from and refusing would leave them unable to clear a curve they no longer
    // want.
    const CurvePoint removed = points_[*hit];
    points_.erase(points_.begin() + static_cast<std::ptrdiff_t>(*hit));
    dragging_.reset();
    moved_ = false;
    return CurveEditRequest{CurveEditRequest::Kind::Remove, *hit, removed};
}

}  // namespace palmier::ui
