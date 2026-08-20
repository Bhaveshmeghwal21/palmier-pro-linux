// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/MediaBrowserPanel.hpp — the Qt 6 Media Browser panel (task 19.5).
//
// This is the thin QWidget surface over MediaBrowserViewModel. It owns no panel
// logic: every decision (what to import, which library rows to show, a clip's
// selectable versions, and the key-moment markers / "no key moments" indication)
// lives in the Qt-free view model, which is unit-tested without Qt. This class
// only renders that model's output into widgets and forwards user actions back
// into it, so the behaviour under test and the behaviour on screen are the same.
//
// The whole translation unit is guarded by PALMIER_HAVE_QT (mirroring
// MainWindow), so the module tree still configures and builds where Qt 6 is not
// installed; the compiled panel is produced only when Qt is found.

#ifndef PALMIER_UI_MEDIABROWSERPANEL_HPP
#define PALMIER_UI_MEDIABROWSERPANEL_HPP

#ifdef PALMIER_HAVE_QT

#include <QWidget>

#include "core/Clip.hpp"  // ClipId
#include "ui/MediaBrowserViewModel.hpp"

class QLabel;
class QListWidget;
class QPushButton;

namespace palmier {
class MediaManager;
}  // namespace palmier

namespace palmier::ui {

class GuiToolGateway;  // ui/GuiToolGateway.hpp — optional gateway-backed import.

/// The Media Browser panel: an import control, the media library list, the
/// selected clip's selectable version list, and the clip's key-moment marker
/// summary. All state comes from an owned MediaBrowserViewModel built over the
/// project's MediaManager and KeyMomentMarkerModel (which must outlive the panel).
class MediaBrowserPanel : public QWidget {
    Q_OBJECT

public:
    MediaBrowserPanel(MediaManager& media, services::KeyMomentMarkerModel& markers,
                      MediaBrowserViewModel::ImportValidator validator,
                      QWidget* parent = nullptr);
    ~MediaBrowserPanel() override;

    /// Show the versions and key-moment markers for `clipId` in the panel.
    void showClip(ClipId clipId);

    /// Rebuild the library / version / marker views from the current model state.
    void refresh();

    /// Install (or clear) the gateway the panel's import button routes new
    /// imports through (task 11.4). When set, `promptImport()` calls
    /// `MediaBrowserViewModel::importMediaViaGateway` instead of the
    /// validator-injected `importMedia()`.
    void setGateway(GuiToolGateway* gateway) noexcept { viewModel_.setGateway(gateway); }

    /// The underlying view model, for MainWindow to read the current library
    /// asset selection when building a placement gesture (usable-editor
    /// Requirement 3).
    [[nodiscard]] MediaBrowserViewModel& viewModel() noexcept { return viewModel_; }

signals:
    /// The library asset selection changed (a row was picked, or the selection
    /// was cleared because the list lost its current row).
    void librarySelectionChanged();

public slots:
    /// Prompt for a media file and import it through the view model, surfacing a
    /// "could not import" message (which names the format / unreadable file) on
    /// rejection (Requirements 3.1, 3.2, 3.3).
    void promptImport();

private:
    void buildUi();
    void refreshLibrary();
    void refreshVersions();
    void refreshKeyMoments();
    void onLibraryRowChanged(int row);

    MediaBrowserViewModel viewModel_;

    QPushButton* importButton_ = nullptr;
    QListWidget* libraryList_ = nullptr;
    QListWidget* versionList_ = nullptr;
    QLabel*      keyMomentLabel_ = nullptr;
    QLabel*      importErrorLabel_ = nullptr;
};

}  // namespace palmier::ui

#endif  // PALMIER_HAVE_QT

#endif  // PALMIER_UI_MEDIABROWSERPANEL_HPP
