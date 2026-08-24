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
#include <QComboBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QString>
#include <QVBoxLayout>

#include <optional>
#include <utility>
#include <vector>

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

/// The three TextAlignment values, in the fixed order textAlignmentCombo_'s
/// index maps onto (Requirement 9.4). Kept local to this translation unit since
/// nothing outside the panel needs a Qt-facing ordering of the enum.
constexpr TextAlignment kAlignmentOrder[] = {TextAlignment::Left, TextAlignment::Center,
                                             TextAlignment::Right};

QString alignmentLabel(TextAlignment alignment) {
    switch (alignment) {
        case TextAlignment::Left:   return QStringLiteral("Left");
        case TextAlignment::Center: return QStringLiteral("Center");
        case TextAlignment::Right:  return QStringLiteral("Right");
    }
    return QStringLiteral("Center");
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

void InspectorPanel::moveEffect(std::size_t from, std::size_t to) {
    const std::optional<ClipInspectorView> view = model_.selectedClip();
    if (!view || from >= view->effects.size() || to >= view->effects.size() || from == to) {
        return;  // the selection or the chain changed since the button was drawn
    }
    std::vector<Uuid> order;
    order.reserve(view->effects.size());
    for (const EffectView& effect : view->effects) {
        order.push_back(effect.id);
    }
    std::swap(order[from], order[to]);
    (void)model_.reorderEffects(std::move(order));
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
    textContainer_ = nullptr;
    textContentEdit_ = nullptr;
    textPointSizeSpin_ = nullptr;
    textAlignmentCombo_ = nullptr;
    captionContainer_ = nullptr;
    captionTextEdit_ = nullptr;

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

    // --- Text and titles (usable-editor task 12; Requirement 9.4) ----------
    // Present only while the selected clip carries a TextStyle.
    if (view->textStyle.has_value()) {
        const TextStyleView& text = *view->textStyle;

        textContainer_ = new QWidget(this);
        auto* textForm = new QFormLayout(textContainer_);
        textForm->addRow(new QLabel(QStringLiteral("Text"), textContainer_));

        textContentEdit_ = new QLineEdit(textContainer_);
        textContentEdit_->setText(QString::fromStdString(text.content));
        QObject::connect(textContentEdit_, &QLineEdit::editingFinished, this, [this] {
            if (textContentEdit_ != nullptr) {
                (void)model_.setTextContent(textContentEdit_->text().toStdString());
            }
        });
        textForm->addRow(QStringLiteral("Content"), textContentEdit_);

        textPointSizeSpin_ = new QDoubleSpinBox(textContainer_);
        textPointSizeSpin_->setRange(1.0, 1000.0);
        textPointSizeSpin_->setSingleStep(1.0);
        textPointSizeSpin_->setValue(text.pointSize);
        QObject::connect(textPointSizeSpin_, &QDoubleSpinBox::editingFinished, this, [this] {
            if (textPointSizeSpin_ != nullptr) {
                (void)model_.setTextStyle(std::nullopt, textPointSizeSpin_->value(),
                                          std::nullopt, std::nullopt, std::nullopt,
                                          std::nullopt, std::nullopt, std::nullopt,
                                          std::nullopt);
            }
        });
        textForm->addRow(QStringLiteral("Point size"), textPointSizeSpin_);

        textAlignmentCombo_ = new QComboBox(textContainer_);
        int currentIndex = 1;  // Center, matching TextStyle's own default.
        for (int i = 0; i < 3; ++i) {
            textAlignmentCombo_->addItem(alignmentLabel(kAlignmentOrder[i]));
            if (kAlignmentOrder[i] == text.alignment) {
                currentIndex = i;
            }
        }
        textAlignmentCombo_->setCurrentIndex(currentIndex);
        // Qt6's QComboBox::currentIndexChanged has only the int overload (the
        // QString overload Qt5 also had is gone), so no disambiguating cast is
        // needed here, unlike some other Qt5-era signal/slot connections.
        QObject::connect(textAlignmentCombo_, &QComboBox::currentIndexChanged, this,
                         [this](int index) {
                             if (index < 0 || index >= 3) {
                                 return;
                             }
                             (void)model_.setTextStyle(std::nullopt, std::nullopt, std::nullopt,
                                                       std::nullopt, std::nullopt, std::nullopt,
                                                       kAlignmentOrder[index], std::nullopt,
                                                       std::nullopt);
                         });
        textForm->addRow(QStringLiteral("Alignment"), textAlignmentCombo_);

        rootLayout_->addWidget(textContainer_);
    }

    // --- Captions and transcription (usable-editor task 13; Requirement 10.2)
    // Present only while the selected clip carries a captionText — no styling
    // controls, since a caption cue carries none of its own.
    if (view->captionText.has_value()) {
        captionContainer_ = new QWidget(this);
        auto* captionForm = new QFormLayout(captionContainer_);
        captionForm->addRow(new QLabel(QStringLiteral("Caption"), captionContainer_));

        captionTextEdit_ = new QLineEdit(captionContainer_);
        captionTextEdit_->setText(QString::fromStdString(*view->captionText));
        QObject::connect(captionTextEdit_, &QLineEdit::editingFinished, this, [this] {
            if (captionTextEdit_ != nullptr) {
                (void)model_.setCaptionText(captionTextEdit_->text().toStdString());
            }
        });
        captionForm->addRow(QStringLiteral("Text"), captionTextEdit_);

        rootLayout_->addWidget(captionContainer_);
    }

    // --- Effect chain ------------------------------------------------------
    effectsContainer_ = new QWidget(this);
    auto* effectsLayout = new QVBoxLayout(effectsContainer_);
    effectsLayout->addWidget(new QLabel(QStringLiteral("Effects"), effectsContainer_));

    for (std::size_t position = 0; position < view->effects.size(); ++position) {
        const EffectView& effect = view->effects[position];
        auto* frame = new QFrame(effectsContainer_);
        frame->setFrameShape(QFrame::StyledPanel);
        auto* form = new QFormLayout(frame);

        // Header row: the effect's type name plus remove/reorder controls
        // (task 9.2; Requirement 6.1, 6.2).
        auto* header = new QWidget(frame);
        auto* headerLayout = new QHBoxLayout(header);
        headerLayout->setContentsMargins(0, 0, 0, 0);
        headerLayout->addWidget(new QLabel(effectTypeName(effect.type), header));
        headerLayout->addStretch(1);

        const Uuid effectId = effect.id;

        // Reordering swaps this effect with its neighbour and submits the whole
        // chain's ids in the new order — the interaction a two-button up/down
        // control offers, mapped onto ReorderEffectsCommand's full-permutation
        // argument (Requirement 6.4: order is what changes rendered output).
        auto* upButton = new QPushButton(QStringLiteral("\u2191"), header);
        upButton->setEnabled(position > 0);
        upButton->setToolTip(QStringLiteral("Move up (changes render order)"));
        QObject::connect(upButton, &QPushButton::clicked, this, [this, position] {
            moveEffect(position, position - 1);
        });
        headerLayout->addWidget(upButton);

        auto* downButton = new QPushButton(QStringLiteral("\u2193"), header);
        downButton->setEnabled(position + 1 < view->effects.size());
        downButton->setToolTip(QStringLiteral("Move down (changes render order)"));
        QObject::connect(downButton, &QPushButton::clicked, this, [this, position] {
            moveEffect(position, position + 1);
        });
        headerLayout->addWidget(downButton);

        auto* removeButton = new QPushButton(QStringLiteral("Remove"), header);
        QObject::connect(removeButton, &QPushButton::clicked, this,
                         [this, effectId] { (void)model_.removeEffect(effectId); });
        headerLayout->addWidget(removeButton);

        form->addRow(header);

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
