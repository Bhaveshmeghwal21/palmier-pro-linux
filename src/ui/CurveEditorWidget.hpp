// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/CurveEditorWidget.hpp — the thin Qt 6 tone-curve surface
// (monitoring-and-grading task 5; Requirement 5.9).
//
// Deliberately minimal, exactly as ui::AudioMeterWidget is minimal over
// ui::AudioMeterViewModel: every rule this control obeys — how a pixel maps to a
// curve coordinate, which point the pointer has grabbed, and when a gesture
// becomes one undoable edit — lives in the Qt-free ui::CurveEditorViewModel this
// widget owns. The widget only (1) paints the transfer function and the points,
// (2) turns mouse events into gesture calls, and (3) hands the resulting request
// to an injected applier.
//
// The applier is a std::function seam rather than a reference to the Inspector or
// the composition root, so this file depends on neither: the shell supplies one
// closure that issues the undoable command. That is also what keeps the widget
// from being able to edit a project directly.
//
// The whole translation unit is behind PALMIER_HAVE_QT, matching every other Qt
// surface in this directory, so the tree still configures where Qt 6 is absent.

#ifndef PALMIER_UI_CURVEEDITORWIDGET_HPP
#define PALMIER_UI_CURVEEDITORWIDGET_HPP

#ifdef PALMIER_HAVE_QT

#include <functional>
#include <map>
#include <string>

#include <QWidget>

#include "core/ToneCurve.hpp"
#include "ui/CurveEditorViewModel.hpp"

class QMouseEvent;
class QPaintEvent;

namespace palmier::ui {

/// A directly editable plot of one channel's tone curve.
class CurveEditorWidget : public QWidget {
    Q_OBJECT

public:
    /// Performs the edit a gesture asked for. Returning nothing is deliberate: the
    /// widget does not need to know whether the edit succeeded, because the project's
    /// own change notification is what refreshes it, and that path is the same whether
    /// the edit came from here, from an undo or from MCP.
    using RequestApplier = std::function<void(CurveChannel, const CurveEditRequest&)>;

    explicit CurveEditorWidget(QWidget* parent = nullptr);
    ~CurveEditorWidget() override;

    /// Install the applier. Left unset, the control is read-only rather than broken:
    /// gestures still track locally but nothing is committed.
    void setRequestApplier(RequestApplier applier);

    /// Choose which channel is edited and shown.
    void setChannel(CurveChannel channel);
    [[nodiscard]] CurveChannel channel() const noexcept { return model_.channel(); }

    /// Re-read the points from an effect's parameters. Called whenever the project
    /// changes, which is what makes an undo or an MCP edit appear here.
    void setParameters(const std::map<std::string, double>& parameters);

    /// The view model, for tests that drive gestures without synthesising QMouseEvents.
    [[nodiscard]] const CurveEditorViewModel& model() const noexcept { return model_; }

    [[nodiscard]] QSize minimumSizeHint() const override;
    [[nodiscard]] QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    /// Hand a request to the applier unless it is a no-op or no applier is installed.
    void apply(const CurveEditRequest& request);

    CurveEditorViewModel model_;
    RequestApplier       applier_;
};

}  // namespace palmier::ui

#endif  // PALMIER_HAVE_QT
#endif  // PALMIER_UI_CURVEEDITORWIDGET_HPP
