// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/MainWindow.hpp — the Qt 6 editor main window (application shell).
//
// This is the top-level editor window shown on a supported host once the
// launch-time platform compatibility check has passed (Requirements 1.3). The
// panels it will host — timeline, preview/player, inspector, media browser, and
// agent chat — are filled in by tasks 19.2-19.6; this task provides the shell
// itself so the editor can appear within the launch budget.
//
// The window is deliberately thin: it contains NO platform-decision logic. The
// entry point (src/app/main.cpp) runs the Qt-free PlatformCompatibility checker
// and only constructs a MainWindow when the host is compatible, so an
// unsupported platform or a missing dependency never reaches this class
// (Requirements 1.4, 1.5). Constructing and showing the window performs no
// network activity, honoring "start without a network connection" (13.3/13.4).
//
// The entire translation unit is guarded by PALMIER_HAVE_QT (mirroring the
// project's PALMIER_HAVE_VULKAN / PALMIER_HAVE_FFMPEG guard style) so the module
// tree still configures and builds where Qt 6 is not installed; the compiled
// window is produced only when Qt is found.

#ifndef PALMIER_UI_MAINWINDOW_HPP
#define PALMIER_UI_MAINWINDOW_HPP

#ifdef PALMIER_HAVE_QT

#include <QMainWindow>

class QLabel;

namespace palmier::ui {

/// The editor's top-level window. Later UI tasks (19.2-19.6) dock the timeline,
/// preview, inspector, media browser, and agent-chat panels into it.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private:
    void buildMenus();
    void buildCentralPlaceholder();

    QLabel* placeholder_ = nullptr;
};

} // namespace palmier::ui

#endif // PALMIER_HAVE_QT

#endif // PALMIER_UI_MAINWINDOW_HPP
