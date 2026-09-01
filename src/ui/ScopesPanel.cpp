// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/ScopesPanel.cpp — implementation of the Qt 6 scopes surface. See the header for why
// the cadence and empty-state rules are not here.

#include "ui/ScopesPanel.hpp"

#ifdef PALMIER_HAVE_QT

#include <algorithm>
#include <cstdint>

#include <QCheckBox>
#include <QColor>
#include <QPainter>
#include <QPointF>
#include <QRect>
#include <QSettings>
#include <QString>
#include <QVBoxLayout>

namespace palmier::ui {
namespace {

constexpr int kScopeHeight = 96;
constexpr int kScopeSpacing = 6;
constexpr int kPreferredWidth = 260;

const QColor kBackgroundColour{0x14, 0x14, 0x14};
const QColor kGridColour{0x30, 0x30, 0x30};
const QColor kRedColour{0xd0, 0x50, 0x50};
const QColor kGreenColour{0x50, 0xc0, 0x60};
const QColor kBlueColour{0x50, 0x80, 0xd0};
const QColor kLumaColour{0xd8, 0xd8, 0xd8};
const QColor kEmptyTextColour{0x80, 0x80, 0x80};

/// One channel's histogram as a filled polyline, scaled to the tallest bin of any channel
/// so the four traces stay comparable -- scaling each to its own peak would make a flat
/// channel look as busy as a peaky one.
void drawHistogramChannel(QPainter& painter, const QRect& area,
                          const std::array<std::uint32_t, gpu::kScopeLevels>& bins,
                          std::uint32_t peak, const QColor& colour) {
    if (peak == 0) {
        return;
    }
    painter.setPen(colour);
    for (int i = 0; i < gpu::kScopeLevels; ++i) {
        const double fraction = static_cast<double>(bins[static_cast<std::size_t>(i)]) /
                                static_cast<double>(peak);
        const double x = area.left() + static_cast<double>(i) / (gpu::kScopeLevels - 1) *
                                          static_cast<double>(area.width());
        const double h = fraction * static_cast<double>(area.height());
        painter.drawLine(QPointF{x, static_cast<double>(area.bottom())},
                         QPointF{x, static_cast<double>(area.bottom()) - h});
    }
}

}  // namespace

ScopesPanel::ScopesPanel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(kScopeSpacing);

    for (const ScopeKind kind : kScopeKinds) {
        auto* box = new QCheckBox(QString::fromUtf8(scopeKindName(kind)), this);
        box->setChecked(model_.isVisible(kind));
        QObject::connect(box, &QCheckBox::toggled, this, [this, kind](bool on) {
            model_.setVisible(kind, on);
            saveVisibility();
            update();
        });
        toggles_[static_cast<std::size_t>(kind)] = box;
        layout->addWidget(box);
    }
    layout->addStretch(1);

    loadVisibility();
    setMinimumWidth(160);
}

ScopesPanel::~ScopesPanel() = default;

void ScopesPanel::setPreviewFrameInterval(std::chrono::microseconds interval) {
    previewFrameInterval_ = interval;
}

void ScopesPanel::observeFrame(const gpu::RenderedFrame& frame) {
    const ScopeBudget budget{.lastCost = model_.lastCost(),
                             .previewFrameInterval = previewFrameInterval_};
    const auto now = std::chrono::steady_clock::now();
    if (!model_.shouldRecompute(now, budget)) {
        return;
    }

    const auto* pixels = static_cast<const std::uint8_t*>(frame.hostData());
    // Measure the real cost rather than estimating it: a hardcoded figure would be wrong on
    // every machine but the one it was measured on, and the budget is only meaningful if it
    // reflects this host. Computed ONCE -- calling update() a second time to record the cost
    // would compute every scope twice and spend double the budget it exists to protect.
    const auto started = std::chrono::steady_clock::now();
    model_.update(pixels, static_cast<int>(frame.width()), static_cast<int>(frame.height()), now,
                  model_.lastCost());
    model_.recordCost(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started));
    update();
}

void ScopesPanel::clearFrame() {
    model_.clear();
    update();
}

void ScopesPanel::setScopeVisible(ScopeKind kind, bool visible) {
    model_.setVisible(kind, visible);
    syncCheckBoxes();
    update();
}

bool ScopesPanel::isScopeVisible(ScopeKind kind) const { return model_.isVisible(kind); }

void ScopesPanel::loadVisibility() {
    QSettings settings;
    for (const ScopeKind kind : kScopeKinds) {
        const QString key = QString::fromStdString(scopeVisibilitySettingsKey(kind));
        // Default true, so a first run shows all three rather than an empty panel the user
        // has to discover how to populate.
        model_.setVisible(kind, settings.value(key, true).toBool());
    }
    syncCheckBoxes();
}

void ScopesPanel::saveVisibility() const {
    QSettings settings;
    for (const ScopeKind kind : kScopeKinds) {
        settings.setValue(QString::fromStdString(scopeVisibilitySettingsKey(kind)),
                          model_.isVisible(kind));
    }
}

