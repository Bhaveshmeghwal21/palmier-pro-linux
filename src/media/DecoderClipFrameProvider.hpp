// SPDX-License-Identifier: GPL-3.0-or-later
//
// media/DecoderClipFrameProvider.hpp — the decoder-backed implementation of
// gpu::ClipFrameProvider (task 7.3; Requirements 5.1, 5.5, 14.8).
//
// gpu::Compositor already takes its source pixels from an injected seam:
//
//     using ClipFrameProvider = std::function<Result<SourceFrame>(const Clip&, Duration)>;
//
// This component fills that seam with real decoded media, and it is the only
// place in the playback pipeline that knows about *timelines*:
//
//   1. **Mapping.** A request for `(clip, timelinePosition)` becomes the source
//      position `sourceIn + (timelinePosition - timelineStart)`. A position
//      outside `[timelineStart, timelineEnd)` is an error, not a clamp — the
//      compositor only asks about clips it gathered as visible, so an
//      out-of-range request is a caller bug and silently clamping it would
//      present the wrong frame (Requirement 5.1).
//   2. **Decoder residency.** One MediaDecoder per asset, kept in an LRU cache
//      of bounded size. A cache miss opens a decoder; overflow evicts the
//      least-recently-used asset.
//   3. **Teardown.** An eviction hands the retired decoder to
//      media::DecoderTeardownQueue (via DecodeWorkerPool::retireAsset), so
//      closing an FFmpeg decode context never blocks the playback thread
//      (Requirement 14.8, upstream PR 405).
//   4. **Sequential versus seek.** A request for the decoder's next sequential
//      frame decodes straight ahead; anything else seeks first. The decision is
//      made inside DecodeWorkerPool because it has to be atomic with respect to
//      the prefetch workers that share the same decoder cursor.
//   5. **Conversion.** A media::DecodedFrame (CPU buffer or host-mapped
//      GPU-resident frame) becomes a gpu::SourceFrame.
//   6. **Failure.** A decode failure is *returned as an error* naming the asset.
//      gpu::Compositor::renderAt propagates a provider error without writing to
//      the render target, so no partial frame is ever presented (Requirement
//      5.5), and PreviewController turns that error into a pause plus a notice.
//
// Threading. The provider has playback-thread affinity: it is called from
// gpu::Compositor::renderAt on whichever single thread drives presentation, and
// it is not internally synchronised. All concurrency lives in the pool it owns.
//
// Testability. Decoders are opened through MediaDecoder's existing
// DecodeBackendFactory seam, so the whole component runs against a synthetic
// backend with no FFmpeg, no GPU and no media files (see
// tests/media/playback_frame_fidelity_property_test.cpp, Property 20).

#ifndef PALMIER_MEDIA_DECODERCLIPFRAMEPROVIDER_HPP
#define PALMIER_MEDIA_DECODERCLIPFRAMEPROVIDER_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <list>
#include <unordered_map>
#include <vector>

#include "core/Clip.hpp"
#include "core/Duration.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/Result.hpp"
#include "core/Uuid.hpp"
#include "gpu/Compositor.hpp"
#include "media/DecodeWorkerPool.hpp"
#include "media/DecoderTeardownQueue.hpp"
#include "media/MediaDecoder.hpp"

namespace palmier::media {

/// The timeline-to-source mapping this provider implements, exposed because the
/// playback transport and the export planner reason about the same arithmetic.
[[nodiscard]] constexpr Duration clipSourcePosition(const Clip& clip,
                                                    Duration timelinePosition) noexcept {
    return clip.sourceIn + (timelinePosition - clip.timelineStart);
}

/// Configuration of a DecoderClipFrameProvider. Declared at namespace scope (and
/// aliased as DecoderClipFrameProvider::Options) so it can be a defaulted
/// constructor argument.
struct ClipFrameProviderOptions {
    /// Assets whose decoders stay resident. Overflow evicts the LRU asset.
    std::size_t decoderCacheCapacity{3};
    /// Frames the workers decode ahead after each delivered frame. 0 disables
    /// prefetching and makes every request a synchronous decode.
    std::size_t prefetchDepth{2};
    /// Source frame interval used when an asset declares no usable frame rate.
    Duration defaultSourceFrameDuration{Duration::fromNanoseconds(33'333'333)};
    /// Decode preferences handed to every MediaDecoder this provider opens.
    DecodePrefs prefs{};
    /// Worker/queue geometry of the owned DecodeWorkerPool.
    DecodeWorkerPoolOptions pool{};
};

/// Provider-level observability counters. Monotonic over the provider's lifetime.
struct ClipFrameProviderStats {
    std::uint64_t requests{0};       ///< frameFor() calls.
    std::uint64_t delivered{0};      ///< Requests that returned a frame.
    std::uint64_t failures{0};       ///< Requests that returned an error.
    std::uint64_t decodersOpened{0}; ///< Cache misses that opened a decoder.
    std::uint64_t evictions{0};      ///< LRU evictions handed to the teardown queue.
};

/// Decoder-backed gpu::ClipFrameProvider. Not copyable or movable: the
/// composition root owns one and installs a binding of it on the compositor.
class DecoderClipFrameProvider {
public:
    /// Resolves a clip's asset reference to a media path. Production wiring reads
    /// the session's media library; the default resolver uses the reference's own
    /// `sourcePath`, which is what the project document already records.
    using AssetPathResolver =
        std::function<Result<std::filesystem::path>(const MediaAssetRef&)>;

