// SPDX-License-Identifier: GPL-3.0-or-later
//
// media/DecoderClipFrameProvider.cpp — implementation (task 7.3; Requirements
// 5.1, 5.5, 14.8). See DecoderClipFrameProvider.hpp for the contract.

#include "media/DecoderClipFrameProvider.hpp"

#include <string>
#include <utility>

#include "core/Error.hpp"

namespace palmier::media {
namespace {

[[nodiscard]] std::string nameOf(const MediaAssetRef& asset) {
    if (!asset.sourcePath.empty()) return asset.sourcePath;
    return "asset " + asset.assetId.toString();
}

} // namespace

DecoderClipFrameProvider::DecoderClipFrameProvider(DecoderTeardownQueue& teardown,
                                                   DecodeBackendFactory factory, Options options,
                                                   AssetPathResolver resolver)
    : factory_(std::move(factory)),
      options_(options),
      resolver_(std::move(resolver)),
      pool_(teardown, options.pool) {
    if (options_.decoderCacheCapacity == 0) options_.decoderCacheCapacity = 1;
    if (!options_.defaultSourceFrameDuration.isPositive()) {
        options_.defaultSourceFrameDuration = Duration::fromNanoseconds(33'333'333);
    }
    if (!resolver_) {
        // Default resolution: the asset reference's own recorded source path.
        resolver_ = [](const MediaAssetRef& asset) -> Result<std::filesystem::path> {
            if (asset.sourcePath.empty()) {
                return err<std::filesystem::path>(
                    notFound("asset " + asset.assetId.toString() + " records no source path"));
            }
            return std::filesystem::path{asset.sourcePath};
        };
    }
}

DecoderClipFrameProvider::~DecoderClipFrameProvider() = default;

// ---------------------------------------------------------------------------
// LRU residency
// ---------------------------------------------------------------------------

void DecoderClipFrameProvider::touch(const Uuid& assetId) {
    const auto it = resident_.find(assetId);
    if (it == resident_.end()) return;
    lru_.splice(lru_.begin(), lru_, it->second);
    it->second = lru_.begin();
}

void DecoderClipFrameProvider::evictLeastRecentlyUsed() {
    if (lru_.empty()) return;
    const Uuid victim = lru_.back();
    lru_.pop_back();
    resident_.erase(victim);
    // Hands the decoder to the DecoderTeardownQueue: the playback thread returns
    // immediately and the FFmpeg close runs on the teardown thread (Req 14.8).
    pool_.retireAsset(victim);
    ++stats_.evictions;
}

Result<void> DecoderClipFrameProvider::ensureResident(const MediaAssetRef& asset) {
    if (resident_.find(asset.assetId) != resident_.end()) {
        touch(asset.assetId);
        return ok();
    }

    auto path = resolver_(asset);
    if (path.isError()) return err(std::move(path).error());

    auto activated = pool_.activateAsset(asset.assetId, path.value(), options_.prefs, factory_);
    if (activated.isError()) {
        const Error original = activated.error();
        return err(makeError(original.code(), "cannot open " + nameOf(asset) + " for playback: " +
                                                  original.message()));
    }

    lru_.push_front(asset.assetId);
    resident_.emplace(asset.assetId, lru_.begin());
    ++stats_.decodersOpened;

    // Evict only after the new asset is resident, so the asset just requested is
    // never the victim.
    while (lru_.size() > options_.decoderCacheCapacity) evictLeastRecentlyUsed();
    return ok();
}

// ---------------------------------------------------------------------------
// The gpu::ClipFrameProvider contract
// ---------------------------------------------------------------------------

Result<gpu::SourceFrame> DecoderClipFrameProvider::frameFor(const Clip& clip,
                                                           Duration timelinePosition) {
    ++stats_.requests;

    const auto fail = [this](Error error) -> Result<gpu::SourceFrame> {
        ++stats_.failures;
        return err<gpu::SourceFrame>(std::move(error));
    };

    if (!clip.assetRef.isValid()) {
        return fail(invalidArgument("clip " + clip.id.toString() + " references no media asset"));
    }
    if (timelinePosition < clip.timelineStart || timelinePosition >= clip.timelineEnd()) {
        return fail(outOfRange("timeline position " + std::to_string(timelinePosition.milliseconds()) +
                               " ms lies outside clip " + clip.id.toString()));
    }

    const Duration sourcePosition = clipSourcePosition(clip, timelinePosition);
    if (sourcePosition.isNegative()) {
        return fail(outOfRange("clip " + clip.id.toString() + " maps to a negative source position"));
    }

    if (auto resident = ensureResident(clip.assetRef); resident.isError()) {
        return fail(std::move(resident).error());
    }

    const Duration step =
        pool_.sourceFrameDuration(clip.assetRef.assetId, options_.defaultSourceFrameDuration);

    auto decoded = pool_.decodeFor(clip.assetRef.assetId, clip.id, sourcePosition, step);
    if (decoded.isError()) {
        // Returned, not swallowed: renderAt emits no partial frame (Req 5.5).
        return fail(std::move(decoded).error());
    }

    // Warm the workers for the frames that sequential playback will ask for next.
    if (options_.prefetchDepth > 0) {
        const Duration next = sourcePosition + step;
        if (next < clip.sourceOut) {
            pool_.prefetch(clip.assetRef.assetId, clip.id, next, step, options_.prefetchDepth);
        }
    }

    ++stats_.delivered;
    return std::move(decoded).value().image;
}

gpu::ClipFrameProvider DecoderClipFrameProvider::asProvider() {
    return [this](const Clip& clip, Duration position) -> Result<gpu::SourceFrame> {
        return frameFor(clip, position);
    };
}

void DecoderClipFrameProvider::releaseAll() {
    while (!lru_.empty()) evictLeastRecentlyUsed();
}

std::vector<Uuid> DecoderClipFrameProvider::residentAssets() const {
    return std::vector<Uuid>(lru_.begin(), lru_.end());
}

} // namespace palmier::media
