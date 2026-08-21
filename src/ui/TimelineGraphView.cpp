// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/TimelineGraphView.cpp — implementation of the graphical timeline view
// (task 11.1-11.6).
//
// Compiled only when Qt 6 is available (PALMIER_HAVE_QT).

#include "ui/TimelineGraphView.hpp"

#ifdef PALMIER_HAVE_QT

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QString>
#include <QWheelEvent>

#include "core/EditCommands.hpp"
#include "ui/TimelineViewModel.hpp"

namespace palmier::ui {

namespace {

// The trailing filename of a source path, for a short on-rectangle label. An
// empty or slash-free path is returned as-is.
QString shortLabel(const std::string& sourcePath) {
    const std::size_t slash = sourcePath.find_last_of("/\\");
    const std::string name = slash == std::string::npos ? sourcePath : sourcePath.substr(slash + 1);
    return QString::fromStdString(name);
}

}  // namespace

TimelineGraphView::TimelineGraphView(TimelineViewModel& viewModel, QWidget* parent)
    : QWidget(parent), viewModel_(viewModel) {
    setMouseTracking(true);
    setMinimumHeight(kRulerHeight + kLaneHeight + kLaneSpacing);
    setFocusPolicy(Qt::ClickFocus);
}

TimelineGraphView::~TimelineGraphView() = default;

// ---------------------------------------------------------------------------
// Coordinate mapping (Requirement 8.1)
// ---------------------------------------------------------------------------

int TimelineGraphView::xForDuration(Duration d) const noexcept {
    const double seconds = (d - scrollOffset_).seconds();
    return static_cast<int>(std::lround(seconds * pixelsPerSecond_));
}

Duration TimelineGraphView::durationForX(int x) const noexcept {
    const double seconds = static_cast<double>(x) / pixelsPerSecond_;
    return scrollOffset_ + Duration::fromNanoseconds(
                               static_cast<std::int64_t>(seconds * Duration::kTicksPerSecond));
}

int TimelineGraphView::laneTop(std::size_t trackRow) const noexcept {
    return kRulerHeight + static_cast<int>(trackRow) * (kLaneHeight + kLaneSpacing);
}

std::optional<std::size_t> TimelineGraphView::laneAtY(int y) const noexcept {
    if (y < kRulerHeight) {
        return std::nullopt;
    }
    const int relative = y - kRulerHeight;
    const int stride = kLaneHeight + kLaneSpacing;
    const std::size_t row = static_cast<std::size_t>(relative / stride);
    if (relative % stride >= kLaneHeight) {
        return std::nullopt;  // the inter-lane gap, not a lane
    }
    if (row >= viewModel_.trackCount()) {
        return std::nullopt;
    }
    return row;
}

// ---------------------------------------------------------------------------
// Hit testing
// ---------------------------------------------------------------------------

std::optional<TimelineGraphView::ClipHit> TimelineGraphView::hitTestClip(QPoint pos) const {
    const std::optional<std::size_t> row = laneAtY(pos.y());
    if (!row) {
        return std::nullopt;
    }
    const std::size_t clipCount = viewModel_.clipCount(*row);
    for (std::size_t column = 0; column < clipCount; ++column) {
        const std::optional<ClipView> clip = viewModel_.clipAt(*row, column);
        if (!clip) {
            continue;
        }
        const int left = xForDuration(clip->timelineStart);
        const int right = xForDuration(clip->timelineEnd());
        if (pos.x() < left || pos.x() > right) {
            continue;
        }
        ClipHit hit{*row, column, DragKind::Move};
        if (pos.x() - left <= kEdgeGrabPx) {
            hit.zone = DragKind::TrimStart;
        } else if (right - pos.x() <= kEdgeGrabPx) {
            hit.zone = DragKind::TrimEnd;
        }
        return hit;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Selection (Requirement 8.7)
// ---------------------------------------------------------------------------

void TimelineGraphView::setSelection(std::optional<ClipId> clipId, std::optional<Uuid> trackId) {
    const bool clipChanged = clipId != selectedClip_;
    selectedClip_ = clipId;
    selectedTrack_ = trackId;
    update();
    if (clipChanged) {
        if (selectedClip_) {
            emit clipSelected(QString::fromStdString(selectedClip_->toString()));
        } else {
            emit selectionCleared();
        }
    }
    emit placementTrackChanged();
}

void TimelineGraphView::selectTrack(Uuid trackId) {
    setSelection(std::nullopt, trackId);
}

void TimelineGraphView::clearSelection() {
    setSelection(std::nullopt, std::nullopt);
}

void TimelineGraphView::refresh() {
    // A clip that disappeared from under an active selection (deleted from any
    // surface — this GUI, the MCP endpoint, or the agent) is reported as
    // cleared rather than left pointing at nothing, mirroring
    // TimelinePanel::reconcileSelection()'s existing rule for the tree it
    // replaces.
    if (selectedClip_ && !viewModel_.locate(*selectedClip_)) {
        setSelection(std::nullopt, selectedTrack_);
    }
    update();
}

// ---------------------------------------------------------------------------
// Painting (Requirements 8.1, 8.2, 8.7)
// ---------------------------------------------------------------------------

void TimelineGraphView::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);

    const QColor background = palette().color(QPalette::Base);
    painter.fillRect(rect(), background);

    // --- Ruler (Requirement 8.2): a tick and a timecode label roughly every
    // 80 pixels, at a "nice" (1/2/5 * 10^n seconds) interval for the current
    // zoom, so the label density stays readable at every pixelsPerSecond_.
    const QColor rulerColor = palette().color(QPalette::Mid);
    painter.fillRect(QRect(0, 0, width(), kRulerHeight), palette().color(QPalette::AlternateBase));
    painter.setPen(rulerColor);

    double niceStep = 1.0;
    {
        const double targetSeconds = 80.0 / std::max(pixelsPerSecond_, 0.0001);
        static const double steps[] = {1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1800, 3600};
        niceStep = steps[std::size(steps) - 1];
        for (double candidate : steps) {
            if (candidate >= targetSeconds) {
                niceStep = candidate;
                break;
            }
        }
    }
    const double firstTickSeconds =
        std::floor(scrollOffset_.seconds() / niceStep) * niceStep;
    for (double t = firstTickSeconds; xForDuration(Duration::fromNanoseconds(static_cast<std::int64_t>(
                                          t * Duration::kTicksPerSecond))) <= width();
         t += niceStep) {
        if (t < 0) continue;
        const int x = xForDuration(
            Duration::fromNanoseconds(static_cast<std::int64_t>(t * Duration::kTicksPerSecond)));
        painter.drawLine(x, kRulerHeight - 6, x, kRulerHeight);
        const int totalSeconds = static_cast<int>(std::lround(t));
        const int m = (totalSeconds / 60) % 60;
        const int s = totalSeconds % 60;
        const int h = totalSeconds / 3600;
        const QString label = h > 0
            ? QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'))
            : QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QChar('0'));
        painter.drawText(x + 2, kRulerHeight - 8, label);
    }

