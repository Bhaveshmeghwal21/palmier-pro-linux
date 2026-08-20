// SPDX-License-Identifier: GPL-3.0-or-later
//
// app/main.cpp — Palmier Pro application entry point (application shell).
//
// Launch sequence (Requirement 1):
//   0. Resolve the runtime configuration with app::AppSettings — built-in defaults,
//      then the key=value config file, then the PALMIER_* environment variables,
//      then the command line (Requirements 10.2, 16.3) — and hand the resulting
//      AppConfig to the composition root, so every configurable option (the MCP
//      endpoint, remote access, the agent interpreter, the generative backend) is
//      reachable from the shipped binary. `--help` prints the accepted options and
//      exits here; anything the resolver could not use is reported on stderr and
//      ignored, leaving the safe default in place, and the editor still starts.
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
#include <memory>

#include "app/AppSettings.hpp"
#include "app/ApplicationComposition.hpp"
#include "app/ComponentConstructionError.hpp"
#include "app/PlatformCompatibility.hpp"
#include "core/Result.hpp"

#ifdef PALMIER_HAVE_QT
#include <QApplication>
#include <QMessageBox>
#include <QString>

#include "ui/MainWindow.hpp"
#endif

namespace {

using palmier::app::AppSettings;
using palmier::app::ApplicationComposition;
using palmier::app::CompatibilityReport;

/// Print the incompatibility report to stderr (used on every build, and as the
/// only channel in a headless build without Qt).
void reportToConsole(const CompatibilityReport& report) {
    std::cerr << report.message() << '\n';
}

/// Report every input `AppSettings` could not make sense of — a malformed config
/// line, an unknown key, an unknown option, an out-of-range value — on stderr,
/// followed by the accepted-option list so the operator can see what was expected.
///
/// Rejected input is NOT fatal, by design: the resolver ignores what it cannot use
/// and leaves the lower-precedence (ultimately the safe default) value in place, so
/// the editor still starts. Reporting it here is what keeps that from being silent.
/// Diagnostics never contain a value, so this is safe with a bearer token
/// configured (Requirements 10.3, 10.8).
void reportSettingsDiagnostics(const AppSettings& settings) {
    if (settings.diagnostics().empty()) {
        return;
    }
    for (const std::string& diagnostic : settings.diagnostics()) {
        std::cerr << "palmier-pro: " << diagnostic << '\n';
    }
    std::cerr << '\n' << AppSettings::usage();
}

/// Report the composition's non-fatal startup errors — a rejected agent
/// interpreter id (Requirement 11.8), a rejected generative backend id
/// (Requirement 12.8) — and, once `start()` has run, every unmet remote-access
/// prerequisite (Requirement 10.12) and the unencrypted-traffic warning
/// (Requirement 10.7). Each names the rejected id or the unmet requirement and
/// never a credential value; in every case the application is already up with the
/// safe fallback installed.
void reportStartupErrors(const ApplicationComposition& composition) {
    for (const std::string& error : composition.startupErrors()) {
        std::cerr << "palmier-pro: " << error << '\n';
    }
    if (!composition.remoteAccessStartupError().empty()) {
        std::cerr << "palmier-pro: " << composition.remoteAccessStartupError() << '\n';
    }
    if (!composition.remoteAccessWarning().empty()) {
        std::cerr << "palmier-pro: " << composition.remoteAccessWarning() << '\n';
    }
}

} // namespace

