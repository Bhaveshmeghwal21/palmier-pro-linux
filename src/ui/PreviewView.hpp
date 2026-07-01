// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/PreviewView.hpp — the thin Qt 6 preview/player surface (task 19.3;
// Requirements 2.8, 10.7).
//
// This QWidget is intentionally minimal: all playback logic — the playhead
// clock, the >= 24 fps cadence, and the GPU-active / CPU-fallback path selection
// — lives in the Qt-free PreviewController (see PreviewController.hpp), which
// this view owns and drives. The view only (1) runs a QTimer that calls
// PreviewController::pump() at the preview frame rate, (2) receives each
// composited frame through the controller's FrameSink and uploads its RGBA8
// pixels into a QImage, and (3) paints that image scaled into the widget.
//
// The entire translation unit is guarded by PALMIER_HAVE_QT (mirroring the
// project's PALMIER_HAVE_VULKAN / MainWindow guard style) so the module tree
// still configures and builds where Qt 6 is not installed; the compiled surface
// is produced only when Qt is found. The Qt-free PreviewController is always
// buildable and is what the unit tests exercise.

#ifndef PALMIER_UI_PREVIEWVIEW_HPP
#define PALMIER_UI_PREVIEWVIEW_HPP

#ifdef PALMIER_HAVE_QT

#include <memory>

#include <QImage>
#include <QWidget>

#include "gpu/Compositor.hpp"
#include "gpu/GpuContext.hpp"
#include "ui/PreviewController.hpp"

class QTimer;
class QPaintEvent;

namespace palmier::ui {

/// The editor's preview/player widget. Owns a PreviewController and paints the
/// frames it produces. Playback transport is delegated to the controller.
class PreviewView : public QWidget {
    Q_OBJECT

public:
    PreviewView(gpu::Compositor& compositor, const gpu::GpuContext& context,
                PreviewProjectSource projectSource, QWidget* parent = nullptr);
    ~PreviewView() override;

    /// Access the underlying playback engine (transport, path selection, state).
    [[nodiscard]] PreviewController& controller() noexcept { return controller_; }

public slots:
    void play();
    void pause();
    void stop();
    void seekSeconds(double seconds);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void onTimer();
    void uploadFrame(const gpu::RenderedFrame& frame, RenderPath path);

    PreviewController controller_;
    QTimer*           timer_{nullptr};
    QImage            frameImage_{};
};

} // namespace palmier::ui

#endif // PALMIER_HAVE_QT

#endif // PALMIER_UI_PREVIEWVIEW_HPP
