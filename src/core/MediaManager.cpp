// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/MediaManager.cpp — implementation of the project media library and
// per-clip generated-clip version history (Requirements 3.1, 3.4).

#include "core/MediaManager.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace palmier {

MediaManager::MediaManager(std::size_t retentionCapacity)
    : retentionCapacity_(std::max(retentionCapacity, kMinRetainedVersions)) {}

// --- Media library ----------------------------------------------------------

Result<void> MediaManager::importAsset(const MediaAssetRef& asset) {
    if (!asset.isValid()) {
        return err(invalidArgument("cannot import a media asset with a nil asset id"));
    }
    if (assetIndex_.find(asset.assetId) != assetIndex_.end()) {
        return err(makeError(ErrorCode::AlreadyExists,
                             "media asset " + asset.assetId.toString() +
                                 " is already in the library"));
    }
    assetIndex_.emplace(asset.assetId, library_.size());
    library_.push_back(asset);
    return ok();
}

bool MediaManager::hasAsset(const Uuid& assetId) const {
    return assetIndex_.find(assetId) != assetIndex_.end();
}

std::optional<MediaAssetRef> MediaManager::asset(const Uuid& assetId) const {
    const auto it = assetIndex_.find(assetId);
    if (it == assetIndex_.end()) {
        return std::nullopt;
    }
    return library_[it->second];
}

// --- Per-clip version history -----------------------------------------------

Result<void> MediaManager::registerClip(ClipId clipId, const MediaAssetRef& assetRef,
                                        Duration sourceIn, Duration sourceOut,
                                        bool generated) {
    if (sourceOut <= sourceIn) {
        return err(invalidArgument("clip version requires sourceOut > sourceIn"));
    }
    if (!hasAsset(assetRef.assetId)) {
        return err(notFound("clip asset " + assetRef.assetId.toString() +
                            " does not resolve in the media library"));
    }
    if (histories_.find(clipId) != histories_.end()) {
        return err(makeError(ErrorCode::AlreadyExists,
                             "clip " + clipId.toString() + " is already tracked"));
    }

    ClipHistory history;
    history.versions.push_back(ClipVersion{assetRef, sourceIn, sourceOut, generated});
    history.selected = 0;
    histories_.emplace(clipId, std::move(history));
    return ok();
}

Result<void> MediaManager::replaceWithGeneratedClip(ClipId clipId, const MediaAssetRef& assetRef,
                                                    Duration sourceIn, Duration sourceOut) {
    if (sourceOut <= sourceIn) {
        return err(invalidArgument("generated clip requires sourceOut > sourceIn"));
    }
    if (!hasAsset(assetRef.assetId)) {
        return err(notFound("generated asset " + assetRef.assetId.toString() +
                            " must be added to the media library before it replaces a clip"));
    }
    const auto it = histories_.find(clipId);
    if (it == histories_.end()) {
        return err(failedPrecondition("clip " + clipId.toString() +
                                      " has no existing version to replace"));
    }

    ClipHistory& history = it->second;
    // The prior version(s) stay in `versions`; append the generated version and
    // make it the selected one.
    history.versions.push_back(ClipVersion{assetRef, sourceIn, sourceOut, /*generated=*/true});
    history.selected = history.versions.size() - 1;

    // Enforce the retention bound: keep at most retentionCapacity_ versions,
    // dropping the oldest first. The retention capacity is always >= 10, so the
    // "at least the 10 most recent versions" guarantee (Requirement 3.4) holds.
    if (history.versions.size() > retentionCapacity_) {
        const std::size_t drop = history.versions.size() - retentionCapacity_;
        history.versions.erase(history.versions.begin(),
                               history.versions.begin() + static_cast<std::ptrdiff_t>(drop));
        history.selected = history.selected >= drop ? history.selected - drop : 0;
    }
    return ok();
}

bool MediaManager::tracksClip(ClipId clipId) const {
    return histories_.find(clipId) != histories_.end();
}

std::size_t MediaManager::versionCount(ClipId clipId) const {
    const auto it = histories_.find(clipId);
    return it == histories_.end() ? 0 : it->second.versions.size();
}

std::vector<ClipVersion> MediaManager::versions(ClipId clipId) const {
    const auto it = histories_.find(clipId);
    if (it == histories_.end()) {
        return {};
    }
    return it->second.versions;
}

std::optional<ClipVersion> MediaManager::versionAt(ClipId clipId, std::size_t index) const {
    const auto it = histories_.find(clipId);
    if (it == histories_.end() || index >= it->second.versions.size()) {
        return std::nullopt;
    }
    return it->second.versions[index];
}

std::optional<std::size_t> MediaManager::selectedVersionIndex(ClipId clipId) const {
    const auto it = histories_.find(clipId);
    if (it == histories_.end()) {
        return std::nullopt;
    }
    return it->second.selected;
}

std::optional<ClipVersion> MediaManager::selectedVersion(ClipId clipId) const {
    const auto it = histories_.find(clipId);
    if (it == histories_.end()) {
        return std::nullopt;
    }
    return it->second.versions[it->second.selected];
}

Result<void> MediaManager::selectVersion(ClipId clipId, std::size_t index) {
    const auto it = histories_.find(clipId);
    if (it == histories_.end()) {
        return err(notFound("clip " + clipId.toString() + " is not tracked"));
    }
    if (index >= it->second.versions.size()) {
        return err(outOfRange("version index " + std::to_string(index) +
                              " is out of range for clip " + clipId.toString()));
    }
    it->second.selected = index;
    return ok();
}

Result<void> MediaManager::removeAsset(const Uuid& assetId) {
    const auto it = assetIndex_.find(assetId);
    if (it == assetIndex_.end()) {
        return err(notFound("media asset " + assetId.toString() +
                            " is not in the library"));
    }

    for (const auto& [clipId, history] : histories_) {
        const bool referenced = std::any_of(
            history.versions.begin(), history.versions.end(),
            [&assetId](const ClipVersion& version) { return version.assetRef.assetId == assetId; });
        if (referenced) {
            return err(failedPrecondition(
                "media asset " + assetId.toString() +
                " is still referenced by tracked clip " + clipId.toString()));
        }
    }

    library_.erase(library_.begin() + static_cast<std::ptrdiff_t>(it->second));
    assetIndex_.clear();
    for (std::size_t index = 0; index < library_.size(); ++index) {
        assetIndex_.emplace(library_[index].assetId, index);
    }
    return ok();
}

} // namespace palmier
