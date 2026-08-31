// SPDX-License-Identifier: GPL-3.0-or-later
//
// media/PeakEnvelopeCache.hpp — a bounded, LRU-evicting store of per-asset peak
// envelopes (monitoring-and-grading Requirement 2.5, 2.6, 2.7).
//
// One envelope per ASSET, shared by every clip referencing it. A timeline with
// forty cuts from one interview holds one envelope, not forty: the envelope is
// keyed on source identity, and each clip maps its own trim onto it at draw time.
//
// ## Why the cache also remembers failure and silence
//
// Three outcomes have to be distinguished, and only three:
//
//   * **Not computed yet** — absent from the cache. A lookup misses and the caller
//     may schedule the work.
//   * **Computed, and the asset has no audio** — present, with an empty envelope.
//     Requirement 2.6: draw nothing, report nothing. This must be a CACHED
//     result, not an absence, or a silent asset would be re-decoded on every
//     repaint forever while never producing anything.
//   * **Computation failed** — present, flagged, with the reason. Requirement 2.7:
//     reported once, never retried per repaint, and the clip still draws and stays
//     editable.
//
// Collapsing silence and failure into "no envelope" would lose the second
// distinction and turn the cheapest case into the most expensive one, so presence
// in this cache means "we know the answer", and the answer itself may be an empty
// envelope or a failure. That mirrors the Audio_Engine's existing "silent, not
// failed" separation rather than inventing a second vocabulary for it.
//
// Bounded by entry count rather than by bytes: an envelope's size is a function of
// asset duration and bucket width, both known to the caller, and a count is the
// bound a reader can reason about without measuring. Eviction is strict LRU on
// every access, including a lookup that hits.
//
// Pure standard library — no decoder, no thread, no Qt, no I/O.

#ifndef PALMIER_MEDIA_PEAKENVELOPECACHE_HPP
#define PALMIER_MEDIA_PEAKENVELOPECACHE_HPP

#include <cstddef>
#include <cstdint>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/Uuid.hpp"
#include "media/PeakEnvelope.hpp"

namespace palmier::media {

/// What the cache knows about one asset. Presence of this entry means the answer
/// is known; the entry says what the answer is.
struct EnvelopeCacheEntry {
    /// The computed envelope. Empty both for a failure and for an asset that
    /// genuinely carries no audio — `failed` is what separates the two.
    PeakEnvelope envelope{};
    /// True when computation failed. The clip still draws (Requirement 2.7).
    bool         failed = false;
    /// The failure's message, for the single report Requirement 2.7 allows.
    std::string  failure{};

    /// Known, succeeded, and there was no audio to draw (Requirement 2.6).
    [[nodiscard]] bool isSilent() const noexcept { return !failed && envelope.empty(); }
};

/// Observability counters, monotonic over the cache's lifetime.
struct EnvelopeCacheStats {
    std::uint64_t hits{0};
    std::uint64_t misses{0};
    std::uint64_t insertions{0};
    std::uint64_t evictions{0};
    /// Lookups that hit an entry already flagged failed — the count that proves
    /// Requirement 2.7's "not retried on every repaint".
    std::uint64_t failureHits{0};
};

class PeakEnvelopeCache {
public:
    /// Default bound. Large enough that a realistic edit's working set of audio
    /// assets stays resident, small enough that the memory is bounded and the
    /// eviction path is exercised in practice rather than only in tests.
    static constexpr std::size_t kDefaultCapacity = 32;

    explicit PeakEnvelopeCache(std::size_t capacity = kDefaultCapacity) noexcept;

    /// Look up `assetId`, marking it most-recently-used. Returns nullptr when the
    /// answer is not known yet — which is the caller's cue to schedule the work.
    ///
    /// The returned pointer is invalidated by any later `store`/`storeFailure`/
    /// `clear`, and by any `find` that evicts. Callers copy what they need.
    [[nodiscard]] const EnvelopeCacheEntry* find(const Uuid& assetId);

    /// Look up without disturbing recency or the counters. For assertions and
    /// diagnostics, so a test can observe the cache without changing it.
    [[nodiscard]] const EnvelopeCacheEntry* peek(const Uuid& assetId) const noexcept;

    /// Record a successful computation. An empty `envelope` is the legitimate
    /// answer for an asset with no audio stream (Requirement 2.6). Replaces any
    /// existing entry, including a failure, so a re-import can recover.
    void store(const Uuid& assetId, PeakEnvelope envelope);

    /// Record a failure so it is never recomputed per repaint (Requirement 2.7).
    void storeFailure(const Uuid& assetId, std::string reason);

    /// Drop `assetId`'s entry if present, e.g. because the asset was removed.
    /// Returns whether anything was dropped.
    bool forget(const Uuid& assetId);

    void clear() noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] bool contains(const Uuid& assetId) const noexcept {
        return entries_.find(assetId) != entries_.end();
    }
    [[nodiscard]] EnvelopeCacheStats stats() const noexcept { return stats_; }

    /// Most-recently-used first. Exists so the eviction ORDER is assertable, not
    /// merely the eviction count.
    [[nodiscard]] std::vector<Uuid> recencyOrder() const;

private:
    struct Slot {
        EnvelopeCacheEntry           entry{};
        std::list<Uuid>::iterator    position{};
    };

    /// Move `assetId` to the front of the recency list.
    void touch(const Uuid& assetId);
    /// Evict from the back until at most `capacity_` entries remain.
    void evictToCapacity();

    std::size_t                            capacity_;
    std::list<Uuid>                        recency_{};  ///< front = most recent
    std::unordered_map<Uuid, Slot>         entries_{};
    EnvelopeCacheStats                     stats_{};
};

} // namespace palmier::media

#endif // PALMIER_MEDIA_PEAKENVELOPECACHE_HPP
