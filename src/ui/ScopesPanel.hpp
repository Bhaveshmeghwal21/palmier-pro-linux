// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/ScopesPanel.hpp — the Qt 6 histogram, waveform and vectorscope surface
// (monitoring-and-grading task 6; Requirement 6.1, 6.2, 6.3, 6.6, 6.7).
//
// Thin over ui::ScopesViewModel, exactly as ui::AudioMeterWidget is thin over
// ui::AudioMeterViewModel: the cadence rules, the empty state and per-scope visibility all
// live in the Qt-free view model, and this file paints them and owns the three checkboxes.
//
// Requirement 6.2 is satisfied by WHERE the frames come from, not by anything here: the
// shell installs this panel's observeFrame() as ui::PreviewView's frame observer, so the
// buffer measured is the very one the Preview uploads. A panel that rendered its own frame
// could disagree with the picture beside it, which is what 6.2 forbids.
//
// The three visibility toggles are CHECKBOXES ON THE PANEL rather than menu actions. A new
// menu would shift the menu bar's indices, which several shell tests address positionally,
// and a scope's visibility belongs beside the scope. They persist through QSettings under
// the keys ui::scopeVisibilitySettingsKey() supplies (Requirement 6.7).
//
// The whole translation unit is behind PALMIER_HAVE_QT, matching every other Qt surface
// here, so the tree still configures where Qt 6 is absent.

#ifndef PALMIER_UI_SCOPESPANEL_HPP
#define PALMIER_UI_SCOPESPANEL_HPP

#ifdef PALMIER_HAVE_QT

#include <array>
#include <chrono>

#include <QWidget>

#include "gpu/Compositor.hpp"
#include "ui/ScopesViewModel.hpp"

class QCheckBox;
class QPaintEvent;

namespace palmier::ui {

/// The three scopes, stacked, each individually hideable.
class ScopesPanel : public QWidget {
    Q_OBJECT

public:
    explicit ScopesPanel(QWidget* parent = nullptr);
    ~ScopesPanel() override;

    /// Measure one composited frame. Installed as ui::PreviewView's frame observer, so this
    /// is the Requirement 6.2 seam.
    ///
    /// Honours the view model's cadence decision, and MEASURES its own cost to feed back as
    /// the next decision's budget — the budget has to come from real work, since a
    /// hardcoded estimate would be wrong on every machine but the one it was measured on.
    void observeFrame(const gpu::RenderedFrame& frame);

    /// Drop the current reading: no project, or the playhead past the end (Requirement 6.6).
    void clearFrame();

    /// The Preview's frame interval, which sets the 10 percent budget. Zero while paused,
    /// when there is no presented frame rate to protect.
    void setPreviewFrameInterval(std::chrono::microseconds interval);

    void setScopeVisible(ScopeKind kind, bool visible);
    [[nodiscard]] bool isScopeVisible(ScopeKind kind) const;

    /// Load and store the three visibility flags (Requirement 6.7).
    void loadVisibility();
    void saveVisibility() const;

    /// The view model, for tests that assert readings without synthesising paint events.
    [[nodiscard]] const ScopesViewModel& model() const noexcept { return model_; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void syncCheckBoxes();

    ScopesViewModel           model_;
    std::chrono::microseconds previewFrameInterval_{0};
    std::array<QCheckBox*, 3> toggles_{nullptr, nullptr, nullptr};
};

}  // namespace palmier::ui

#endif  // PALMIER_HAVE_QT
#endif  // PALMIER_UI_SCOPESPANEL_HPP
