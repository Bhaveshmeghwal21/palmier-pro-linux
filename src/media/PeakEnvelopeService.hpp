// SPDX-License-Identifier: GPL-3.0-or-later
//
// media/PeakEnvelopeService.hpp — off-thread envelope production with a bounded
// cache in front of it (monitoring-and-grading Requirement 2.2, 2.5, 2.6, 2.7).
//
// This is the piece a renderer actually talks to. It composes the three parts that
// already exist and adds exactly one thing of its own: the guarantee that a paint
// never waits for a decoder.
//
//     lookup() --> PeakEnvelopeCache --> (miss) --> worker --> extractPeakEnvelope
//                        ^                                          |
//                        +------------------ store ------------------+
//
// ## The rule that shapes the whole interface
//
// `lookup()` NEVER blocks and never decodes. It answers from the cache, and on a
// miss it schedules the work and returns "not yet". A repaint calls it once per
// audio clip and proceeds immediately, which is Requirement 2.2's "the shell
// remains responsive while it runs" expressed as a precondition rather than as an
// aspiration. When the answer arrives the service fires a callback so the view can
// ask for a repaint; it does not reach into any widget itself.
//
// ## Why scheduling has to be deduplicated
//
// A repaint happens many times a second and every one of them looks up every
// visible clip. Without suppression, a single 2-second decode would be scheduled
// forty times over, and twenty clips sharing one asset would each queue their own
// copy of the same work. So an asset already in flight is not re-queued, and a
// failure is remembered in the cache rather than retried (Requirement 2.7). The
// service counts both suppressions, so "not retried on every repaint" is an
// observable number instead of a claim.
//
// ## Testability
//
// Two seams, both deliberate:
//
//   * the decoder arrives as a DecodeBackendFactory, so a synthetic backend
//     replaces FFmpeg entirely;
//   * `drainFor(timeout)` is a BOUNDED wait until no job is queued or in flight,
//     mirroring DecodeWorkerPool::drainFor. Tests observe the worker by draining
//     it, never by sleeping on a wall clock and hoping.

#ifndef PALMIER_MEDIA_PEAKENVELOPESERVICE_HPP
#define PALMIER_MEDIA_PEAKENVELOPESERVICE_HPP

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "core/Duration.hpp"
#include "core/Uuid.hpp"
#include "media/MediaDecoder.hpp"
#include "media/PeakEnvelope.hpp"
#include "media/PeakEnvelopeCache.hpp"
#include "media/PeakEnvelopeExtractor.hpp"

namespace palmier::media {

/// Geometry and policy of a PeakEnvelopeService.
struct PeakEnvelopeServiceOptions {
    /// Source-time width of one bucket. 10 ms gives 100 buckets per second — finer
    /// than a timeline pixel at any usable zoom, while a one-hour asset still costs
    /// only 360,000 buckets.
    Duration    bucketDuration{Duration::fromMilliseconds(10)};
    /// Entries the cache holds before evicting least-recently-used.
    std::size_t cacheCapacity{PeakEnvelopeCache::kDefaultCapacity};
    /// Worker threads. One is enough: envelope extraction is throughput work with
    /// no latency requirement, and a single worker keeps decode order predictable.
    std::size_t workerCount{1};
    /// Bound on how much audio one extraction reads.
    EnvelopeExtractionLimits limits{};
};

/// Observability counters, monotonic over the service's lifetime.
struct PeakEnvelopeServiceStats {
    std::uint64_t scheduled{0};   ///< Jobs actually queued.
    std::uint64_t completed{0};   ///< Extractions that produced an envelope.
    std::uint64_t silent{0};      ///< Extractions that found no audio (Requirement 2.6).
    std::uint64_t failed{0};      ///< Extractions that reported an error (Requirement 2.7).
    /// Lookups that wanted work already queued or in flight. Proves one asset is
    /// decoded once however many clips reference it and however often they repaint.
    std::uint64_t duplicatesSuppressed{0};
    /// Lookups that hit a remembered failure. Proves Requirement 2.7's "not
    /// retried on every repaint" numerically.
    std::uint64_t failuresNotRetried{0};
};

/// What a lookup found. `pending` is the only state that is neither an answer nor
/// an error: the work is under way and the caller should draw nothing yet.
struct EnvelopeLookup {
    /// The envelope to draw, or null when there is nothing to draw.
    std::shared_ptr<const PeakEnvelope> envelope{};
    /// True while the answer is still being computed.
    bool                                pending = false;
    /// True when this asset's extraction failed (Requirement 2.7).
    bool                                failed = false;
    /// The failure message, when `failed`.
    std::string                         failure{};

