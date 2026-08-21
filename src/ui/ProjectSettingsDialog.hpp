// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/ProjectSettingsDialog.hpp — the mutable project settings dialog (task 10.2;
// Requirement 7).
//
// Reads the current project's frame rate, canvas resolution and colour space
// from a live snapshot and, on Apply, submits only the fields the user actually
// changed through `GuiToolGateway::setProjectSettings()` — the same
// `project.set_settings` tool the MCP endpoint and the in-app agent can also
// call, so a change made here is undoable, marks the project modified, and is
// refused by name if it falls outside the declared range, exactly as it would be
// from any other caller (Requirements 7.1, 7.4, 7.5).
//
// Unlike ExportDialog, this dialog has no long-running operation to poll: the
// command applies synchronously, so Apply either updates the form to reflect
// success or shows the tool's own error message — the same immediate-feedback
// pattern InspectorPanel uses for its own edits.

#ifndef PALMIER_UI_PROJECTSETTINGSDIALOG_HPP
#define PALMIER_UI_PROJECTSETTINGSDIALOG_HPP

#ifdef PALMIER_HAVE_QT

#include <QDialog>

#include "core/ColorSpace.hpp"
#include "core/FrameRate.hpp"
#include "core/Resolution.hpp"

class QComboBox;
class QSpinBox;
class QDoubleSpinBox;
class QPushButton;
class QLabel;

namespace palmier::ui {

class GuiToolGateway;

/// The project settings dialog. Owns no project state itself: every value shown
/// is read fresh from the snapshot passed to the constructor, and every change
/// is submitted through the gateway rather than applied locally.
class ProjectSettingsDialog : public QDialog {
    Q_OBJECT

public:
    /// `gateway` must outlive the dialog. `currentFps`/`currentCanvas`/
    /// `currentColorSpace` seed the form from the project's live values
    /// (Requirement 7.2 — "reads the current values").
    ProjectSettingsDialog(GuiToolGateway& gateway, FrameRate currentFps,
                         Resolution currentCanvas, ColorSpace currentColorSpace,
                         QWidget* parent = nullptr);
    ~ProjectSettingsDialog() override;

private slots:
    void onApplyClicked();

private:
    void buildLayout();

    GuiToolGateway& gateway_;
    FrameRate       currentFps_;
    Resolution      currentCanvas_;
    ColorSpace      currentColorSpace_;
    double          lastAppliedFpsValue_ = 0.0;  ///< avoids round-tripping fps through FrameRate

    QDoubleSpinBox* fpsSpin_ = nullptr;
    QSpinBox*       widthSpin_ = nullptr;
    QSpinBox*       heightSpin_ = nullptr;
    QComboBox*      colorSpaceCombo_ = nullptr;
    QPushButton*    applyButton_ = nullptr;
    QLabel*         statusLabel_ = nullptr;
};

}  // namespace palmier::ui

#endif  // PALMIER_HAVE_QT

#endif  // PALMIER_UI_PROJECTSETTINGSDIALOG_HPP
