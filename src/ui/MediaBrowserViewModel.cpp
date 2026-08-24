// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/MediaBrowserViewModel.cpp — implementation of the Media Browser panel's
// Qt-free presentation model (see MediaBrowserViewModel.hpp for the contract).

#include "ui/MediaBrowserViewModel.hpp"

#include <utility>

#include "core/Error.hpp"
#include "core/MediaManager.hpp"
#include "services/Json.hpp"
#include "ui/GuiToolGateway.hpp"

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

Result<services::Json> MediaBrowserViewModel::importMediaViaGateway(const std::string& path) {
    if (gateway_ == nullptr) {
        return err<services::Json>(
            failedPrecondition("MediaBrowserViewModel: no gateway is installed"));
    }
    Result<services::Json> result = gateway_->importMedia(path);
    if (result.isOk()) {
        // Cache the probed duration so a later placement gesture can size the
        // clip without re-probing the file (usable-editor Requirement 3).
        const services::Json& payload = result.value();
        if (payload.isObject()) {
            const services::Json* assetIdField = payload.find("assetId");
            const services::Json* durationField = payload.find("durationMs");
            if (assetIdField != nullptr && assetIdField->isString() && durationField != nullptr &&
                durationField->isNumber()) {
                if (const std::optional<Uuid> assetId = Uuid::parse(assetIdField->asString())) {
                    assetDurations_[*assetId] = Duration::fromMilliseconds(durationField->asInt());
                }
            }
        }
    }
    return result;
}

// --- Library (Requirement 3.1) ----------------------------------------------

std::vector<MediaLibraryEntry> MediaBrowserViewModel::library() const {
    std::vector<MediaLibraryEntry> rows;
    const std::vector<MediaAssetRef>& lib = media_.library();
    rows.reserve(lib.size());
    for (const MediaAssetRef& asset : lib) {
        MediaLibraryEntry entry{asset.assetId, asset.sourcePath, deriveDisplayName(asset),
                                asset.tags};
        if (matchesFilter(entry)) {
            rows.push_back(std::move(entry));
        }
    }
    return rows;
}

std::size_t MediaBrowserViewModel::libraryCount() const { return media_.assetCount(); }

bool MediaBrowserViewModel::libraryContains(const Uuid& assetId) const {
    return media_.hasAsset(assetId);
}

namespace {
/// Case-insensitive ASCII lowering, matching the convention every other
/// case-insensitive comparison in this tree (e.g. RemoteAccessGate's origin
/// host matching) already uses: this is a UI filter over ASCII-typical file
/// names and user-typed tags, not a Unicode-aware search.
[[nodiscard]] std::string toLowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}
}  // namespace

void MediaBrowserViewModel::setFilterText(std::string filter) {
    filterText_ = std::move(filter);
}

bool MediaBrowserViewModel::matchesFilter(const MediaLibraryEntry& entry) const {
    if (filterText_.empty()) {
        return true;
    }
    const std::string needle = toLowerAscii(filterText_);
    if (toLowerAscii(entry.displayName).find(needle) != std::string::npos) {
        return true;
    }
    if (toLowerAscii(entry.sourcePath).find(needle) != std::string::npos) {
        return true;
    }
    for (const std::string& tag : entry.tags) {
        if (toLowerAscii(tag).find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

Result<void> MediaBrowserViewModel::setAssetTags(const Uuid& assetId,
                                                 std::vector<std::string> tags) {
    return media_.setAssetTags(assetId, std::move(tags));
}

std::vector<std::string> MediaBrowserViewModel::assetTags(const Uuid& assetId) const {
    const std::optional<MediaAssetRef> asset = media_.asset(assetId);
    if (!asset.has_value()) {
        return {};
    }
    return asset->tags;
}

std::optional<Duration> MediaBrowserViewModel::assetDuration(const Uuid& assetId) const {
    const auto it = assetDurations_.find(assetId);
    if (it == assetDurations_.end()) {
        return std::nullopt;
    }
    return it->second;
}

// --- Library asset selection (usable-editor Requirement 3) ------------------

void MediaBrowserViewModel::selectLibraryAsset(Uuid assetId) { selectedLibraryAsset_ = assetId; }

void MediaBrowserViewModel::clearLibraryAssetSelection() noexcept {
    selectedLibraryAsset_.reset();
}

std::optional<Uuid> MediaBrowserViewModel::selectedLibraryAsset() const {
    if (!selectedLibraryAsset_.has_value()) {
        return std::nullopt;
    }
    // A selection pointing at an asset no longer in the library (removed by
    // another surface) is reported as no selection, mirroring how
    // InspectorViewModel treats a selected clip that has disappeared.
    if (!media_.hasAsset(*selectedLibraryAsset_)) {
        return std::nullopt;
    }
    return selectedLibraryAsset_;
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
