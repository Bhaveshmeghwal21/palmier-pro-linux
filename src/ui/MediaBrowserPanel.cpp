// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/MediaBrowserPanel.cpp — implementation of the Qt 6 Media Browser panel.
//
// Compiled only when Qt 6 is available (PALMIER_HAVE_QT). The panel is a thin
// skin over MediaBrowserViewModel: it renders the model's library / version /
// key-moment output into widgets and forwards user actions (import, clip and
// version selection) back into the model. No panel logic lives here.

#include "ui/MediaBrowserPanel.hpp"

#ifdef PALMIER_HAVE_QT

#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QString>
#include <QVBoxLayout>

#include <string>
#include <utility>

#include "services/Json.hpp"
#include "ui/GuiToolGateway.hpp"

namespace palmier::ui {

MediaBrowserPanel::MediaBrowserPanel(MediaManager& media,
                                     services::KeyMomentMarkerModel& markers,
                                     MediaBrowserViewModel::ImportValidator validator,
                                     QWidget* parent)
    : QWidget(parent), viewModel_(media, markers, std::move(validator)) {
    buildUi();
    refresh();
}

MediaBrowserPanel::~MediaBrowserPanel() = default;

void MediaBrowserPanel::buildUi() {
    auto* layout = new QVBoxLayout(this);

    importButton_ = new QPushButton(QStringLiteral("Import Media…"), this);
    connect(importButton_, &QPushButton::clicked, this, &MediaBrowserPanel::promptImport);
    layout->addWidget(importButton_);

    importErrorLabel_ = new QLabel(this);
    importErrorLabel_->setWordWrap(true);
    importErrorLabel_->setStyleSheet(QStringLiteral("color: #c0392b;"));
    importErrorLabel_->hide();
    layout->addWidget(importErrorLabel_);

    layout->addWidget(new QLabel(QStringLiteral("Library"), this));
    // Usable-editor tasks.md task 15.2: a free-text filter over the flat
    // library, matched against display name, source path and tags
    // (MediaBrowserViewModel::matchesFilter()). Setting it never touches the
    // project or the media library — it only changes which rows library()
    // returns — so no gateway/tool routing is needed here, unlike every
    // import/tag-mutating action in this panel.
    filterEdit_ = new QLineEdit(this);
    filterEdit_->setPlaceholderText(QStringLiteral("Filter by name, path or tag…"));
    connect(filterEdit_, &QLineEdit::textChanged, this, [this](const QString& text) {
        viewModel_.setFilterText(text.toStdString());
        refreshLibrary();
    });
    layout->addWidget(filterEdit_);

    libraryList_ = new QListWidget(this);
    connect(libraryList_, &QListWidget::currentRowChanged, this,
            &MediaBrowserPanel::onLibraryRowChanged);
    layout->addWidget(libraryList_);

    layout->addWidget(new QLabel(QStringLiteral("Clip Versions"), this));
    versionList_ = new QListWidget(this);
    // Selecting a version row rolls the clip back to that take (Requirement 3.4).
    connect(versionList_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row < 0) {
            return;
        }
        if (const auto clip = viewModel_.selectedClip()) {
            (void)viewModel_.selectVersion(*clip, static_cast<std::size_t>(row));
            refreshKeyMoments();
        }
    });
    layout->addWidget(versionList_);

    keyMomentLabel_ = new QLabel(this);
    keyMomentLabel_->setWordWrap(true);
    layout->addWidget(keyMomentLabel_);
}

