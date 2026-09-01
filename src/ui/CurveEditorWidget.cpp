// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/CurveEditorWidget.cpp — implementation of the Qt 6 tone-curve surface.

#include "ui/CurveEditorWidget.hpp"

#ifdef PALMIER_HAVE_QT

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

#include <QColor>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPointF>
#include <QRect>
#include <QSize>

namespace palmier::ui {
namespace {

constexpr int    kPreferredSide = 180;
constexpr int    kMinimumSide = 90;
constexpr double kPointRadiusPx = 3.5;

const QColor kBackgroundColour{0x1a, 0x1a, 0x1a};
const QColor kGridColour{0x33, 0x33, 0x33};
const QColor kDiagonalColour{0x44, 0x44, 0x44};
const QColor kCurveColour{0xe8, 0xe8, 0xe8};
const QColor kPointColour{0x3d, 0xa5, 0x4a};
const QColor kDraggedPointColour{0xf0, 0xc0, 0x40};

}  // namespace

CurveEditorWidget::CurveEditorWidget(QWidget* parent) : QWidget(parent) {
    setMouseTracking(false);  // moves only matter while a button is held
    setFocusPolicy(Qt::ClickFocus);
}

CurveEditorWidget::~CurveEditorWidget() = default;

void CurveEditorWidget::setRequestApplier(RequestApplier applier) {
    applier_ = std::move(applier);
}

void CurveEditorWidget::setChannel(CurveChannel channel) {
    if (channel == model_.channel()) {
        return;
    }
    model_.setChannel(channel);
    // The points belong to the OLD channel, so they must not be shown against the new
    // one. Clearing rather than keeping them means the control reads as empty until the
    // owner supplies the new channel's parameters, which is honest; keeping them would
    // show one channel's curve labelled as another's.
    model_.setPoints(std::vector<CurvePoint>{});
    update();
}

void CurveEditorWidget::setParameters(const std::map<std::string, double>& parameters) {
    model_.setPoints(parameters);
    update();
}

QSize CurveEditorWidget::minimumSizeHint() const { return QSize{kMinimumSide, kMinimumSide}; }

QSize CurveEditorWidget::sizeHint() const { return QSize{kPreferredSide, kPreferredSide}; }

void CurveEditorWidget::apply(const CurveEditRequest& request) {
    update();  // the working copy moved, so repaint whether or not it is committed
    if (request.isNoOp() || !applier_) {
        return;
    }
    applier_(model_.channel(), request);
}

void CurveEditorWidget::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    const double w = static_cast<double>(width());
    const double h = static_cast<double>(height());
    painter.fillRect(rect(), kBackgroundColour);
    if (w <= 0.0 || h <= 0.0) {
        return;
    }

    // Quarter grid and the identity diagonal, so a curve is readable as a DEPARTURE from
    // no-op rather than as an absolute shape.
    painter.setPen(QPen(kGridColour, 1));
    for (int i = 1; i < 4; ++i) {
        const double f = static_cast<double>(i) / 4.0;
        painter.drawLine(QPointF{f * w, 0.0}, QPointF{f * w, h});
        painter.drawLine(QPointF{0.0, f * h}, QPointF{w, f * h});
    }
    painter.setPen(QPen(kDiagonalColour, 1));
    painter.drawLine(QPointF{0.0, h}, QPointF{w, 0.0});

    // The transfer function, one column per table entry. This is bakeToneCurve's own
    // table, so what is drawn is what the renderer applies rather than a second plot of
    // the same points.
    const ToneCurveTable table = model_.transferTable();
    painter.setPen(QPen(kCurveColour, 2));
    QPointF previous{0.0, h - static_cast<double>(table[0]) / 255.0 * h};
    for (std::size_t i = 1; i < table.size(); ++i) {
        const double x = static_cast<double>(i) / 255.0 * w;
        const double y = h - static_cast<double>(table[i]) / 255.0 * h;
        const QPointF current{x, y};
        painter.drawLine(previous, current);
        previous = current;
    }

    // The control points on top, with the dragged one distinct so the user can see which
    // point they actually grabbed -- the common failure of a curve control is grabbing a
    // neighbour without noticing.
    const std::optional<std::size_t> dragged = model_.draggedIndex();
    const std::vector<CurvePoint>&   points = model_.points();
    for (std::size_t i = 0; i < points.size(); ++i) {
        const CurvePixel at = model_.toPixel(points[i], w, h);
        const bool isDragged = dragged.has_value() && *dragged == i;
        painter.setBrush(isDragged ? kDraggedPointColour : kPointColour);
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(QPointF{at.x, at.y}, kPointRadiusPx, kPointRadiusPx);
    }
}

void CurveEditorWidget::mousePressEvent(QMouseEvent* event) {
    const CurvePixel at{event->position().x(), event->position().y()};
    const double w = static_cast<double>(width());
    const double h = static_cast<double>(height());

    // The right button removes. A modifier would be discoverable only by accident, and a
    // double-click would make every deliberate removal race the double-click interval.
    // A removal IS committed immediately, because a single click is the whole gesture.
    if (event->button() == Qt::RightButton) {
        apply(model_.remove(at, w, h));
        return;
    }
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    // Nothing is committed on press: the view model's release() is the single commit
    // point, so click-drag-to-place is one undoable edit rather than an add then a move.
    (void)model_.press(at, w, h);
    update();
}

void CurveEditorWidget::mouseMoveEvent(QMouseEvent* event) {
    if (!model_.isDragging()) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    // Moves only advance the working copy so the drag is visible. Committing each one
    // would put a history entry on every mouse move, and undoing a single drag would then
    // take dozens of presses.
    (void)model_.drag(CurvePixel{event->position().x(), event->position().y()},
                      static_cast<double>(width()), static_cast<double>(height()));
    update();
}

void CurveEditorWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    // The release is the commit, and it carries the final position -- so the whole
    // gesture is one undoable edit (Requirement 5.7).
    apply(model_.release());
}

}  // namespace palmier::ui

#endif  // PALMIER_HAVE_QT
