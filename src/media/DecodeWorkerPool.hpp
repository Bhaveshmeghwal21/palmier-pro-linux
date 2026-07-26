// SPDX-License-Identifier: GPL-3.0-or-later
//
// media/DecodeWorkerPool.hpp — the playback decode stage: N = 2 worker threads,
// one MediaDecoder per active asset, decoded frames pushed to a bounded
// lock-protected per-clip queue (task 7.3; Requirements 5.1, 5.5, 14.8).
//
// This is the "decode" half of the playback pipeline in design.md's "Runtime
// pipelines" diagram:
//
//     MediaDecoder (per asset) --> bounded frame queue --> DecoderClipFrameProvider
//
// and the "Video decode for playback" row of the D5 threading table. The pool
// owns every decoder used for playback, so exactly one thread ever touches a
// given MediaDecoder at a time, and it owns the per-clip frame queues the
// provider pops from.
//
// Responsibilities, and deliberately nothing else:
//
//   * **Decoder ownership.** activateAsset() opens one MediaDecoder for an asset
//     through MediaDecoder's existing DecodeBackendFactory seam;
//     retireAsset() hands that decoder to the DecoderTeardownQueue and returns
//     immediately, so an LRU eviction on the playback thread never pays FFmpeg's
//     close cost (Requirement 14.8).
//   * **Serialisation per asset.** A decoder is a stateful cursor. Every access
//     (a synchronous decodeFor(), or a worker running a prefetch job) claims the
//     asset first, so two clips sharing one asset interleave safely rather than
//     corrupting each other's cursor.
//   * **Sequential-versus-seek.** The pool tracks, per asset, the source
//     position the decoder's *next* frame will satisfy. A request for that
//     position is a sequential nextFrame(); anything else seeks first. This is
//     the mechanism behind the provider's "seeks when the request is not the
//     next sequential frame" contract, and it lives here because the decision
//     and the decode have to be atomic with respect to the worker threads.
//   * **Bounded per-clip queues.** prefetch() enqueues decode jobs for the
//     workers; a finished job pushes its frame onto that clip's queue, which is
//     capped at Options::clipQueueCapacity. A job that finds the queue full is
//     dropped rather than blocking a worker, because a dropped prefetch only
//     costs a later decode while a blocked worker would stall every other clip.
//
// What the pool does *not* do: it knows nothing about timelines, clips'
// timelineStart/sourceIn mapping, LRU policy or the compositor. That is
// DecoderClipFrameProvider's job (DecoderClipFrameProvider.hpp).
//
// Failure policy (Requirement 5.5). A decode failure is *returned*, never
// swallowed and never partially applied: decodeFor() yields the decoder's error
// unchanged so gpu::Compositor::renderAt propagates it and emits no partial
// frame. A failing prefetch job is discarded silently (it was speculative) and
// counted, so the next synchronous request re-attempts and reports the error.
//
// Testability: no FFmpeg, GPU or real media is required — the factory seam takes
// any IDecodeBackend, and the pool's own logic is standard-library only.

#ifndef PALMIER_MEDIA_DECODEWORKERPOOL_HPP
#define PALMIER_MEDIA_DECODEWORKERPOOL_HPP

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/Clip.hpp"
#include "core/Duration.hpp"
#include "core/Result.hpp"
#include "core/Uuid.hpp"
#include "gpu/Compositor.hpp"
#include "media/DecoderTeardownQueue.hpp"
#include "media/MediaDecoder.hpp"

namespace palmier::media {

/// One decoded frame, converted to the compositor's working format and tagged
/// with the *source* position it was decoded for (not a timeline position — the
/// pool has no notion of a timeline).
struct DecodedClipFrame {
    Duration         sourcePosition{};
    gpu::SourceFrame image{};
};

/// Convert a DecodedFrame into the compositor's gpu::SourceFrame.
///
/// Both frame flavours are accepted: a CPU frame's pixel buffer is copied, and a
/// GPU-resident frame is read through its host-visible mapping. Errors:
///   * Unsupported — the frame is not RGBA8, has zero width/height, carries no
///     host-visible pixels, or its buffer is short of width*height*4 bytes.
///   * OutOfRange  — the frame is an end-of-stream marker (no pixels exist).
[[nodiscard]] Result<gpu::SourceFrame> toSourceFrame(const DecodedFrame& frame);

/// Worker/queue geometry of a DecodeWorkerPool. Declared at namespace scope (and
/// aliased as DecodeWorkerPool::Options, mirroring MediaImportOptions) so it can
/// be a defaulted constructor argument.
struct DecodeWorkerPoolOptions {
    /// Worker threads. The design fixes N = 2 for playback.
    std::size_t workerCount{2};
    /// Maximum frames held per clip queue. Bounded so a fast decoder cannot grow
    /// memory without limit while the compositor is behind.
    std::size_t clipQueueCapacity{4};
};

/// Observability counters. Monotonic over a pool's lifetime.
struct DecodeWorkerPoolStats {
    std::uint64_t queueHits{0};         ///< Requests satisfied from a prefetched queue.
    std::uint64_t sequentialDecodes{0}; ///< Synchronous decodes that needed no seek.
    std::uint64_t seeks{0};             ///< Decodes (synchronous or prefetch) preceded by a seek.
    std::uint64_t prefetchedFrames{0};  ///< Frames a worker pushed onto a clip queue.
    std::uint64_t prefetchDrops{0};     ///< Prefetch jobs abandoned (queue full, asset gone, decode failed).
    std::uint64_t decodeFailures{0};    ///< Decode attempts that reported an error.
    std::uint64_t decodersOpened{0};    ///< Successful activateAsset() calls.
    std::uint64_t decodersRetired{0};   ///< Decoders handed to the teardown queue.
};

/// The playback decode stage. Not copyable and not movable: the composition root
/// owns one instance and hands out references.
class DecodeWorkerPool {
public:
    using Options = DecodeWorkerPoolOptions;
    using Stats   = DecodeWorkerPoolStats;