void MediaBrowserPanel::promptImport() {
    const QString file = QFileDialog::getOpenFileName(this, QStringLiteral("Import Media"));
    if (file.isEmpty()) {
        return;
    }

    // Task 11.4: when a gateway is installed (the real application shell always
    // installs one), route the import through it — the SAME `media.import`
    // tool call the MCP endpoint and the in-app agent use — rather than through
    // the validator-injected importMedia() path, which predates the shared tool
    // surface and does not register the asset with the current ProjectSession.
    if (GuiToolGateway* gateway = viewModel_.gateway(); gateway != nullptr) {
        const Result<services::Json> result =
            viewModel_.importMediaViaGateway(file.toStdString());
        if (result.isError()) {
            importErrorLabel_->setText(QStringLiteral("Could not import: %1")
                                           .arg(QString::fromStdString(
                                               result.error().message())));
            importErrorLabel_->show();
            return;
        }
        importErrorLabel_->hide();
        refreshLibrary();
        return;
    }

    const auto result = viewModel_.importMedia(std::filesystem::path(file.toStdString()));
    if (result.isError()) {
        const std::string& msg =
            viewModel_.lastImportError() ? *viewModel_.lastImportError() : std::string("import failed");
        importErrorLabel_->setText(QStringLiteral("Could not import: %1")
                                       .arg(QString::fromStdString(msg)));
        importErrorLabel_->show();
        return;
    }
    importErrorLabel_->hide();
    refreshLibrary();
}

void MediaBrowserPanel::showClip(ClipId clipId) {
    viewModel_.selectClip(clipId);
    refreshVersions();
    refreshKeyMoments();
}

void MediaBrowserPanel::refresh() {
    refreshLibrary();
    refreshVersions();
    refreshKeyMoments();
}

void MediaBrowserPanel::refreshLibrary() {
    if (!libraryList_) {
        return;
    }
    libraryList_->clear();
    for (const MediaLibraryEntry& row : viewModel_.library()) {
        libraryList_->addItem(QString::fromStdString(row.displayName));
    }
}

void MediaBrowserPanel::refreshVersions() {
    if (!versionList_) {
        return;
    }
    QSignalBlocker blocker(versionList_);
    versionList_->clear();
    int selectedRow = -1;
    for (const ClipVersionEntry& row : viewModel_.versionsForSelectedClip()) {
        const QString label = QStringLiteral("v%1 %2%3")
                                  .arg(row.index + 1)
                                  .arg(QString::fromStdString(row.assetRef.assetId.toString()))
                                  .arg(row.generated ? QStringLiteral(" (generated)") : QString());
        versionList_->addItem(label);
        if (row.selected) {
            selectedRow = static_cast<int>(row.index);
        }
    }
    if (selectedRow >= 0) {
        versionList_->setCurrentRow(selectedRow);
    }
}

void MediaBrowserPanel::refreshKeyMoments() {
    if (!keyMomentLabel_) {
        return;
    }
    const KeyMomentDisplay display = viewModel_.keyMomentsForSelectedClip();
    switch (display.state) {
        case KeyMomentDisplayState::NotAnalyzed:
            keyMomentLabel_->clear();
            break;
        case KeyMomentDisplayState::NoKeyMoments:
            // Requirement 5.4: indicate that no key moments were found.
            keyMomentLabel_->setText(QStringLiteral("No key moments were found."));
            break;
        case KeyMomentDisplayState::KeyMomentsFound:
            // Requirement 5.3: a marker at each detected timestamp.
            keyMomentLabel_->setText(
                QStringLiteral("%1 key-moment marker(s)").arg(display.markers.size()));
            break;
    }
}

void MediaBrowserPanel::onLibraryRowChanged(int row) {
    // Row index maps directly onto viewModel_.library()'s order (both are the
    // Media Manager's import-order library, read fresh here rather than cached,
    // so this stays correct even if the library changed since the list was last
    // populated).
    const std::vector<MediaLibraryEntry> rows = viewModel_.library();
    if (row < 0 || static_cast<std::size_t>(row) >= rows.size()) {
        viewModel_.clearLibraryAssetSelection();
    } else {
        viewModel_.selectLibraryAsset(rows[static_cast<std::size_t>(row)].assetId);
    }
    emit librarySelectionChanged();
}

}  // namespace palmier::ui

#endif  // PALMIER_HAVE_QT