    // --- Lanes and clips (Requirement 8.1) ---------------------------------
    const std::size_t tracks = viewModel_.trackCount();
    for (std::size_t row = 0; row < tracks; ++row) {
        const int top = laneTop(row);
        const std::optional<TrackRow> track = viewModel_.trackAt(row);
        const bool laneSelected =
            !selectedClip_.has_value() && selectedTrack_.has_value() && track &&
            *selectedTrack_ == track->id;
        const QColor laneColor = laneSelected
            ? palette().color(QPalette::Highlight).lighter(180)
            : (row % 2 == 0 ? palette().color(QPalette::Base)
                            : palette().color(QPalette::AlternateBase));
        painter.fillRect(QRect(0, top, width(), kLaneHeight), laneColor);

        const std::size_t clipCount = viewModel_.clipCount(row);
        for (std::size_t column = 0; column < clipCount; ++column) {
            const std::optional<ClipView> clip = viewModel_.clipAt(row, column);
            if (!clip) {
                continue;
            }
            Duration start = clip->timelineStart;
            Duration end = clip->timelineEnd();
            // While this exact clip is being dragged, paint its LIVE (not yet
            // committed) position/extent instead of the model's — the model
            // only changes once the gesture is actually applied on release.
            if (drag_ && drag_->clipId == clip->id) {
                switch (drag_->kind) {
                    case DragKind::Move:
                        start = drag_->originalStart + drag_->liveDelta;
                        end = start + clip->duration;
                        break;
                    case DragKind::TrimStart:
                        start = clip->timelineStart + drag_->liveDelta;
                        end = clip->timelineEnd();
                        break;
                    case DragKind::TrimEnd:
                        start = clip->timelineStart;
                        end = clip->timelineEnd() + drag_->liveDelta;
                        break;
                    case DragKind::None:
                        break;
                }
            }
            const int left = xForDuration(start);
            const int right = xForDuration(end);
            if (right < 0 || left > width()) {
                continue;  // entirely off-screen; skip the paint work
            }
            const QRect clipRect(left, top + 2, std::max(right - left, 2), kLaneHeight - 4);

            const bool selected = selectedClip_.has_value() && *selectedClip_ == clip->id;
            QColor fill = selected ? palette().color(QPalette::Highlight)
                                   : palette().color(QPalette::Button);
            painter.fillRect(clipRect, fill);
            painter.setPen(selected ? palette().color(QPalette::HighlightedText)
                                    : palette().color(QPalette::Text));
            painter.drawRect(clipRect);

            const QString label = shortLabel(clip->assetRef.sourcePath);
            if (!label.isEmpty() && clipRect.width() > 12) {
                const QFontMetrics metrics(painter.font());
                painter.drawText(
                    clipRect.adjusted(4, 0, -4, 0), Qt::AlignVCenter | Qt::AlignLeft,
                    metrics.elidedText(label, Qt::ElideRight, clipRect.width() - 8));
            }
        }
    }

