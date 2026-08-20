// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/ProjectFileActions.cpp — implementation of the File menu action
// orchestration (task 11.5).
//
// Qt-free: every dialog is behind the injected UiPrompts seam, so this file
// compiles and is unit-testable without Qt. MainWindow (task 11.2) supplies the
// real QFileDialog/QMessageBox-backed UiPrompts.

#include "ui/ProjectFileActions.hpp"

#include <utility>

namespace palmier::ui {

ProjectFileActions::ProjectFileActions(services::ProjectSession& session, GuiToolGateway& gateway,
                                       UiPrompts prompts)
    : session_(session), gateway_(gateway), prompts_(std::move(prompts)) {}

bool ProjectFileActions::resolvePendingIfModified(bool* outDismissed,
                                                  std::string* outSaveError) {
    *outDismissed = false;
    if (outSaveError != nullptr) {
        outSaveError->clear();
    }
    if (!session_.modified()) {
        return true;
    }
    if (!prompts_.confirmUnsavedChanges) {
        // No prompt installed: proceed without asking (used by headless/test
        // callers that never model unsaved changes). Production always installs
        // a real prompt.
        return true;
    }

    const services::ProjectSession::Status status = session_.status();
    const UnsavedChangesChoice choice = prompts_.confirmUnsavedChanges(status.name);

    switch (choice) {
        case UnsavedChangesChoice::Discard:
            return true;
        case UnsavedChangesChoice::Cancel:
            *outDismissed = true;
            return false;
        case UnsavedChangesChoice::Save: {
            const FileActionResult saveResult = save();
            if (!saveResult.ok) {
                if (outSaveError != nullptr) {
                    *outSaveError = saveResult.message;
                }
                return false;
            }
            return true;
        }
    }
    *outDismissed = true;
    return false;
}

FileActionResult ProjectFileActions::save() {
    if (!session_.documentPath().has_value()) {
        return saveAs();
    }

    Result<services::Json> result = gateway_.saveProject();
    FileActionResult out;
    if (result.isOk()) {
        out.ok = true;
        out.message = "Saved " + session_.documentPath()->string();
    } else {
        out.ok = false;
        out.message = result.error().message();
    }
    if (prompts_.notify) {
        prompts_.notify(FileActionKind::Save, out);
    }
    return out;
}

FileActionResult ProjectFileActions::saveAs() {
    FileActionResult out;
    if (!prompts_.promptSaveDestination) {
        out.ok = false;
        out.message = "No save-destination prompt is installed";
        if (prompts_.notify) prompts_.notify(FileActionKind::SaveAs, out);
        return out;
    }

    const std::optional<std::string> destination = prompts_.promptSaveDestination();
    if (!destination.has_value()) {
        out.ok = false;
        out.dismissed = true;
        out.message = "Save As cancelled";
        return out;  // Requirement 4.10: a dismissed prompt reports no error.
    }

    Result<services::Json> result = gateway_.saveProject(*destination);
    if (result.isOk()) {
        out.ok = true;
        out.message = "Saved " + *destination;
    } else {
        out.ok = false;
        out.message = result.error().message();
    }
    if (prompts_.notify) {
        prompts_.notify(FileActionKind::SaveAs, out);
    }
    return out;
}

FileActionResult ProjectFileActions::open() {
    // Requirement 4.x's PendingIntent contract: unsaved changes must be
    // resolved (Discard/Cancel/Save) BEFORE the user is even asked which file
    // to open, so a Cancel never reaches the open-source prompt at all and a
    // Save actually writes before the picker appears.
    bool dismissed = false;
    std::string saveError;
    if (!resolvePendingIfModified(&dismissed, &saveError)) {
        FileActionResult out;
        out.ok = false;
        out.dismissed = dismissed;
        out.message = dismissed ? "Open cancelled" : saveError;
        if (prompts_.notify) prompts_.notify(FileActionKind::Open, out);
        return out;
    }

    if (!prompts_.promptOpenSource) {
        FileActionResult out;
        out.ok = false;
        out.message = "No open-source prompt is installed";
        return out;
    }
    const std::optional<std::string> path = prompts_.promptOpenSource();
    if (!path.has_value()) {
        FileActionResult out;
        out.ok = false;
        out.dismissed = true;
        out.message = "Open cancelled";
        return out;  // Requirement 4.10: a dismissed prompt reports no error.
    }
    return loadFromPath(*path);
}

FileActionResult ProjectFileActions::openPath(const std::string& path) {
    bool dismissed = false;
    std::string saveError;
    if (!resolvePendingIfModified(&dismissed, &saveError)) {
        FileActionResult out;
        out.ok = false;
        out.dismissed = dismissed;
        out.message = dismissed ? "Open cancelled" : saveError;
        if (prompts_.notify) prompts_.notify(FileActionKind::Open, out);
        return out;
    }
    return loadFromPath(path);
}

FileActionResult ProjectFileActions::loadFromPath(const std::string& path) {
    Result<services::Json> result = gateway_.openProject(path);
    FileActionResult out;
    if (result.isOk()) {
        out.ok = true;
        out.message = "Opened " + path;
    } else {
        out.ok = false;
        out.message = result.error().message();
    }
    if (prompts_.notify) {
        prompts_.notify(FileActionKind::Open, out);
    }
    return out;
}

FileActionResult ProjectFileActions::newProject(const std::string& name, double fps,
                                                std::uint32_t width, std::uint32_t height) {
    bool dismissed = false;
    std::string saveError;
    if (!resolvePendingIfModified(&dismissed, &saveError)) {
        FileActionResult out;
        out.ok = false;
        out.dismissed = dismissed;
        out.message = dismissed ? "New project cancelled" : saveError;
        if (prompts_.notify) prompts_.notify(FileActionKind::New, out);
        return out;
    }

    Result<services::Json> result = gateway_.createProject(name, fps, width, height);
    FileActionResult out;
    if (result.isOk()) {
        out.ok = true;
        out.message = "Created project '" + name + "'";
    } else {
        out.ok = false;
        out.message = result.error().message();
    }
    if (prompts_.notify) {
        prompts_.notify(FileActionKind::New, out);
    }
    return out;
}

}  // namespace palmier::ui
