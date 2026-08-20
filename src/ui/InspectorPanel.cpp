// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/InspectorPanel.cpp — implementation of the Qt 6 Inspector/Effects panel
// (task 19.4). Compiled only when Qt 6 is available (PALMIER_HAVE_QT).
//
// The panel is intentionally thin: it renders the InspectorViewModel's read
// projection and relays user edits back to the model. Every mutation the user
// makes here (opacity, gain, an effect parameter, adding an effect) is applied by
// the model as an undoable EditCommand on the TimelineEngine, so the panel itself
// contains no editing or validation logic — that all lives in (and is tested
// through) the Qt-free view model.

#include "ui/InspectorPanel.hpp"

#ifdef PALMIER_HAVE_QT

#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QLabel>
#include <QPushButton>
#include <QString>
#include <QVBoxLayout>

#include <optional>

namespace palmier::ui {

namespace {

QString effectTypeName(EffectType type) {
    switch (type) {
        case EffectType::Brightness:    return QStringLiteral("Brightness");
        case EffectType::Contrast:      return QStringLiteral("Contrast");
        case EffectType::Blur:          return QStringLiteral("Blur");
        case EffectType::CropTransform: return QStringLiteral("Crop / Transform");
        case EffectType::ColorGrade:    return QStringLiteral("Color Grade");
        case EffectType::InvertColors:  return QStringLiteral("Invert Colors");
        case EffectType::Custom:        return QStringLiteral("Custom");
    }
    return QStringLiteral("Effect");
}

} // namespace

InspectorPanel::InspectorPanel(InspectorViewModel& model, QWidget* parent)
    : QWidget(parent), model_(model) {
    rootLayout_ = new QVBoxLayout(this);
    header_ = new QLabel(this);
    header_->setObjectName(QStringLiteral("inspectorHeader"));
    rootLayout_->addWidget(header_);

    // Repaint whenever the model reports the projection may have changed (a
    // selection change, an Inspector edit, or an external undo/redo/MCP/agent edit).
    model_.setOnChanged([this] {
        if (!refreshing_) {
            rebuild();
        }
    });

    rebuild();
}

InspectorPanel::~InspectorPanel() {
    // Detach our callback so a late notification cannot reach a destroyed panel.
    model_.setOnChanged({});
}

void InspectorPanel::buildEmptyState() {
    header_->setText(QStringLiteral("No clip selected"));
}

void InspectorPanel::rebuild() {
    refreshing_ = true;

    // Clear any previously built dynamic controls (everything after the header).
    while (rootLayout_->count() > 1) {
        QLayoutItem* item = rootLayout_->takeAt(1);
        if (QWidget* w = item->widget()) {
            w->deleteLater();
        }
        delete item;
    }
    propertiesLayout_ = nullptr;
    opacitySpin_ = nullptr;
    gainSpin_ = nullptr;
    effectsContainer_ = nullptr;

    const std::optional<ClipInspectorView> view = model_.selectedClip();
    if (!view) {
        buildEmptyState();
        refreshing_ = false;
        return;
    }

    header_->setText(QStringLiteral("Clip %1").arg(QString::fromStdString(view->id.toString())));

    // --- Clip properties ---------------------------------------------------
    propertiesLayout_ = new QFormLayout();
    rootLayout_->addLayout(propertiesLayout_);

    opacitySpin_ = new QDoubleSpinBox(this);
    opacitySpin_->setRange(0.0, 1.0);
    opacitySpin_->setSingleStep(0.05);
    opacitySpin_->setValue(view->opacity);
    QObject::connect(opacitySpin_, &QDoubleSpinBox::editingFinished, this, [this] {
        if (opacitySpin_ != nullptr) {
            (void)model_.setOpacity(opacitySpin_->value());
        }
    });
    propertiesLayout_->addRow(QStringLiteral("Opacity"), opacitySpin_);

    gainSpin_ = new QDoubleSpinBox(this);
    gainSpin_->setRange(0.0, 8.0);
    gainSpin_->setSingleStep(0.1);
    gainSpin_->setValue(view->gain);
    QObject::connect(gainSpin_, &QDoubleSpinBox::editingFinished, this, [this] {
        if (gainSpin_ != nullptr) {
            (void)model_.setGain(gainSpin_->value());
        }
    });
    propertiesLayout_->addRow(QStringLiteral("Gain"), gainSpin_);

    // --- Effect chain ------------------------------------------------------
    effectsContainer_ = new QWidget(this);
    auto* effectsLayout = new QVBoxLayout(effectsContainer_);
    effectsLayout->addWidget(new QLabel(QStringLiteral("Effects"), effectsContainer_));

    for (const EffectView& effect : view->effects) {
        auto* frame = new QFrame(effectsContainer_);
        frame->setFrameShape(QFrame::StyledPanel);
        auto* form = new QFormLayout(frame);
        form->addRow(new QLabel(effectTypeName(effect.type), frame));

        const Uuid effectId = effect.id;
        for (const EffectParameterView& param : effect.parameters) {
            auto* spin = new QDoubleSpinBox(frame);
            spin->setRange(-1000.0, 1000.0);
            spin->setDecimals(4);
            spin->setValue(param.value);
            const QString name = QString::fromStdString(param.name);
            QObject::connect(spin, &QDoubleSpinBox::editingFinished, this,
                             [this, effectId, name, spin] {
                                 (void)model_.setEffectParameter(
                                     effectId, name.toStdString(), spin->value());
                             });
            form->addRow(name, spin);
        }
        effectsLayout->addWidget(frame);
    }

    auto* addBrightness = new QPushButton(QStringLiteral("Add Brightness"), effectsContainer_);
    QObject::connect(addBrightness, &QPushButton::clicked, this,
                     [this] { (void)model_.addBrightnessEffect(0.0); });
    effectsLayout->addWidget(addBrightness);

    rootLayout_->addWidget(effectsContainer_);
    rootLayout_->addStretch(1);

    refreshing_ = false;
}

} // namespace palmier::ui

#endif // PALMIER_HAVE_QT
