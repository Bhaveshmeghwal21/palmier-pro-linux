// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/MediaBrowserViewModel.cpp — implementation of the Media Browser panel's
// Qt-free presentation model (see MediaBrowserViewModel.hpp for the contract).

#include "ui/MediaBrowserViewModel.hpp"

#include <utility>

#include "core/MediaManager.hpp"

namespace palmier::ui {

namespace {

/// A short, user-facing label for a library row: the source path's file name
/// when present, otherwise the asset id (so a row is never blank).
[[nodiscard]] std::string deriveDisplayName(const MediaAssetRef& asset) {
    if (!asset.sourcePath.empty()) {
        std::filesystem::path p(asset.sourcePath);
        const std::string name = p.filename().string();
        if (!name.empty()) {
            return name;
        }
    }
    return asset.assetId.toString();
}

}  // namespace

MediaBrowserViewModel::MediaBrowserViewModel(MediaManager& media,
                                             services::KeyMomentMarkerModel& markers,
                                             ImportValidator validator)
    : media_(media), markers_(markers), validator_(std::move(validator)) {}

// --- Import (Requirements 3.1, 3.2, 3.3) ------------------------------------

Result<MediaAssetRef> MediaBrowserViewModel::importMedia(const std::filesystem::path& path) {
    if (!validator_) {
        // Defensive: with no validator wired, treat the import as unable to
        // proceed rather than silently cataloging an unvalidated asset. The
        // library is left unchanged.
        Error e = failedPrecondition("no media import validator is configured");
        lastImportError_ = e.message();
        return err<MediaAssetRef>(std::move(e));
    }

    // Validate first. On rejection (unsupported format 3.2, unreadable 3.3, or
    // any other error) the Media Manager is never touched, so the library is
    // left unchanged; we retain the message so the view can name the failure.
    Result<MediaAssetRef> validated = validator_(path);
    if (validated.isError()) {
        lastImportError_ = validated.error().message();
        return validated;
    }

    // Add to the project library, making the media available for placement on
    // the timeline (Requirement 3.1). importAsset never partially applies, so a
    // failure here (e.g. a duplicate id) also leaves the library unchanged.
    MediaAssetRef asset = std::move(validated).value();
    Result<void> added = media_.importAsset(asset);
    if (added.isError()) {
        lastImportError_ = added.error().message();
        return err<MediaAssetRef>(added.error());
    }

    lastImportError_.reset();
    return asset;
}

// --- Library (Requirement 3.1) ----------------------------------------------

std::vector<MediaLibraryEntry> MediaBrowserViewModel::library() const {
    std::vector<MediaLibraryEntry> rows;
    const std::vector<MediaAssetRef>& lib = media_.library();
    rows.reserve(lib.size());
    for (const MediaAssetRef& asset : lib) {
        rows.push_back(MediaLibraryEntry{asset.assetId, asset.sourcePath, deriveDisplayName(asset)});
    }
    return rows;
}

std::size_t MediaBrowserViewModel::libraryCount() const { return media_.assetCount(); }

bool MediaBrowserViewModel::libraryContains(const Uuid& assetId) const {
    return media_.hasAsset(assetId);
}

// --- Clip selection ---------------------------------------------------------

void MediaBrowserViewModel::selectClip(ClipId clipId) { selectedClip_ = clipId; }

void MediaBrowserViewModel::clearClipSelection() noexcept { selectedClip_.reset(); }

// --- Clip version history (Requirement 3.4) ---------------------------------

std::vector<ClipVersionEntry> MediaBrowserViewModel::versionsFor(ClipId clipId) const {
    std::vector<ClipVersionEntry> rows;
    const std::vector<ClipVersion> versions = media_.versions(clipId);
    const std::optional<std::size_t> selected = media_.selectedVersionIndex(clipId);
    rows.reserve(versions.size());
    for (std::size_t i = 0; i < versions.size(); ++i) {
        const ClipVersion& v = versions[i];
        rows.push_back(ClipVersionEntry{i, v.assetRef, v.sourceIn, v.sourceOut, v.generated,
                                        selected.has_value() && *selected == i});
    }
    return rows;
}

std::vector<ClipVersionEntry> MediaBrowserViewModel::versionsForSelectedClip() const {
    if (!selectedClip_.has_value()) {
        return {};
    }
    return versionsFor(*selectedClip_);
}

std::optional<std::size_t> MediaBrowserViewModel::selectedVersionIndex(ClipId clipId) const {
    return media_.selectedVersionIndex(clipId);
}

Result<void> MediaBrowserViewModel::selectVersion(ClipId clipId, std::size_t index) {
    return media_.selectVersion(clipId, index);
}

// --- Key-moment markers (Requirements 5.3, 5.4) -----------------------------

KeyMomentDisplay MediaBrowserViewModel::keyMomentsFor(ClipId clipId) const {
    KeyMomentDisplay display;

    const std::optional<services::ClipMarkers> recorded = markers_.markersFor(clipId);
    if (!recorded.has_value()) {
        // No detection has completed for this clip yet: neither markers nor the
        // "no key moments" indication apply.
        display.state = KeyMomentDisplayState::NotAnalyzed;
        return display;
    }

    if (recorded->presence == services::MarkerPresence::NoKeyMoments) {
        // Requirement 5.4: detection completed with zero moments — no markers,
        // show the "no key moments were found" indication.
        display.state = KeyMomentDisplayState::NoKeyMoments;
        return display;
    }

    // Requirement 5.3: one marker per detected timestamp, in the ascending order
    // the marker model preserves.
    display.state = KeyMomentDisplayState::KeyMomentsFound;
    display.markers.reserve(recorded->markers.size());
    for (const services::KeyMomentMarker& marker : recorded->markers) {
        display.markers.push_back(KeyMomentMarkerView{marker.milliseconds()});
    }
    return display;
}

KeyMomentDisplay MediaBrowserViewModel::keyMomentsForSelectedClip() const {
    if (!selectedClip_.has_value()) {
        return KeyMomentDisplay{};
    }
    return keyMomentsFor(*selectedClip_);
}

}  // namespace palmier::ui
