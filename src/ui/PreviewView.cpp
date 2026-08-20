// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/PreviewView.cpp — implementation of the thin Qt 6 preview/player surface.
//
// Compiled only when Qt 6 is available (PALMIER_HAVE_QT). Every decision that
// matters for correctness (cadence, playhead, GPU/CPU path) is made by the
// Qt-free PreviewController; this file is display glue only.

#include "ui/PreviewView.hpp"

#ifdef PALMIER_HAVE_QT

#include <algorithm>
#include <cmath>
#include <utility>

#include <QPainter>
#include <QTimer>

namespace palmier::ui {

PreviewView::PreviewView(gpu::Compositor& compositor, const gpu::GpuContext& context,
                         PreviewProjectSource projectSource, QWidget* parent)
    : QWidget(parent),
      ownedController_(std::in_place, compositor, context, std::move(projectSource)),
      controller_(*ownedController_),
      timer_(new QTimer(this)) {
    setMinimumSize(160, 90);
    wireSinkAndTimer();
}

PreviewView::PreviewView(PreviewController& controller, QWidget* parent)
    : QWidget(parent), ownedController_(std::nullopt), controller_(controller),
      timer_(new QTimer(this)) {
    setMinimumSize(160, 90);
    wireSinkAndTimer();
}

void PreviewView::wireSinkAndTimer() {
    // Upload each composited frame to our QImage for painting.
    controller_.setFrameSink(
        [this](const gpu::RenderedFrame& frame, RenderPath path) { uploadFrame(frame, path); });

    connect(timer_, &QTimer::timeout, this, &PreviewView::onTimer);
}

PreviewView::~PreviewView() = default;

void PreviewView::play() {
    controller_.play();
    // Drive pump() a little faster than the preview rate so we never starve the
    // >= 24 fps cadence; the controller itself decides which frames are actually
    // due, so an eager timer only ever presents on schedule.
    const double fps = std::max(24.0, controller_.previewFps());
    const int intervalMs = std::max(1, static_cast<int>(std::lround(1000.0 / (fps * 2.0))));
    timer_->start(intervalMs);
}

void PreviewView::pause() {
    controller_.pause();
    timer_->stop();
}

void PreviewView::stop() {
    controller_.stop();
    timer_->stop();
    // Repaint one cleared/still frame at the reset position.
    (void)controller_.renderFrame();
    update();
}

void PreviewView::seekSeconds(double seconds) {
    controller_.seek(Duration::fromSeconds(std::max(0.0, seconds)));
    if (!controller_.isPlaying()) {
        (void)controller_.renderFrame();
        update();
    }
}

void PreviewView::onTimer() {
    if (controller_.pump() > 0) {
        update();
    }
    if (!controller_.isPlaying()) {
        timer_->stop();
    }
}

void PreviewView::uploadFrame(const gpu::RenderedFrame& frame, RenderPath /*path*/) {
    const auto* pixels = static_cast<const uchar*>(frame.hostData());
    if (pixels == nullptr || frame.width() == 0 || frame.height() == 0) {
        return;
    }
    // Copy into an owned QImage (the frame's lease is released after the sink
    // returns, so we must not alias its memory).
    frameImage_ = QImage(pixels, static_cast<int>(frame.width()),
                         static_cast<int>(frame.height()), QImage::Format_RGBA8888)
                      .copy();
}

void PreviewView::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);
    if (frameImage_.isNull()) {
        return;
    }
    // Letterbox the frame into the widget preserving aspect ratio.
    const QSize scaled = frameImage_.size().scaled(size(), Qt::KeepAspectRatio);
    const QRect target(QPoint((width() - scaled.width()) / 2,
                              (height() - scaled.height()) / 2),
                       scaled);
    painter.drawImage(target, frameImage_);
}

} // namespace palmier::ui

#endif // PALMIER_HAVE_QT
