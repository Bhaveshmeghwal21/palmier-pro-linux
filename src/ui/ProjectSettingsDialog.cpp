// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/ProjectSettingsDialog.cpp — implementation of the mutable project settings
// dialog (task 10.2).
//
// Compiled only when Qt 6 is available (PALMIER_HAVE_QT).

#include "ui/ProjectSettingsDialog.hpp"

#ifdef PALMIER_HAVE_QT

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QString>
#include <QVBoxLayout>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ui/GuiToolGateway.hpp"

namespace palmier::ui {

namespace {

// The colour spaces this dialog offers: exactly the set project.set_settings
// accepts (Requirement 7.1 — "the same ranges project.create accepts"), spelled
// with the core's own stable display names. `Unknown` is deliberately absent, as
// it is not a selectable working space anywhere else in the tree.
const std::vector<ColorSpace>& selectableColorSpaces() {
    static const std::vector<ColorSpace> spaces = {
        ColorSpace::Srgb,       ColorSpace::Rec709,     ColorSpace::Rec2020,
        ColorSpace::Rec2100Pq,  ColorSpace::Rec2100Hlg, ColorSpace::DisplayP3,
        ColorSpace::LinearSrgb};
    return spaces;
}

}  // namespace

ProjectSettingsDialog::ProjectSettingsDialog(GuiToolGateway& gateway, FrameRate currentFps,
                                             Resolution currentCanvas,
                                             ColorSpace currentColorSpace, QWidget* parent)
    : QDialog(parent),
      gateway_(gateway),
      currentFps_(currentFps),
      currentCanvas_(currentCanvas),
      currentColorSpace_(currentColorSpace),
      lastAppliedFpsValue_(currentFps.toDouble()) {
    setWindowTitle(QStringLiteral("Project Settings"));
    buildLayout();
}

ProjectSettingsDialog::~ProjectSettingsDialog() = default;

void ProjectSettingsDialog::buildLayout() {
    auto* rootLayout = new QVBoxLayout(this);
    auto* form = new QFormLayout();

    // Requirement 7.2: every field is seeded from the current, live value.
    fpsSpin_ = new QDoubleSpinBox(this);
    fpsSpin_->setRange(1.0, 240.0);  // the same range project.set_settings enforces
    fpsSpin_->setDecimals(3);
    fpsSpin_->setValue(currentFps_.toDouble());
    form->addRow(QStringLiteral("Frame rate (fps):"), fpsSpin_);

    auto* canvasRow = new QWidget(this);
    auto* canvasLayout = new QHBoxLayout(canvasRow);
    canvasLayout->setContentsMargins(0, 0, 0, 0);
    widthSpin_ = new QSpinBox(canvasRow);
    widthSpin_->setRange(16, 7680);
    widthSpin_->setValue(static_cast<int>(currentCanvas_.width));
    heightSpin_ = new QSpinBox(canvasRow);
    heightSpin_->setRange(16, 4320);
    heightSpin_->setValue(static_cast<int>(currentCanvas_.height));
    canvasLayout->addWidget(widthSpin_);
    canvasLayout->addWidget(new QLabel(QStringLiteral("x"), canvasRow));
    canvasLayout->addWidget(heightSpin_);
    form->addRow(QStringLiteral("Canvas (pixels):"), canvasRow);

    colorSpaceCombo_ = new QComboBox(this);
    int selectedIndex = 0;
    for (std::size_t i = 0; i < selectableColorSpaces().size(); ++i) {
        const ColorSpace cs = selectableColorSpaces()[i];
        const std::string_view name = toStringView(cs);
        colorSpaceCombo_->addItem(
            QString::fromUtf8(name.data(), static_cast<int>(name.size())));
        if (cs == currentColorSpace_) {
            selectedIndex = static_cast<int>(i);
        }
    }
    colorSpaceCombo_->setCurrentIndex(selectedIndex);
    form->addRow(QStringLiteral("Colour space:"), colorSpaceCombo_);

    rootLayout->addLayout(form);

    statusLabel_ = new QLabel(this);
    statusLabel_->setWordWrap(true);
    rootLayout->addWidget(statusLabel_);

    applyButton_ = new QPushButton(QStringLiteral("Apply"), this);
    connect(applyButton_, &QPushButton::clicked, this, &ProjectSettingsDialog::onApplyClicked);
    rootLayout->addWidget(applyButton_);
}

void ProjectSettingsDialog::onApplyClicked() {
    // Only the fields that actually differ from the value the dialog was opened
    // with are submitted, so an Apply that touches nothing is not sent as a
    // (still-valid, but pointless) request naming every field.
    std::optional<double> fps;
    if (fpsSpin_ != nullptr && fpsSpin_->value() != lastAppliedFpsValue_) {
        fps = fpsSpin_->value();
    }
    std::optional<std::uint32_t> width;
    std::optional<std::uint32_t> height;
    if (widthSpin_ != nullptr && heightSpin_ != nullptr &&
        (static_cast<std::uint32_t>(widthSpin_->value()) != currentCanvas_.width ||
         static_cast<std::uint32_t>(heightSpin_->value()) != currentCanvas_.height)) {
        width = static_cast<std::uint32_t>(widthSpin_->value());
        height = static_cast<std::uint32_t>(heightSpin_->value());
    }
    std::optional<std::string> colorSpace;
    if (colorSpaceCombo_ != nullptr) {
        const int index = colorSpaceCombo_->currentIndex();
        if (index >= 0 && static_cast<std::size_t>(index) < selectableColorSpaces().size()) {
            const ColorSpace selected = selectableColorSpaces()[static_cast<std::size_t>(index)];
            if (selected != currentColorSpace_) {
                colorSpace = std::string(toStringView(selected));
            }
        }
    }

    if (!fps && !width && !colorSpace) {
        statusLabel_->setText(QStringLiteral("Nothing changed."));
        return;
    }

    const Result<Json> result =
        gateway_.setProjectSettings(fps, width, height, colorSpace);
    if (result.isError()) {
        statusLabel_->setText(
            QStringLiteral("Refused: %1")
                .arg(QString::fromStdString(result.error().toString())));
        return;
    }

    // Requirement 7.2/7.4: the change already applied through the gateway (one
    // undoable edit); this dialog's own "current" baseline advances to match, so
    // a second Apply without further edits reports "Nothing changed" rather than
    // resubmitting the same values. fps is tracked as the raw value the user
    // entered rather than round-tripped through FrameRate, since the exact
    // rational this fps snaps to (e.g. NTSC pull-down handling) is a Tool_Surface
    // decision this dialog has no reason to duplicate.
    if (fps) lastAppliedFpsValue_ = *fps;
    if (width && height) currentCanvas_ = Resolution{*width, *height};
    if (colorSpace) {
        for (const ColorSpace candidate : selectableColorSpaces()) {
            if (toStringView(candidate) == *colorSpace) {
                currentColorSpace_ = candidate;
                break;
            }
        }
    }
    statusLabel_->setText(QStringLiteral("Applied."));
}

}  // namespace palmier::ui

#endif  // PALMIER_HAVE_QT
