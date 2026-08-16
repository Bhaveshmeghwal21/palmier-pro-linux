// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/ExportDialog.cpp — implementation of the export dialog and progress
// surface (task 11.6).
//
// Compiled only when Qt 6 is available (PALMIER_HAVE_QT).

#include "ui/ExportDialog.hpp"

#ifdef PALMIER_HAVE_QT

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

namespace palmier::ui {

namespace {
// Poll the coordinator's queued progress/outcome at this cadence. Comfortably
// inside the coordinator's own <=1s progress-report ceiling (Requirement 7.3)
// so no report is ever held back waiting on this timer.
constexpr int kPumpIntervalMs = 200;
}  // namespace

ExportDialog::ExportDialog(services::ExportCoordinator& coordinator, Resolution defaultResolution,
                           FrameRate defaultFps, QWidget* parent)
    : QDialog(parent),
      coordinator_(coordinator),
      defaultResolution_(defaultResolution),
      defaultFps_(defaultFps),
      pumpTimer_(new QTimer(this)) {
    setWindowTitle(QStringLiteral("Export Video"));
    buildLayout();

    connect(pumpTimer_, &QTimer::timeout, this, &ExportDialog::onPump);
}

ExportDialog::~ExportDialog() {
    // Do not touch the coordinator's running export here: it is the
    // application's single Requirement 1.1 instance and outlives this dialog by
    // design. Closing the dialog while an export runs simply stops polling; a
    // fresh ExportDialog (or MainWindow's own pump) can resume observing it.
}

void ExportDialog::buildLayout() {
    auto* rootLayout = new QVBoxLayout(this);
    auto* form = new QFormLayout();

    auto* pathRow = new QWidget(this);
    auto* pathLayout = new QHBoxLayout(pathRow);
    pathLayout->setContentsMargins(0, 0, 0, 0);
    outputPathEdit_ = new QLineEdit(pathRow);
    browseButton_ = new QPushButton(QStringLiteral("Browse…"), pathRow);
    connect(browseButton_, &QPushButton::clicked, this, &ExportDialog::onBrowseClicked);
    pathLayout->addWidget(outputPathEdit_, /*stretch=*/1);
    pathLayout->addWidget(browseButton_);
    form->addRow(QStringLiteral("Output path:"), pathRow);

    containerCombo_ = new QComboBox(this);
    containerCombo_->addItems({QStringLiteral("mp4"), QStringLiteral("mov"),
                               QStringLiteral("mkv"), QStringLiteral("webm")});
    form->addRow(QStringLiteral("Container:"), containerCombo_);

    codecCombo_ = new QComboBox(this);
    codecCombo_->addItems(
        {QStringLiteral("h264"), QStringLiteral("hevc"), QStringLiteral("vp9")});
    form->addRow(QStringLiteral("Codec:"), codecCombo_);

    widthSpin_ = new QSpinBox(this);
    widthSpin_->setRange(128, 3840);
    widthSpin_->setValue(static_cast<int>(defaultResolution_.width));
    form->addRow(QStringLiteral("Width:"), widthSpin_);

    heightSpin_ = new QSpinBox(this);
    heightSpin_->setRange(128, 2160);
    heightSpin_->setValue(static_cast<int>(defaultResolution_.height));
    form->addRow(QStringLiteral("Height:"), heightSpin_);

    fpsSpin_ = new QDoubleSpinBox(this);
    fpsSpin_->setRange(1.0, 120.0);
    fpsSpin_->setValue(defaultFps_.toDouble());
    form->addRow(QStringLiteral("Frame rate:"), fpsSpin_);

    bitrateSpin_ = new QSpinBox(this);
    bitrateSpin_->setRange(100, 200000);
    bitrateSpin_->setValue(8000);
    form->addRow(QStringLiteral("Bit rate (kbps):"), bitrateSpin_);

    includeAudioCheck_ = new QCheckBox(QStringLiteral("Include audio"), this);
    includeAudioCheck_->setChecked(true);
    form->addRow(QString(), includeAudioCheck_);

    preferHardwareCheck_ = new QCheckBox(QStringLiteral("Prefer hardware encoding"), this);
    preferHardwareCheck_->setChecked(true);
    form->addRow(QString(), preferHardwareCheck_);

    overwriteCheck_ = new QCheckBox(QStringLiteral("Overwrite existing file"), this);
    form->addRow(QString(), overwriteCheck_);

    rootLayout->addLayout(form);

    progressBar_ = new QProgressBar(this);
    progressBar_->setRange(0, 100);
    progressBar_->setValue(0);
    rootLayout->addWidget(progressBar_);

    statusLabel_ = new QLabel(QStringLiteral("Ready."), this);
    rootLayout->addWidget(statusLabel_);

    auto* buttonRow = new QWidget(this);
    auto* buttonLayout = new QHBoxLayout(buttonRow);
    buttonLayout->setContentsMargins(0, 0, 0, 0);
    startButton_ = new QPushButton(QStringLiteral("Export"), buttonRow);
    connect(startButton_, &QPushButton::clicked, this, &ExportDialog::onStartClicked);
    cancelButton_ = new QPushButton(QStringLiteral("Cancel"), buttonRow);
    cancelButton_->setEnabled(false);
    connect(cancelButton_, &QPushButton::clicked, this, &ExportDialog::onCancelClicked);
    buttonLayout->addStretch(1);
    buttonLayout->addWidget(startButton_);
    buttonLayout->addWidget(cancelButton_);
    rootLayout->addWidget(buttonRow);
}

void ExportDialog::onBrowseClicked() {
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export Video"), outputPathEdit_->text(),
        QStringLiteral("Video files (*.mp4 *.mov *.mkv *.webm)"));
    if (!path.isEmpty()) {
        outputPathEdit_->setText(path);
    }
}

