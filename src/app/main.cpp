// SPDX-License-Identifier: GPL-3.0-or-later
//
// app/main.cpp — Palmier Pro application entry point (application shell).
//
// Launch sequence (Requirement 1):
//   1. Run the Qt-free platform compatibility check (PlatformCompatibility):
//      verify CPU architecture, glibc >= 2.31, and that each required runtime
//      dependency is loadable. This performs NO network activity, so the editor
//      starts without a network connection (Requirements 1.3, 13.3, 13.4).
//   2. If the host is an unsupported platform (architecture / glibc) or is
//      missing required dependencies, show a message that names EACH unmet item
//      and exit WITHOUT constructing the editor (Requirements 1.4, 1.5).
//   3. Otherwise construct and show the editor main window (Requirement 1.3).
//
// The decision logic lives entirely in the Qt-free checker; this entry point is
// a thin shell around it. The whole file compiles both with and without Qt:
// when Qt 6 is present (PALMIER_HAVE_QT) it uses QApplication + QMessageBox +
// MainWindow; otherwise it degrades to a console shell that still runs the check
// and reports the verdict on stderr, so the application target is buildable and
// the launch gate is exercisable even where Qt is not installed.

#include <iostream>

#include "app/ApplicationComposition.hpp"
#include "app/PlatformCompatibility.hpp"
#include "core/Result.hpp"

#ifdef PALMIER_HAVE_QT
#include <QApplication>
#include <QMessageBox>
#include <QString>

#include "ui/MainWindow.hpp"
#endif

namespace {

using palmier::app::CompatibilityReport;

/// Print the incompatibility report to stderr (used on every build, and as the
/// only channel in a headless build without Qt).
void reportToConsole(const CompatibilityReport& report) {
    std::cerr << report.message() << '\n';
}

} // namespace

int main(int argc, char** argv) {
#ifdef PALMIER_HAVE_QT
    // A QApplication is required before showing any widget (including the message
    // box used for the unsupported/missing-dependency path). Creating it does not
    // touch the network.
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Palmier Pro"));

    const CompatibilityReport report = palmier::app::checkPlatformCompatibility();
    if (!report.compatible()) {
        // Requirements 1.4 / 1.5: name each unmet item and exit without the editor.
        reportToConsole(report);
        const QString title = report.unsupportedPlatform()
                                  ? QStringLiteral("Unsupported platform")
                                  : QStringLiteral("Missing dependencies");
        QMessageBox::critical(nullptr, title,
                              QString::fromStdString(report.message()));
        return 1;
    }

    // Supported host: compose the application and start the MCP server, then
    // show the editor (Requirements 1.6, 7.1, 7.2). The composition root wires
    // GpuContext, TimelineEngine, the media library, project I/O, auth, the
    // generative client, the MCP server + executor, the agent orchestrator, and
    // localization together; constructing it performs no network activity, so the
    // editor still starts without a network connection (13.3/13.4).
    palmier::app::ApplicationComposition composition;

    // Start the loopback MCP endpoint (127.0.0.1:19789/mcp). On a port-in-use
    // conflict the server refuses to start and leaves the project unchanged
    // (Requirement 7.3); we surface a non-blocking warning and continue running
    // the editor, which is fully usable without the agent endpoint.
    if (const palmier::Result<void> started = composition.start(); !started) {
        std::cerr << "MCP server did not start: " << started.error().toString() << '\n';
        QMessageBox::warning(
            nullptr, QStringLiteral("MCP server unavailable"),
            QString::fromStdString(
                "The MCP server could not start:\n" + started.error().message() +
                "\n\nThe editor will run, but external agents cannot connect."));
    }

    palmier::ui::MainWindow window;
    window.show();
    const int exitCode = app.exec();

    // Stop the MCP server on close (Requirement 7.9).
    composition.stop();
    return exitCode;
#else
    // Headless build (Qt not installed): still run the launch gate so the
    // decision logic is exercisable, and report the verdict on the console.
    (void)argc;
    (void)argv;
    const CompatibilityReport report = palmier::app::checkPlatformCompatibility();
    if (!report.compatible()) {
        reportToConsole(report);
        return 1;
    }

    // Exercise the composition root + MCP lifecycle headlessly so the launch/close
    // path is runnable where Qt is not installed (Requirements 1.6, 7.2, 7.9).
    palmier::app::ApplicationComposition composition;
    if (const palmier::Result<void> started = composition.start(); !started) {
        std::cerr << "MCP server did not start: " << started.error().toString() << '\n';
    } else {
        std::cout << "MCP server listening on 127.0.0.1:" << composition.mcpBoundPort()
                  << "/mcp\n";
        composition.stop();
    }
    std::cout << "Platform supported; editor UI requires a Qt 6 build.\n";
    return 0;
#endif
}
