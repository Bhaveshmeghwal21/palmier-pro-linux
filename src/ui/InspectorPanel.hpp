// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/InspectorPanel.hpp — the Qt 6 widget surface for the Inspector/Effects panel
// (task 19.4).
//
// This is the thin presentation layer over the Qt-free InspectorViewModel: it
// renders the selected clip's properties (opacity, gain, trimmed boundaries) and
// its effect chain, and forwards user gestures (edit a value, add an effect,
// change an effect parameter) to the view model, which maps them onto the
// undoable EditCommand path on the TimelineEngine. All editing logic lives in the
// view model; this class only builds/refreshes controls and relays signals, so
// the behavior stays fully unit-testable without Qt.
//
// The whole translation unit is guarded by PALMIER_HAVE_QT (mirroring
// MainWindow) so the module tree still configures and builds where Qt 6 is not
// installed; the compiled widget is produced only when Qt is found.

#ifndef PALMIER_UI_INSPECTORPANEL_HPP
#define PALMIER_UI_INSPECTORPANEL_HPP

#ifdef PALMIER_HAVE_QT

#include <QWidget>

#include "ui/InspectorViewModel.hpp"

class QFormLayout;
class QVBoxLayout;
class QLabel;
class QDoubleSpinBox;

namespace palmier::ui {

/// The Inspector/Effects dock widget. Owns no editing logic: it observes an
/// InspectorViewModel (via setOnChanged) and rebuilds its controls from the
/// model's projection, forwarding user edits back to the model.
class InspectorPanel : public QWidget {
    Q_OBJECT

public:
    explicit InspectorPanel(InspectorViewModel& model, QWidget* parent = nullptr);
    ~InspectorPanel() override;

private:
    void rebuild();          ///< Rebuild all controls from the current projection.
    void buildEmptyState();  ///< Show the "no clip selected" placeholder.

    InspectorViewModel& model_;

    QVBoxLayout*    rootLayout_ = nullptr;
    QLabel*         header_ = nullptr;
    QFormLayout*    propertiesLayout_ = nullptr;
    QDoubleSpinBox* opacitySpin_ = nullptr;
    QDoubleSpinBox* gainSpin_ = nullptr;
    QWidget*        effectsContainer_ = nullptr;

    bool refreshing_ = false;  ///< Guards against feedback while repopulating.
};

} // namespace palmier::ui

#endif // PALMIER_HAVE_QT

#endif // PALMIER_UI_INSPECTORPANEL_HPP