    explicit DecodeWorkerPool(DecoderTeardownQueue& teardown, Options options = Options{});
    ~DecodeWorkerPool();

    DecodeWorkerPool(const DecodeWorkerPool&)            = delete;
    DecodeWorkerPool& operator=(const DecodeWorkerPool&) = delete;
    DecodeWorkerPool(DecodeWorkerPool&&)                 = delete;
    DecodeWorkerPool& operator=(DecodeWorkerPool&&)      = delete;

    // --- Decoder lifecycle --------------------------------------------------

    /// Open one MediaDecoder for `assetId` from `path` using `factory`. A second
    /// activation of an already-active asset succeeds without reopening.
    /// Propagates MediaDecoder::open's error unchanged on failure.
    [[nodiscard]] Result<void> activateAsset(const Uuid& assetId,
                                             const std::filesystem::path& path,
                                             const DecodePrefs& prefs,
                                             const DecodeBackendFactory& factory);

    /// Hand `assetId`'s decoder to the teardown queue and drop every queued
    /// frame that came from it. Returns immediately; the destructor runs on the
    /// teardown thread. Unknown assets are ignored.
    void retireAsset(const Uuid& assetId);

    [[nodiscard]] bool isAssetActive(const Uuid& assetId) const;
    [[nodiscard]] std::size_t activeAssetCount() const;

    /// The source frame interval reported by the asset's primary video stream, or
    /// `fallback` when the asset is unknown or declares no usable frame rate.
    [[nodiscard]] Duration sourceFrameDuration(const Uuid& assetId, Duration fallback) const;

    // --- Frame delivery -----------------------------------------------------

    /// Deliver the frame for `sourcePosition` of `assetId` on behalf of `clipId`.
    ///
    /// Order of attempts: a matching frame already prefetched onto the clip's
    /// queue; otherwise a sequential decode when the decoder's next frame is
    /// exactly the requested one; otherwise a seek followed by a decode.
    /// `frameStep` is the source frame interval and defines both the match
    /// tolerance (half a step) and the next expected position.
    ///
    /// Errors: FailedPrecondition (pool shut down), NotFound (asset not active),
    /// InvalidArgument (non-positive frameStep), OutOfRange (end of stream), or
    /// the decoder's own error unchanged.
    [[nodiscard]] Result<DecodedClipFrame> decodeFor(const Uuid& assetId, const ClipId& clipId,
                                                     Duration sourcePosition, Duration frameStep);

    /// Speculatively decode `frameCount` frames starting at `firstSourcePosition`
    /// onto `clipId`'s queue. Returns immediately; the work runs on the workers.
    void prefetch(const Uuid& assetId, const ClipId& clipId, Duration firstSourcePosition,
                  Duration frameStep, std::size_t frameCount);

    /// Drop every queued frame for `clipId` (e.g. the clip left the timeline).
    void discardClipQueue(const ClipId& clipId);

    [[nodiscard]] std::size_t queuedFrames(const ClipId& clipId) const;

    /// Wait until no prefetch job is queued or in flight. Bounded; reports
    /// whether the pool reached idle. Exists so callers and tests can observe
    /// the workers without sleeping on a wall clock.
    [[nodiscard]] bool drainFor(std::chrono::milliseconds timeout);

    /// Stop the workers, drop pending jobs and queued frames, and retire every
    /// decoder through the teardown queue. Idempotent; called by the destructor.
    void shutdown();

    [[nodiscard]] std::size_t workerCount() const noexcept { return workers_.size(); }
    [[nodiscard]] Stats stats() const;

private:
    /// One active asset: its decoder plus the cursor bookkeeping that decides
    /// sequential-versus-seek. `busy` is the per-asset claim.
    struct AssetEntry {
        std::unique_ptr<MediaDecoder> decoder{};
        std::filesystem::path         path{};
        Duration                      frameDuration{};
        Duration                      nextExpected{};
        bool                          hasExpected{false};
        bool                          busy{false};
    };

    struct Job {
        Uuid     assetId{};
        ClipId   clipId{};
        Duration position{};
        Duration step{};
    };

    /// Claim `assetId` for exclusive use, waiting while another thread holds it.
    /// Returns null when the asset is not active or the pool is shutting down.
    /// Called with `lock` held; may release and re-acquire it while waiting.
    AssetEntry* claimAsset(std::unique_lock<std::mutex>& lock, const Uuid& assetId);
    void        releaseAsset(AssetEntry& entry);

    /// Decode one frame from a *claimed* entry. Runs with the pool mutex
    /// released, which is the whole point of the claim.
    [[nodiscard]] Result<DecodedClipFrame> decodeClaimed(AssetEntry& entry, Duration position,
                                                         bool seekFirst) const;

    void workerLoop();
    void finishJob(std::unique_lock<std::mutex>& lock);

    DecoderTeardownQueue&   teardown_;
    Options                 options_;

    mutable std::mutex      mutex_;
    std::condition_variable jobsAvailable_{};
    std::condition_variable assetFree_{};
    std::condition_variable idle_{};

    std::unordered_map<Uuid, std::unique_ptr<AssetEntry>> assets_{};
    std::unordered_map<Uuid, std::deque<DecodedClipFrame>> clipQueues_{};
    std::deque<Job>          jobs_{};
    std::size_t              inFlight_{0};
    bool                     stopping_{false};
    Stats                    stats_{};
    std::vector<std::thread> workers_{};
};

} // namespace palmier::media

#endif // PALMIER_MEDIA_DECODEWORKERPOOL_HPP
