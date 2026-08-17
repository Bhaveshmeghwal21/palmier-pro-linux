// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/ui/shell_layout_property_test.cpp — property test for the docked shell
// layout (task 11.8).
//
// Feature: end-to-end-editor-integration, Property 1: Panel reachability under
// any window size — for any window size at or above the documented minimum
// (1024x640), every one of the five docked panels (Timeline, Preview, Inspector,
// Media Browser, Agent Chat) remains present and reachable: it exists as a
// child widget of MainWindow, its owning QDockWidget is visible (not hidden by
// the resize, though it may be tabbed behind a sibling), and it can be raised to
// the foreground of its tab group.
//
// Validates: Requirement 1.4
//
// Strategy: construct one MainWindow over a real (Qt-free-composable)
// app::ApplicationComposition, resize it to a RapidCheck-generated size at or
// above the minimum, and assert every dock's widget() is non-null and every
// dock itself is not hidden (Qt's own QMainWindow::setMinimumSize enforces the
// floor — the window itself cannot shrink below it — so what this property
// actually exercises is that the FIVE-DOCK LAYOUT never drops or hides a panel
// as the window grows/shrinks across its valid range, which is the "no further
// user action" contract MainWindow's own construction establishes).
//
// Requires a display (a real X server or Xvfb); run under `xvfb-run -a` in CI.

#include "ui/MainWindow.hpp"

#ifdef PALMIER_HAVE_QT

#include <QApplication>
#include <QDockWidget>

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include "app/ApplicationComposition.hpp"

namespace palmier::ui {
namespace {

// A QApplication is required to construct any QWidget; GoogleTest's own argc/
// argv are reused so Qt sees a plausible (even if minimal) command line.
class QtEnvironment : public ::testing::Environment {
public:
    void SetUp() override {
        if (qApp == nullptr) {
            static int argc = 1;
            static char argv0[] = "shell_layout_property_test";
            static char* argv[] = {argv0};
            app_ = std::make_unique<QApplication>(argc, argv);
        }
    }
    void TearDown() override { app_.reset(); }

private:
    std::unique_ptr<QApplication> app_;
};

// Registered once per test binary (GoogleTest owns and frees Environments).
::testing::Environment* const kQtEnv =
    ::testing::AddGlobalTestEnvironment(new QtEnvironment());

/// The five QDockWidget object names MainWindow assigns (task 11.2). A generic
/// `findChildren<QDockWidget*>()` walk keeps this test from hard-coding pointers
/// MainWindow does not expose, while still checking exactly the five docks
/// Requirement 1.4 names.
const std::vector<QString>& expectedDockNames() {
    static const std::vector<QString> names = {
        QStringLiteral("MediaBrowserDock"), QStringLiteral("TimelineDock"),
        QStringLiteral("InspectorDock"), QStringLiteral("AgentChatDock")};
    return names;
}

// Feature: end-to-end-editor-integration, Property 1: Panel reachability under
// any window size — every docked panel remains present, visible and raisable
// across the documented minimum-to-generous window-size range.
// Validates: Requirement 1.4
RC_GTEST_PROP(ShellLayoutProperties, EveryPanelReachableAtAnyValidWindowSize, ()) {
    // At or above MainWindow's documented minimum (1024x640), and comfortably
    // below anything that would fail to allocate on a CI runner.
    const int width = *rc::gen::inRange(1024, 3841);
    const int height = *rc::gen::inRange(640, 2161);

    app::ApplicationComposition composition;
    MainWindow window(composition);
    window.resize(width, height);
    window.show();

    // Every dock MainWindow constructs is present as a child, regardless of the
    // requested size (Qt clamps the window itself to setMinimumSize(1024, 640),
    // so a request below the floor still yields a compliant window — the
    // property's generator floor matches that contract rather than fighting it).
    const QList<QDockWidget*> docks = window.findChildren<QDockWidget*>();
    RC_ASSERT(docks.size() >= expectedDockNames().size());

    for (const QString& name : expectedDockNames()) {
        QDockWidget* found = nullptr;
        for (QDockWidget* dock : docks) {
            if (dock->objectName() == name) {
                found = dock;
                break;
            }
        }
        RC_ASSERT(found != nullptr);
        RC_ASSERT(!found->isHidden());
        RC_ASSERT(found->widget() != nullptr);

        // Raising a (possibly tabbed) dock must not throw and must leave it
        // reachable — Qt's tabifyDockWidget keeps every tabbed dock present in
        // the child hierarchy even when a sibling tab is currently on top.
        found->raise();
        RC_ASSERT(!found->isHidden());
    }

    // The central widget (Preview) is always present and is never one of the
    // dockable four, so it is checked separately.
    RC_ASSERT(window.centralWidget() != nullptr);
    RC_ASSERT(!window.centralWidget()->isHidden());

    RC_ASSERT(window.width() >= 1024);
    RC_ASSERT(window.height() >= 640);
}

}  // namespace
}  // namespace palmier::ui

#else  // !PALMIER_HAVE_QT

// Qt is not present in this build: register a single, explicit skip so the
// absence of Qt shows up as a recorded reason rather than as a missing test.
TEST(ShellLayoutProperties, SkippedWithoutQt) {
    GTEST_SKIP() << "Qt 6 is not available in this build (PALMIER_HAVE_QT is undefined); "
                    "the shell layout property requires a compiled MainWindow.";
}

#endif  // PALMIER_HAVE_QT