services::ExportRequest2 ExportDialog::buildRequest() const {
    services::ExportRequest2 request;
    request.outputPath = outputPathEdit_->text().toStdString();
    request.container = containerCombo_->currentText().toStdString();

    const QString codecText = codecCombo_->currentText();
    if (codecText == QStringLiteral("hevc")) {
        request.codec = gpu::CodecId::HEVC;
    } else if (codecText == QStringLiteral("vp9")) {
        request.codec = gpu::CodecId::VP9;
    } else {
        request.codec = gpu::CodecId::H264;
    }

    request.resolution = Resolution{static_cast<std::uint32_t>(widthSpin_->value()),
                                    static_cast<std::uint32_t>(heightSpin_->value())};
    // FrameRate has no fromDouble(): build an exact rational from the spin box's
    // value at a fixed denominator, which is precise enough for the integer and
    // simple-fractional rates (24/25/30/50/60) the form realistically produces.
    request.frameRate = FrameRate(static_cast<std::int64_t>(fpsSpin_->value() * 1000.0 + 0.5),
                                  1000);
    request.bitrateKbps = bitrateSpin_->value();
    request.includeAudio = includeAudioCheck_->isChecked();
    request.preferHardware = preferHardwareCheck_->isChecked();
    request.overwrite = overwriteCheck_->isChecked();
    return request;
}

void ExportDialog::setFormEnabled(bool enabled) {
    outputPathEdit_->setEnabled(enabled);
    browseButton_->setEnabled(enabled);
    containerCombo_->setEnabled(enabled);
    codecCombo_->setEnabled(enabled);
    widthSpin_->setEnabled(enabled);
    heightSpin_->setEnabled(enabled);
    fpsSpin_->setEnabled(enabled);
    bitrateSpin_->setEnabled(enabled);
    includeAudioCheck_->setEnabled(enabled);
    preferHardwareCheck_->setEnabled(enabled);
    overwriteCheck_->setEnabled(enabled);
    startButton_->setEnabled(enabled);
    cancelButton_->setEnabled(!enabled);
}

void ExportDialog::onStartClicked() {
    const services::ExportRequest2 request = buildRequest();

    if (Result<void> valid = services::ExportCoordinator::validate(request); valid.isError()) {
        statusLabel_->setText(QString::fromStdString(valid.error().message()));
        return;
    }

    setFormEnabled(false);
    progressBar_->setValue(0);
    statusLabel_->setText(QStringLiteral("Exporting…"));

    Result<void> started = coordinator_.begin(
        request,
        [this](const services::ExportProgressReport& report) { reportProgress(report); },
        [this](const Result<services::ExportOutcome>& outcome) { reportCompletion(outcome); });

    if (started.isError()) {
        statusLabel_->setText(QString::fromStdString(started.error().message()));
        setFormEnabled(true);
        return;
    }

    pumpTimer_->start(kPumpIntervalMs);
}

void ExportDialog::onCancelClicked() {
    coordinator_.cancel();
    statusLabel_->setText(QStringLiteral("Cancelling…"));
}

void ExportDialog::onPump() {
    // Every callback runs from here (the UI thread), never from the worker, so
    // the widgets below are always touched safely (Requirement 7.3's "keeping
    // the window responsive" — pump() only drains already-queued notifications
    // and never blocks).
    coordinator_.pump();
    if (!coordinator_.running()) {
        pumpTimer_->stop();
    }
}

void ExportDialog::reportProgress(const services::ExportProgressReport& report) {
    progressBar_->setValue(report.percent);
    statusLabel_->setText(QStringLiteral("Exporting… %1% (%2/%3 frames)")
                              .arg(report.percent)
                              .arg(static_cast<qulonglong>(report.framesEncoded))
                              .arg(static_cast<qulonglong>(report.totalFrames)));
}

void ExportDialog::reportCompletion(const Result<services::ExportOutcome>& outcome) {
    pumpTimer_->stop();
    setFormEnabled(true);

    if (outcome.isError()) {
        const QString message = QString::fromStdString(outcome.error().message());
        statusLabel_->setText(message);
        emit exportFinished(false, message);
        return;
    }

    const services::ExportOutcome& result = outcome.value();
    if (result.cancelled) {
        const QString message = QStringLiteral("Export cancelled.");
        statusLabel_->setText(message);
        emit exportFinished(false, message);
        return;
    }

    progressBar_->setValue(100);
    const QString message =
        QStringLiteral("Exported %1 frames to %2")
            .arg(static_cast<qulonglong>(result.framesEncoded))
            .arg(QString::fromStdString(result.outputPath.string()));
    statusLabel_->setText(message);
    emit exportFinished(true, message);
}

}  // namespace palmier::ui

#endif  // PALMIER_HAVE_QT