    // --- Playhead marker (Requirement 8.2) ---------------------------------
    const int playheadX = xForDuration(playhead_);
    if (playheadX >= 0 && playheadX <= width()) {
        painter.setPen(QPen(QColor(220, 30, 30), 2));
        painter.drawLine(playheadX, 0, playheadX, height());
    }
}

void TimelineGraphView::setPlayhead(Duration position) {
    if (position == playhead_) {
        return;
    }
    playhead_ = position;
    update();
}

// ---------------------------------------------------------------------------
// Mouse interaction (Requirements 8.3, 8.5, 8.6, 8.7)
// ---------------------------------------------------------------------------

void TimelineGraphView::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        return;
    }
    const std::optional<ClipHit> hit = hitTestClip(event->pos());
    if (!hit) {
        // Requirement 8.3: a click on the ruler or an empty part of a lane
        // moves the playhead. A lane click also selects that lane as the
        // placement target (or clears the selection entirely if the click
        // landed below every lane), mirroring what selecting the track's tree
        // row used to mean.
        const std::optional<std::size_t> row = laneAtY(event->pos().y());
        if (row) {
            const std::optional<TrackRow> track = viewModel_.trackAt(*row);
            setSelection(std::nullopt, track ? std::make_optional(track->id) : std::nullopt);
        } else if (event->pos().y() >= kRulerHeight) {
            clearSelection();
        }
        emit seekRequested(durationForX(event->pos().x()).milliseconds());
        return;
    }

    const std::optional<ClipView> clip = viewModel_.clipAt(hit->trackRow, hit->clipColumn);
    if (!clip) {
        return;
    }
    const std::optional<TrackRow> track = viewModel_.trackAt(hit->trackRow);
    setSelection(clip->id, track ? std::make_optional(track->id) : std::nullopt);

    DragState state;
    state.kind = hit->zone;
    state.clipId = clip->id;
    state.originalStart = clip->timelineStart;
    state.originalSourceIn = clip->sourceIn;
    state.originalSourceOut = clip->sourceOut;
    state.pressX = event->pos().x();
    drag_ = state;
}

