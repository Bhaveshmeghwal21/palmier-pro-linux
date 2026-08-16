// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/ExportDialog.hpp — the export configuration dialog and progress surface
// (task 11.6; Requirements 7.1, 7.3, 7.7).
//
// Collects every field Requirement 7.2 lets a caller specify — output path,
// container, codec, resolution, frame rate, bit rate, audio inclusion, hardware
// preference and the overwrite acknowledgement — and starts the export directly
// against the application's single `services::ExportCoordinator`
// (Requirement 1.1), NOT through the blocking `timeline.export` tool call: that
// tool call is synchronous by contract (services::makeExportToolHandler awaits
// the whole export inline, per its own documented "the GUI does not take this
// path" note), which would freeze the UI thread for the export's duration. The
// coordinator's own admission (`validate()`/`validateTimeline()`) and worker
// model are exactly what the tool call itself drives, so a dialog-started export
// enforces the identical rules (Requirements 7.6, 7.9, 7.11) — only the
// synchronous-wait wrapper is bypassed.
//
// A QTimer polls `ExportCoordinator::pump()` at a modest interval (mirroring
// PreviewView's timer-driven pump of PreviewController), which marshals queued
// progress reports and the final outcome onto the UI thread and updates the
// status-bar progress bar this dialog owns. The Cancel button calls
// `ExportCoordinator::cancel()`, which the coordinator guarantees leaves no
// partial output file (Requirement 7.7).

#ifndef PALMIER_UI_EXPORTDIALOG_HPP
#define PALMIER_UI_EXPORTDIALOG_HPP

#ifdef PALMIER_HAVE_QT

#include <QDialog>

#include "services/ExportCoordinator.hpp"

class QComboBox;
class QLineEdit;
class QCheckBox;
class QSpinBox;
class QDoubleSpinBox;
class QPushButton;
class QProgressBar;
class QLabel;
class QTimer;

namespace palmier::ui {

/// The export configuration dialog. Owns no export state itself beyond the
/// in-flight UI widgets — all export state lives in the shared
/// `services::ExportCoordinator`, so this dialog could be destroyed and
/// recreated without disturbing a running export (the coordinator is the
/// single Requirement 1.1 component, not this dialog).
class ExportDialog : public QDialog {
    Q_OBJECT

public:
    /// `coordinator` must outlive the dialog (it is the application's single
    /// instance, owned by ApplicationComposition). `defaultResolution` and
    /// `defaultFps` seed the form from the current project's canvas/frame rate.
    ExportDialog(services::ExportCoordinator& coordinator, Resolution defaultResolution,
                FrameRate defaultFps, QWidget* parent = nullptr);
    ~ExportDialog() override;

signals:
    /// Emitted once, when the export this dialog started finishes (success,
    /// failure or cancellation), so MainWindow's status bar can show a final
    /// notice even if the dialog has since been closed/hidden.
    void exportFinished(bool succeeded, const QString& message);

private slots:
    void onBrowseClicked();
    void onStartClicked();
    void onCancelClicked();
    void onPump();

private:
    void buildLayout();
    services::ExportRequest2 buildRequest() const;
    void setFormEnabled(bool enabled);
    void reportProgress(const services::ExportProgressReport& report);
    void reportCompletion(const Result<services::ExportOutcome>& outcome);

    services::ExportCoordinator& coordinator_;
    Resolution                   defaultResolution_;
    FrameRate                    defaultFps_;

    QLineEdit*      outputPathEdit_ = nullptr;
    QPushButton*    browseButton_ = nullptr;
    QComboBox*      containerCombo_ = nullptr;
    QComboBox*      codecCombo_ = nullptr;
    QSpinBox*       widthSpin_ = nullptr;
    QSpinBox*       heightSpin_ = nullptr;
    QDoubleSpinBox* fpsSpin_ = nullptr;
    QSpinBox*       bitrateSpin_ = nullptr;
    QCheckBox*      includeAudioCheck_ = nullptr;
    QCheckBox*      preferHardwareCheck_ = nullptr;
    QCheckBox*      overwriteCheck_ = nullptr;
    QPushButton*    startButton_ = nullptr;
    QPushButton*    cancelButton_ = nullptr;
    QProgressBar*   progressBar_ = nullptr;
    QLabel*         statusLabel_ = nullptr;
    QTimer*         pumpTimer_ = nullptr;
};

}  // namespace palmier::ui

#endif  // PALMIER_HAVE_QT

#endif  // PALMIER_UI_EXPORTDIALOG_HPP