    using Options = ClipFrameProviderOptions;
    using Stats   = ClipFrameProviderStats;

    /// `factory` is MediaDecoder's backend factory seam — pass
    /// ffmpegDecodeBackendFactory() in production, a synthetic backend in tests.
    DecoderClipFrameProvider(DecoderTeardownQueue& teardown, DecodeBackendFactory factory,
                             Options options = Options{}, AssetPathResolver resolver = {});
    ~DecoderClipFrameProvider();

    DecoderClipFrameProvider(const DecoderClipFrameProvider&)            = delete;
    DecoderClipFrameProvider& operator=(const DecoderClipFrameProvider&) = delete;
    DecoderClipFrameProvider(DecoderClipFrameProvider&&)                 = delete;
    DecoderClipFrameProvider& operator=(DecoderClipFrameProvider&&)      = delete;

    /// The gpu::ClipFrameProvider contract: the composited source pixels for
    /// `clip` at `timelinePosition`.
    ///
    /// Errors:
    ///   * InvalidArgument   — the clip names no asset.
    ///   * OutOfRange        — the position lies outside the clip's timeline span,
    ///                         or the mapped source position has no frame.
    ///   * NotFound / Io / Unsupported / FailedPrecondition — from resolving the
    ///     asset path, opening the decoder, or the decode itself; the message
    ///     names the asset (Requirement 5.5).
    [[nodiscard]] Result<gpu::SourceFrame> frameFor(const Clip& clip, Duration timelinePosition);

    /// A gpu::ClipFrameProvider bound to this instance, for
    /// gpu::Compositor::setFrameProvider. The provider must outlive the binding.
    [[nodiscard]] gpu::ClipFrameProvider asProvider();

    /// Retire every resident decoder through the teardown queue and drop every
    /// queued frame (e.g. on project.open). Returns immediately.
    void releaseAll();

    // --- Observability ------------------------------------------------------

    [[nodiscard]] std::size_t residentDecoderCount() const noexcept { return lru_.size(); }
    /// Resident assets, most recently used first.
    [[nodiscard]] std::vector<Uuid> residentAssets() const;
    [[nodiscard]] std::size_t cacheCapacity() const noexcept {
        return options_.decoderCacheCapacity;
    }
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    /// Pool-level counters: queue hits, sequential decodes, seeks, prefetches.
    [[nodiscard]] DecodeWorkerPool::Stats poolStats() const { return pool_.stats(); }
    [[nodiscard]] const DecodeWorkerPool& pool() const noexcept { return pool_; }
    [[nodiscard]] DecodeWorkerPool& pool() noexcept { return pool_; }

private:
    /// Make `asset` resident, opening a decoder on a miss and evicting the LRU
    /// asset when the cache overflows.
    [[nodiscard]] Result<void> ensureResident(const MediaAssetRef& asset);
    void                       touch(const Uuid& assetId);
    void                       evictLeastRecentlyUsed();

    DecodeBackendFactory                                     factory_;
    Options                                                  options_;
    AssetPathResolver                                        resolver_;
    DecodeWorkerPool                                         pool_;
    std::list<Uuid>                                          lru_{}; ///< front = most recent.
    std::unordered_map<Uuid, std::list<Uuid>::iterator>      resident_{};
    Stats                                                    stats_{};
};

} // namespace palmier::media

#endif // PALMIER_MEDIA_DECODERCLIPFRAMEPROVIDER_HPP
