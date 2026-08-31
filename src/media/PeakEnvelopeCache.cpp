// SPDX-License-Identifier: GPL-3.0-or-later
//
// media/PeakEnvelopeCache.cpp — implementation of the bounded LRU envelope store.

#include "media/PeakEnvelopeCache.hpp"

#include <utility>

namespace palmier::media {

PeakEnvelopeCache::PeakEnvelopeCache(std::size_t capacity) noexcept
    // A zero capacity would make every store immediately evict itself, so the
    // cache would "work" while never caching — a bug that hides. One is the
    // smallest honest bound.
    : capacity_(capacity == 0 ? 1 : capacity) {}

const EnvelopeCacheEntry* PeakEnvelopeCache::find(const Uuid& assetId) {
    const auto it = entries_.find(assetId);
    if (it == entries_.end()) {
        ++stats_.misses;
        return nullptr;
    }
    ++stats_.hits;
    if (it->second.entry.failed) {
        // Counted so Requirement 2.7's "not retried on every repaint" is
        // observable: a repainting caller drives this up while the decoder is
        // never touched again.
        ++stats_.failureHits;
    }
    touch(assetId);
    return &it->second.entry;
}

const EnvelopeCacheEntry* PeakEnvelopeCache::peek(const Uuid& assetId) const noexcept {
    const auto it = entries_.find(assetId);
    return it == entries_.end() ? nullptr : &it->second.entry;
}

void PeakEnvelopeCache::store(const Uuid& assetId, PeakEnvelope envelope) {
    EnvelopeCacheEntry entry;
    entry.envelope = std::move(envelope);
    entry.failed = false;

    const auto it = entries_.find(assetId);
    if (it != entries_.end()) {
        it->second.entry = std::move(entry);
        touch(assetId);
        return;
    }

    recency_.push_front(assetId);
    Slot slot;
    slot.entry = std::move(entry);
    slot.position = recency_.begin();
    entries_.emplace(assetId, std::move(slot));
    ++stats_.insertions;
    evictToCapacity();
}

void PeakEnvelopeCache::storeFailure(const Uuid& assetId, std::string reason) {
    const auto it = entries_.find(assetId);
    if (it != entries_.end()) {
        it->second.entry.envelope = PeakEnvelope{};
        it->second.entry.failed = true;
        it->second.entry.failure = std::move(reason);
        touch(assetId);
        return;
    }

    recency_.push_front(assetId);
    Slot slot;
    slot.entry.failed = true;
    slot.entry.failure = std::move(reason);
    slot.position = recency_.begin();
    entries_.emplace(assetId, std::move(slot));
    ++stats_.insertions;
    evictToCapacity();
}

bool PeakEnvelopeCache::forget(const Uuid& assetId) {
    const auto it = entries_.find(assetId);
    if (it == entries_.end()) return false;
    recency_.erase(it->second.position);
    entries_.erase(it);
    return true;
}

void PeakEnvelopeCache::clear() noexcept {
    entries_.clear();
    recency_.clear();
}

std::vector<Uuid> PeakEnvelopeCache::recencyOrder() const {
    return std::vector<Uuid>(recency_.begin(), recency_.end());
}

void PeakEnvelopeCache::touch(const Uuid& assetId) {
    const auto it = entries_.find(assetId);
    if (it == entries_.end()) return;
    // splice moves the node itself, so the stored iterator stays valid.
    recency_.splice(recency_.begin(), recency_, it->second.position);
    it->second.position = recency_.begin();
}

void PeakEnvelopeCache::evictToCapacity() {
    while (entries_.size() > capacity_ && !recency_.empty()) {
        const Uuid victim = recency_.back();
        recency_.pop_back();
        entries_.erase(victim);
        ++stats_.evictions;
    }
}

} // namespace palmier::media