int main(int argc, char** argv) {
    // Resolve the runtime configuration BEFORE anything else: built-in defaults,
    // then $XDG_CONFIG_HOME/palmier-pro/config, then the PALMIER_* environment
    // variables, then the command line (Requirements 10.2, 16.3). This is what
    // makes every configurable option — the MCP endpoint, remote access, the agent
    // interpreter and the generative backend — reachable from the shipped binary.
    // Resolution reads a file and named environment variables only; it touches no
    // network and constructs nothing.
    const AppSettings settings = AppSettings::fromArgv(argc, argv);

    // `--help` / `-h`: print the accepted options and exit without constructing a
    // project session, a composition or (in the Qt build) a QApplication.
    if (settings.helpRequested()) {
        std::cout << AppSettings::usage();
        return 0;
    }

    reportSettingsDiagnostics(settings);

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
    //
    // Startup construction guard (task 11.7; Requirements 1.1, 1.9): every
    // component the composition owns is designed to degrade gracefully rather
    // than throw (a missing GPU falls back to software, an unconfigured backend
    // falls back offline, ...), so this is not expected to fire on a supported
    // host. It exists for the residual case an exception nonetheless escapes
    // construction, in which case the failure is reported and the editor shell
    // is never constructed or shown.
    std::unique_ptr<ApplicationComposition> compositionPtr;
    try {
        compositionPtr = std::make_unique<ApplicationComposition>(settings.config());
    } catch (const palmier::app::ComponentConstructionError& ex) {
        std::cerr << "palmier-pro: " << ex.what() << '\n';
        QMessageBox::critical(
            nullptr, QStringLiteral("Startup failed"),
            QStringLiteral("Palmier Pro could not start: failed to construct '%1':\n%2")
                .arg(QString::fromStdString(ex.componentName()),
                    QString::fromStdString(ex.reason())));
        return 1;
    } catch (const std::exception& ex) {
        std::cerr << "palmier-pro: startup failed: " << ex.what() << '\n';
        QMessageBox::critical(nullptr, QStringLiteral("Startup failed"),
                              QStringLiteral("Palmier Pro could not start:\n%1")
                                  .arg(QString::fromStdString(ex.what())));
        return 1;
    }
    ApplicationComposition& composition = *compositionPtr;

    // Start the MCP endpoint (127.0.0.1:19789/mcp unless configuration moved it).
    // On a port-in-use conflict the server refuses to start and leaves the project
    // unchanged (Requirement 7.3); we surface a non-blocking warning and continue
    // running the editor, which is fully usable without the agent endpoint.
    if (const palmier::Result<void> started = composition.start(); !started) {
        std::cerr << "MCP server did not start: " << started.error().toString() << '\n';
        QMessageBox::warning(
            nullptr, QStringLiteral("MCP server unavailable"),
            QString::fromStdString(
                "The MCP server could not start:\n" + started.error().message() +
                "\n\nThe editor will run, but external agents cannot connect."));
    }

    // Every non-fatal condition that changed how a component was installed, now
    // that start() has evaluated the remote-access prerequisites too.
    reportStartupErrors(composition);

    palmier::ui::MainWindow window(composition);
    window.show();
    const int exitCode = app.exec();

    // Stop the MCP server on close (Requirement 7.9).
    composition.stop();
    return exitCode;
#else
    // Headless build (Qt not installed): still run the launch gate so the
    // decision logic is exercisable, and report the verdict on the console.
    const CompatibilityReport report = palmier::app::checkPlatformCompatibility();
    if (!report.compatible()) {
        reportToConsole(report);
        return 1;
    }

    // Exercise the composition root + MCP lifecycle headlessly so the launch/close
    // path is runnable where Qt is not installed (Requirements 1.6, 7.2, 7.9). The
    // resolved configuration reaches the composition on this path too, so the
    // console shell honours the same options as the editor.
    std::unique_ptr<ApplicationComposition> compositionPtr;
    try {
        compositionPtr = std::make_unique<ApplicationComposition>(settings.config());
    } catch (const palmier::app::ComponentConstructionError& ex) {
        std::cerr << "palmier-pro: " << ex.what() << '\n';
        return 1;
    } catch (const std::exception& ex) {
        std::cerr << "palmier-pro: startup failed: " << ex.what() << '\n';
        return 1;
    }
    ApplicationComposition& composition = *compositionPtr;
    if (const palmier::Result<void> started = composition.start(); !started) {
        std::cerr << "MCP server did not start: " << started.error().toString() << '\n';
    } else {
        std::cout << "MCP server listening on " << settings.config().mcpHost << ":"
                  << composition.mcpBoundPort() << "/mcp\n";
        composition.stop();
    }
    reportStartupErrors(composition);
    std::cout << "Platform supported; editor UI requires a Qt 6 build.\n";
    return 0;
#endif
}