void ScopesPanel::syncCheckBoxes() {
    for (const ScopeKind kind : kScopeKinds) {
        QCheckBox* box = toggles_[static_cast<std::size_t>(kind)];
        if (box == nullptr) {
            continue;
        }
        const bool wanted = model_.isVisible(kind);
        if (box->isChecked() != wanted) {
            // Block signals: setChecked would otherwise re-enter the toggled handler and
            // write the setting we are in the middle of reading.
            const bool blocked = box->blockSignals(true);
            box->setChecked(wanted);
            box->blockSignals(blocked);
        }
    }
}

void ScopesPanel::paintEvent(QPaintEvent* event) {
    QWidget::paintEvent(event);
    QPainter painter(this);

    // The plots occupy whatever the checkboxes leave. Laid out by hand rather than as child
    // widgets because each is a single custom trace, not a reusable control.
    int top = 0;
    for (const ScopeKind kind : kScopeKinds) {
        QCheckBox* box = toggles_[static_cast<std::size_t>(kind)];
        top = std::max(top, box == nullptr ? 0 : box->geometry().bottom() + kScopeSpacing);
    }

    for (const ScopeKind kind : kScopeKinds) {
        if (!model_.isVisible(kind)) {
            continue;
        }
        const QRect area{4, top, std::max(1, width() - 8), kScopeHeight};
        top += kScopeHeight + kScopeSpacing;
        painter.fillRect(area, kBackgroundColour);

        // Requirement 6.6: say so, rather than leaving an empty box that looks like a
        // black frame.
        if (!model_.hasFrame()) {
            painter.setPen(kEmptyTextColour);
            painter.drawText(area, Qt::AlignCenter, QStringLiteral("No frame"));
            continue;
        }

        painter.setPen(kGridColour);
        for (int i = 1; i < 4; ++i) {
            const int x = area.left() + area.width() * i / 4;
            painter.drawLine(x, area.top(), x, area.bottom());
        }

        switch (kind) {
            case ScopeKind::Histogram: {
                const gpu::Histogram& h = model_.histogram();
                const std::uint32_t peak = h.peakCount();
                drawHistogramChannel(painter, area, h.red, peak, kRedColour);
                drawHistogramChannel(painter, area, h.green, peak, kGreenColour);
                drawHistogramChannel(painter, area, h.blue, peak, kBlueColour);
                drawHistogramChannel(painter, area, h.luma, peak, kLumaColour);
                break;
            }
            case ScopeKind::Waveform: {
                const gpu::LumaWaveform& w = model_.waveform();
                const std::uint32_t peak = w.peakCount();
                if (peak == 0 || w.columns <= 0) {
                    break;
                }
                for (int column = 0; column < w.columns; ++column) {
                    const int x = area.left() + area.width() * column / w.columns;
                    for (int level = 0; level < gpu::kScopeLevels; ++level) {
                        const std::uint32_t count = w.at(column, level);
                        if (count == 0) {
                            continue;
                        }
                        // Brightness by density, so a trace with many pixels at one level
                        // reads as solid and an outlier reads as faint -- which is how a
                        // waveform monitor is read.
                        const int alpha = static_cast<int>(
                            40 + 215.0 * static_cast<double>(count) / static_cast<double>(peak));
                        QColor c = kLumaColour;
                        c.setAlpha(std::min(255, alpha));
                        painter.setPen(c);
                        const int y = area.bottom() -
                                      area.height() * level / (gpu::kScopeLevels - 1);
                        painter.drawPoint(x, y);
                    }
                }
                break;
            }
            case ScopeKind::Vectorscope: {
                const gpu::Vectorscope& v = model_.vectorscope();
                const std::uint32_t peak = v.peakCount();
                if (peak == 0) {
                    break;
                }
                // Square, centred: Cb horizontal, Cr vertical and INVERTED, so increasing
                // Cr (toward red) points up as it does on a real vectorscope.
                const int side = std::min(area.width(), area.height());
                const int left = area.left() + (area.width() - side) / 2;
                for (int cr = 0; cr < gpu::kScopeLevels; ++cr) {
                    for (int cb = 0; cb < gpu::kScopeLevels; ++cb) {
                        const std::uint32_t count = v.at(cb, cr);
                        if (count == 0) {
                            continue;
                        }
                        const int alpha = static_cast<int>(
                            60 + 195.0 * static_cast<double>(count) / static_cast<double>(peak));
                        QColor c = kGreenColour;
                        c.setAlpha(std::min(255, alpha));
                        painter.setPen(c);
                        const int x = left + side * cb / (gpu::kScopeLevels - 1);
                        const int y = area.top() + side -
                                      side * cr / (gpu::kScopeLevels - 1);
                        painter.drawPoint(x, y);
                    }
                }
                break;
            }
        }
    }
}

}  // namespace palmier::ui

#endif  // PALMIER_HAVE_QT