    /// True when the asset is known to carry no drawable audio — either no audio
    /// stream at all or an empty one (Requirement 2.6). Not an error.
    [[nodiscard]] bool isSilent() const noexcept {
        return !pending && !failed && (envelope == nullptr || envelope->empty());
    }
};

/// Off-thread envelope production with a bounded LRU cache.
///
/// Not copyable and not movable: the composition root owns one and hands out
/// references, matching DecodeWorkerPool.
class PeakEnvelopeService {
public:
    using Options = PeakEnvelopeServiceOptions;
    using Stats   = PeakEnvelopeServiceStats;
    /// Fired on a WORKER thread when `assetId`'s answer becomes available. The
    /// callee must not paint from it — it should marshal a repaint request onto its
    /// own thread, which is what a Qt widget's queued signal does.
    using ReadyCallback = std::function<void(const Uuid& assetId)>;

    PeakEnvelopeService(DecodePrefs prefs, DecodeBackendFactory factory,
                        Options options = Options{});
    ~PeakEnvelopeService();

    PeakEnvelopeService(const PeakEnvelopeService&)            = delete;
    PeakEnvelopeService& operator=(const PeakEnvelopeService&) = delete;
    PeakEnvelopeService(PeakEnvelopeService&&)                 = delete;
    PeakEnvelopeService& operator=(PeakEnvelopeService&&)      = delete;

    /// Install the ready callback. Must be called before the first `lookup()` that
    /// could schedule work, or an early completion may fire against no callback.
    void setReadyCallback(ReadyCallback callback);

    /// The answer for `assetId`, scheduling the work on a miss.
    ///
    /// NEVER blocks and never decodes on the calling thread. Safe to call from a
    /// paint handler, once per clip, as often as the view repaints: a miss for an
    /// asset already in flight, or one already known to have failed, costs a
    /// counter increment and nothing else.
    [[nodiscard]] EnvelopeLookup lookup(const Uuid& assetId, const std::filesystem::path& path);

    /// The answer for `assetId` without scheduling anything. For callers that want
    /// to draw what is ready but must not cause work (and for assertions).
    [[nodiscard]] EnvelopeLookup peek(const Uuid& assetId) const;

    /// Forget `assetId`, so a later lookup recomputes it — e.g. the asset was
    /// re-imported. Returns whether anything was forgotten.
    bool forget(const Uuid& assetId);

    /// Bounded wait until no job is queued or in flight. Reports whether the
    /// service reached idle. Exists so callers and tests observe the worker
    /// without sleeping on a wall clock.
    [[nodiscard]] bool drainFor(std::chrono::milliseconds timeout);

    /// Stop the workers and drop pending jobs. Idempotent; called by the
    /// destructor. An extraction already running is allowed to finish, because
    /// abandoning a decoder mid-read is what the DecoderTeardownQueue exists to
    /// avoid doing on a thread someone is waiting on.
    void shutdown();

    [[nodiscard]] std::size_t workerCount() const noexcept { return workers_.size(); }
    [[nodiscard]] Stats stats() const;
    [[nodiscard]] EnvelopeCacheStats cacheStats() const;
    [[nodiscard]] std::size_t cachedAssetCount() const;
    /// Jobs queued but not yet started, plus those running.
    [[nodiscard]] std::size_t outstandingJobs() const;

private:
    struct Job {
        Uuid                  assetId{};
        std::filesystem::path path{};
    };

    void workerLoop();
    /// Build an EnvelopeLookup from a cache entry. Called with the mutex held.
    [[nodiscard]] static EnvelopeLookup lookupFrom(const EnvelopeCacheEntry& entry);

    DecodePrefs           prefs_;
    DecodeBackendFactory  factory_;
    Options               options_;

    mutable std::mutex      mutex_;
    std::condition_variable jobsAvailable_{};
    std::condition_variable idle_{};

    mutable PeakEnvelopeCache   cache_;
    std::deque<Job>             jobs_{};
    /// Assets queued or in flight, so a repaint cannot queue the same work twice.
    std::unordered_set<Uuid>    inFlight_{};
    std::size_t                 running_{0};
    bool                        stopping_{false};
    Stats                       stats_{};
    ReadyCallback               ready_{};
    std::vector<std::thread>    workers_{};
};

} // namespace palmier::media

#endif // PALMIER_MEDIA_PEAKENVELOPESERVICE_HPP