void TimelineGraphView::mouseMoveEvent(QMouseEvent* event) {
    if (!drag_ || drag_->kind == DragKind::None) {
        return;
    }
    const int deltaPx = event->pos().x() - drag_->pressX;
    const double deltaSeconds = static_cast<double>(deltaPx) / pixelsPerSecond_;
    drag_->liveDelta = Duration::fromNanoseconds(
        static_cast<std::int64_t>(deltaSeconds * Duration::kTicksPerSecond));
    update();
}

void TimelineGraphView::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !drag_) {
        return;
    }
    const DragState state = *drag_;
    drag_.reset();

    if (state.liveDelta.isZero()) {
        update();
        return;  // a press-and-release with no movement is a click, not a drag
    }

    const FrameRate fps = viewModel_.project().timelineFps;
    switch (state.kind) {
        case DragKind::Move: {
            // Requirement 8.5: GestureResult::InvalidDrop (an overlap or a
            // negative destination) leaves the clip retained at its ORIGINAL
            // position in the model, which is exactly what gets painted again
            // once drag_ above is cleared and this repaints from the model
            // instead of a live delta — the "visual revert" the requirement
            // asks for falls out of painting the model's own state rather than
            // needing an explicit undo of anything.
            (void)viewModel_.moveClip(state.clipId, state.originalStart + state.liveDelta);
            break;
        }
        case DragKind::TrimStart: {
            const Duration newSourceIn = state.originalSourceIn + state.liveDelta;
            (void)viewModel_.trimClipStart(state.clipId, newSourceIn, fps,
                                          state.originalSourceOut);
            break;
        }
        case DragKind::TrimEnd: {
            const Duration newSourceOut = state.originalSourceOut + state.liveDelta;
            (void)viewModel_.trimClipEnd(state.clipId, newSourceOut, fps,
                                        state.originalSourceOut);
            break;
        }
        case DragKind::None:
            break;
    }
    update();  // the engine's own change notification also triggers refresh(),
               // but this repaints immediately rather than waiting on that signal
}

// ---------------------------------------------------------------------------
// Zoom (Requirement 8.4)
// ---------------------------------------------------------------------------

void TimelineGraphView::zoomBy(double factor, int pivotX) {
    // Keep the timeline position currently under `pivotX` fixed on screen
    // across the zoom change: recompute scrollOffset_ so that duration-at-pivot
    // still maps back to pivotX at the NEW pixelsPerSecond_.
    const Duration atPivot = durationForX(pivotX);
    pixelsPerSecond_ =
        std::clamp(pixelsPerSecond_ * factor, kMinPixelsPerSecond, kMaxPixelsPerSecond);
    const double pivotSeconds = static_cast<double>(pivotX) / pixelsPerSecond_;
    scrollOffset_ = atPivot - Duration::fromNanoseconds(
                                 static_cast<std::int64_t>(pivotSeconds * Duration::kTicksPerSecond));
    if (scrollOffset_.isNegative()) {
        scrollOffset_ = Duration::zero();
    }
    update();
}

void TimelineGraphView::wheelEvent(QWheelEvent* event) {
    if ((event->modifiers() & Qt::ControlModifier) == 0) {
        event->ignore();
        return;
    }
    // Requirement 8.4: zoom while keeping the PLAYHEAD visible across the
    // change, not just whatever was under the cursor — pivoting on the
    // playhead's own current pixel position is what "keeps the playhead
    // visible" means when the playhead itself may be off-screen (in which
    // case pivoting on the cursor could zoom it further out of view).
    const int pivotX = xForDuration(playhead_);
    const double factor = event->angleDelta().y() > 0 ? 1.25 : 0.8;
    zoomBy(factor, pivotX);
    event->accept();
}

void TimelineGraphView::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    update();
}

}  // namespace palmier::ui

#endif  // PALMIER_HAVE_QT
