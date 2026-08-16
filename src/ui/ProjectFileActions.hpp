// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/ProjectFileActions.hpp — File menu actions and the unsaved-changes prompt
// (task 11.5; Requirements 4.1, 4.2, 4.3, 4.5, 4.9, 4.10).
//
// Wraps the File > New / Open / Save / Save As gestures and the unsaved-changes
// confirmation prompt that must precede a destructive close/open/new when the
// current project is modified. Every actual save/open/create call is routed
// through the shared GuiToolGateway (task 11.4), so a File-menu action is
// schema-validated, atomic and observed exactly like an MCP or agent-issued
// project operation (Requirements 1.7, 9.4, 11.5); this class owns only the
// dialog/prompt orchestration around those calls.
//
// PendingIntent state machine (Requirement 4.9): while a confirmation prompt is
// displayed, no state changes and no file is written. The prompt then resolves
// to exactly one outcome:
//   * Save   — the pending close/open/new proceeds ONLY if the save succeeds;
//              a failed save cancels the pending operation and leaves the
//              current project open (Requirement 4.5).
//   * Discard — the pending operation proceeds without writing anything.
//   * Cancel  — the pending operation is abandoned; nothing changes.
// A dismissed prompt (Cancel, or the dialog closed without a choice) writes
// nothing, loads nothing, and reports no error (Requirement 4.10).
//
// This class is deliberately thin: the actual QFileDialog / QMessageBox calls
// are behind small virtual seams (see UiPrompts below) so the state machine and
// its "exactly one outcome, nothing happens while displayed" contract can be
// unit-tested under xvfb without driving real modal dialogs.

#ifndef PALMIER_UI_PROJECTFILEACTIONS_HPP
#define PALMIER_UI_PROJECTFILEACTIONS_HPP

#include <functional>
#include <optional>
#include <string>

#include "core/Result.hpp"
#include "services/ProjectSession.hpp"
#include "ui/GuiToolGateway.hpp"

namespace palmier::ui {

/// The three resolutions of the unsaved-changes prompt (Requirement 4.9).
enum class UnsavedChangesChoice { Save, Discard, Cancel };

/// Which gesture triggered the file action, so the caller's completion
/// callback (if any) can react appropriately.
enum class FileActionKind { New, Open, Save, SaveAs };

/// The outcome of a single ProjectFileActions call.
struct FileActionResult {
    bool        ok = false;
    /// True iff a confirmation prompt was shown and the user cancelled it, or
    /// dismissed a destination/open-path picker (Requirement 4.10): nothing was
    /// written or loaded, and this is NOT an error.
    bool        dismissed = false;
    std::string message;  ///< Human-readable outcome (success note or error).
};

/// Seams for the actual UI prompts, so tests can supply deterministic
/// responses. The production implementation (installed by MainWindow, task
/// 11.2) shows real QFileDialog / QMessageBox dialogs.
struct UiPrompts {
    /// Ask the user to resolve unsaved changes before a destructive operation.
    /// `projectName` is shown in the prompt text.
    std::function<UnsavedChangesChoice(const std::string& projectName)> confirmUnsavedChanges;

    /// Ask for a destination path for Save As / first save. Defaults to a
    /// `.palmier` extension. Returns std::nullopt when the user cancels.
    std::function<std::optional<std::string>()> promptSaveDestination;

    /// Ask for a `.palmier` document to open. Returns std::nullopt when the
    /// user cancels.
    std::function<std::optional<std::string>()> promptOpenSource;

    /// Report a completed action to the user (a written path, or an error).
    /// Optional — a headless/test harness can leave it unset.
    std::function<void(FileActionKind kind, const FileActionResult& result)> notify;
};

/// Orchestrates File > New / Open / Save / Save As against a GuiToolGateway
/// (task 11.4) and a ProjectSession (read-only, for modified()/documentPath()).
class ProjectFileActions {
public:
    /// `session` and `gateway` must outlive this object.
    ProjectFileActions(services::ProjectSession& session, GuiToolGateway& gateway,
                       UiPrompts prompts);

    /// File > Save: writes to the recorded document path. If the project has
    /// never been saved, behaves like Save As (prompts for a destination).
    FileActionResult save();

    /// File > Save As: always prompts for a destination.
    FileActionResult saveAs();

    /// File > Open: if the current project is modified, first resolves the
    /// unsaved-changes prompt; proceeds to the open-source picker + load only
    /// on Save-succeeded or Discard. A cancelled prompt or a cancelled path
    /// picker is `dismissed = true` with no state change.
    FileActionResult open();

    /// File > Open at a known path (e.g. a recent-files entry), skipping the
    /// picker but still running the unsaved-changes prompt when needed.
    FileActionResult openPath(const std::string& path);

    /// File > New: creates a new default-settings project, again preceded by
    /// the unsaved-changes prompt when needed.
    FileActionResult newProject(const std::string& name, double fps, std::uint32_t width,
                               std::uint32_t height);

private:
    // Runs the unsaved-changes prompt if (and only if) the project is
    // currently modified, and returns whether the caller may proceed with its
    // pending destructive operation. `false` means the caller must stop and
    // report `dismissed = true` (Cancel) — a failed Save-then-proceed also
    // returns false, with the save's own error preserved in `outSaveError`.
    bool resolvePendingIfModified(bool* outDismissed, std::string* outSaveError);

    services::ProjectSession& session_;
    GuiToolGateway&            gateway_;
    UiPrompts                  prompts_;
};

}  // namespace palmier::ui

#endif  // PALMIER_UI_PROJECTFILEACTIONS_HPP
