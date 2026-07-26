// SPDX-License-Identifier: GPL-3.0-or-later
//
// media/DecodeWorkerPool.cpp — implementation of the playback decode stage
// (task 7.3; Requirements 5.1, 5.5, 14.8). See DecodeWorkerPool.hpp for the
// contract and the rationale.
//
// Locking discipline, stated once because every method depends on it:
//
//   * `mutex_` guards *all* shared state: the asset table, the per-clip queues,
//     the job deque, the counters and the stopping flag. It is held only for
//     short, allocation-bounded critical sections.
//   * Decoding runs with `mutex_` *released*, under the per-asset `busy` claim.
//     A claim is what makes "one MediaDecoder per active asset, touched by one
//     thread at a time" true without holding the pool lock across an FFmpeg
//     call.
//   * An AssetEntry lives behind a unique_ptr in the table, so its address is
//     stable across rehashes and a claimed entry can be used after the lock is
//     released. retireAsset() and shutdown() wait for the claim before removing
//     it, so a claimed entry is never destroyed under its user.
//
// There is exactly one mutex, so no lock-ordering deadlock is possible; every
// wait is on a condition that another thread signals while holding it.

#include "media/DecodeWorkerPool.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

#include "core/Error.hpp"

namespace palmier::media {
namespace {

/// Half-open tolerance for "this frame satisfies that request": half a source
/// frame interval, so a request lands on exactly one decoded frame.
[[nodiscard]] bool satisfies(Duration have, Duration want, Duration step) noexcept {
    const std::int64_t tolerance = step.ticks() / 2;
    return (have - want).abs().ticks() <= tolerance;
}

[[nodiscard]] std::string describe(const Uuid& assetId, const std::filesystem::path& path) {
    std::string out = "asset ";
    out += assetId.toString();
    if (!path.empty()) {
        out += " (";
        out += path.string();
        out += ")";
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// DecodedFrame -> gpu::SourceFrame
// ---------------------------------------------------------------------------

Result<gpu::SourceFrame> toSourceFrame(const DecodedFrame& frame) {
    if (frame.isEndOfStream()) {
        return err<gpu::SourceFrame>(
            outOfRange("cannot convert an end-of-stream marker to a source frame"));
    }

    const gpu::FrameDesc desc = frame.desc();
    if (desc.format != gpu::FrameFormat::RGBA8) {
        return err<gpu::SourceFrame>(
            unsupported("playback requires RGBA8 decoded frames; the decoder produced another "
                        "pixel format"));
    }
    if (desc.width == 0 || desc.height == 0) {
        return err<gpu::SourceFrame>(unsupported("decoded frame has a zero dimension"));
    }

    const std::size_t bytes = static_cast<std::size_t>(desc.width) * desc.height * 4u;

    const void* source = nullptr;
    std::size_t available = 0;
    if (frame.isCpu()) {
        source    = frame.cpuPixels().data();
        available = frame.cpuPixels().size();
    } else if (frame.isGpuResident() && frame.gpuFrame().hostData() != nullptr) {
        source    = frame.gpuFrame().hostData();
        available = bytes; // A pooled/imported frame is allocated for its own desc.
    }

    if (source == nullptr) {
        return err<gpu::SourceFrame>(
            unsupported("decoded frame carries no host-visible pixels to composite"));
    }
    if (available < bytes) {
        return err<gpu::SourceFrame>(unsupported("decoded frame pixel buffer is shorter than its "
                                                 "declared geometry"));
    }

    gpu::SourceFrame out;
    out.width  = desc.width;
    out.height = desc.height;
    out.rgba.resize(bytes);
    std::memcpy(out.rgba.data(), source, bytes);
    return out;
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

DecodeWorkerPool::DecodeWorkerPool(DecoderTeardownQueue& teardown, Options options)
    : teardown_(teardown), options_(options) {
    if (options_.workerCount == 0) options_.workerCount = 1;
    if (options_.clipQueueCapacity == 0) options_.clipQueueCapacity = 1;

    workers_.reserve(options_.workerCount);
    for (std::size_t i = 0; i < options_.workerCount; ++i) {
        workers_.emplace_back([this] { workerLoop(); });
    }
}

DecodeWorkerPool::~DecodeWorkerPool() { shutdown(); }

// ---------------------------------------------------------------------------
// Per-asset claim
// ---------------------------------------------------------------------------

DecodeWorkerPool::AssetEntry* DecodeWorkerPool::claimAsset(std::unique_lock<std::mutex>& lock,
                                                           const Uuid& assetId) {
    for (;;) {
        if (stopping_) return nullptr;
        const auto it = assets_.find(assetId);
        if (it == assets_.end()) return nullptr;
        AssetEntry* entry = it->second.get();
        if (!entry->busy) {
            entry->busy = true;
            return entry;
        }
        assetFree_.wait(lock);
    }
}

void DecodeWorkerPool::releaseAsset(AssetEntry& entry) {
    entry.busy = false;
    assetFree_.notify_all();
}

// ---------------------------------------------------------------------------
// Decoder lifecycle
// ---------------------------------------------------------------------------

Result<void> DecodeWorkerPool::activateAsset(const Uuid& assetId,
                                             const std::filesystem::path& path,
                                             const DecodePrefs& prefs,
                                             const DecodeBackendFactory& factory) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return err(failedPrecondition("decode worker pool is shut down"));
        if (assets_.find(assetId) != assets_.end()) return ok();
    }

    // Opening happens outside the lock: it touches the filesystem and, in a real
    // build, FFmpeg. Nothing else in the pool is blocked while it runs.
    auto opened = MediaDecoder::open(path, prefs, factory);
    if (opened.isError()) return err(std::move(opened).error());

    auto entry     = std::make_unique<AssetEntry>();
    entry->decoder = std::make_unique<MediaDecoder>(std::move(opened).value());
    entry->path    = path;
    if (const MediaStreamInfo* video = entry->decoder->info().primaryVideoStream();
        video != nullptr && video->frameRate.isValid()) {
        entry->frameDuration = video->frameRate.frameDuration();
    }

    std::unique_lock<std::mutex> lock(mutex_);
    if (stopping_) {
        lock.unlock();
        teardown_.retire(std::move(entry->decoder));
        return err(failedPrecondition("decode worker pool is shut down"));
    }
    // A concurrent activation may have won the race; keep the existing decoder
    // and retire the loser rather than replacing a decoder someone may be using.
    if (assets_.find(assetId) != assets_.end()) {
        lock.unlock();
        teardown_.retire(std::move(entry->decoder));
        return ok();
    }
    assets_.emplace(assetId, std::move(entry));
    ++stats_.decodersOpened;
    return ok();
}

void DecodeWorkerPool::retireAsset(const Uuid& assetId) {
    std::unique_lock<std::mutex> lock(mutex_);
    // Wait for whoever is decoding on this asset to finish before taking it.
    for (;;) {
        const auto it = assets_.find(assetId);
        if (it == assets_.end()) return;
        if (!it->second->busy) break;
        assetFree_.wait(lock);
    }

    const auto it = assets_.find(assetId);
    if (it == assets_.end()) return;
    std::unique_ptr<AssetEntry> entry = std::move(it->second);
    assets_.erase(it);
    ++stats_.decodersRetired;

    // Frames already queued came from this decoder; they are no longer valid
    // playback candidates once the asset leaves the cache.
    for (auto& [clipId, queue] : clipQueues_) {
        (void)clipId;
        queue.clear();
    }
    lock.unlock();
    assetFree_.notify_all();

    // The whole point of the queue: the caller does not pay the close cost.
    teardown_.retire(std::move(entry->decoder));
}

bool DecodeWorkerPool::isAssetActive(const Uuid& assetId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return assets_.find(assetId) != assets_.end();
}

std::size_t DecodeWorkerPool::activeAssetCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return assets_.size();
}

Duration DecodeWorkerPool::sourceFrameDuration(const Uuid& assetId, Duration fallback) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = assets_.find(assetId);
    if (it == assets_.end()) return fallback;
    if (it->second->frameDuration.isPositive()) return it->second->frameDuration;
    return fallback;
}

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------

Result<DecodedClipFrame> DecodeWorkerPool::decodeClaimed(AssetEntry& entry, Duration position,
                                                        bool seekFirst) const {
    if (seekFirst) {
        auto sought = entry.decoder->seek(position);
        if (sought.isError()) return err<DecodedClipFrame>(std::move(sought).error());
    }

    auto decoded = entry.decoder->nextFrame();
    if (decoded.isError()) return err<DecodedClipFrame>(std::move(decoded).error());

    DecodedFrame frame = std::move(decoded).value();
    if (frame.isEndOfStream()) {
        return err<DecodedClipFrame>(
            outOfRange("the source has no frame at " + std::to_string(position.milliseconds()) +
                       " ms (end of stream)"));
    }

    auto image = toSourceFrame(frame);
    if (image.isError()) return err<DecodedClipFrame>(std::move(image).error());

    DecodedClipFrame out;
    out.sourcePosition = position;
    out.image          = std::move(image).value();
    return out;
}

Result<DecodedClipFrame> DecodeWorkerPool::decodeFor(const Uuid& assetId, const ClipId& clipId,
                                                    Duration sourcePosition, Duration frameStep) {
    if (!frameStep.isPositive()) {
        return err<DecodedClipFrame>(
            invalidArgument("a source frame interval must be positive to locate a frame"));
    }

    std::unique_lock<std::mutex> lock(mutex_);
    if (stopping_) {
        return err<DecodedClipFrame>(failedPrecondition("decode worker pool is shut down"));
    }

    // 1. A prefetched frame for exactly this position, if the workers got there
    //    first. Anything else at the head is stale and discarded.
    if (const auto queueIt = clipQueues_.find(clipId); queueIt != clipQueues_.end()) {
        auto& queue = queueIt->second;
        while (!queue.empty()) {
            if (satisfies(queue.front().sourcePosition, sourcePosition, frameStep)) {
                DecodedClipFrame frame = std::move(queue.front());
                queue.pop_front();
                ++stats_.queueHits;
                return frame;
            }
            queue.pop_front();
        }
    }

    // 2. Decode it now, on this thread, under the asset claim.
    AssetEntry* entry = claimAsset(lock, assetId);
    if (entry == nullptr) {
        if (stopping_) {
            return err<DecodedClipFrame>(failedPrecondition("decode worker pool is shut down"));
        }
        return err<DecodedClipFrame>(
            notFound("no decoder is active for asset " + assetId.toString()));
    }

    const bool seekFirst =
        !entry->hasExpected || !satisfies(entry->nextExpected, sourcePosition, frameStep);
    const std::filesystem::path path = entry->path;

    lock.unlock();
    Result<DecodedClipFrame> decoded = decodeClaimed(*entry, sourcePosition, seekFirst);
    lock.lock();

    if (seekFirst) {
        ++stats_.seeks;
    } else {
        ++stats_.sequentialDecodes;
    }

    if (decoded.isOk()) {
        entry->hasExpected  = true;
        entry->nextExpected = sourcePosition + frameStep;
    } else {
        // The cursor is unknown after a failure, so the next request seeks.
        entry->hasExpected = false;
        ++stats_.decodeFailures;
    }
    releaseAsset(*entry);
    lock.unlock();

    if (decoded.isError()) {
        // Requirement 5.5: the failure names the asset and is returned, so the
        // compositor emits no partial frame.
        const Error original = decoded.error();
        return err<DecodedClipFrame>(makeError(original.code(),
                                               "decode failed for " + describe(assetId, path) +
                                                   ": " + original.message()));
    }
    return decoded;
}

void DecodeWorkerPool::prefetch(const Uuid& assetId, const ClipId& clipId,
                                Duration firstSourcePosition, Duration frameStep,
                                std::size_t frameCount) {
    if (frameCount == 0 || !frameStep.isPositive()) return;

    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    if (assets_.find(assetId) == assets_.end()) return;

    const std::size_t queued = [&] {
        const auto it = clipQueues_.find(clipId);
        return it == clipQueues_.end() ? 0u : it->second.size();
    }();
    if (queued >= options_.clipQueueCapacity) return;

    const std::size_t room    = options_.clipQueueCapacity - queued;
    const std::size_t planned = std::min(frameCount, room);
    for (std::size_t i = 0; i < planned; ++i) {
        Job job;
        job.assetId  = assetId;
        job.clipId   = clipId;
        job.position = firstSourcePosition + frameStep * static_cast<std::int64_t>(i);
        job.step     = frameStep;
        jobs_.push_back(job);
    }
    jobsAvailable_.notify_all();
}

void DecodeWorkerPool::discardClipQueue(const ClipId& clipId) {
    std::lock_guard<std::mutex> lock(mutex_);
    clipQueues_.erase(clipId);
}

std::size_t DecodeWorkerPool::queuedFrames(const ClipId& clipId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = clipQueues_.find(clipId);
    return it == clipQueues_.end() ? 0u : it->second.size();
}

// ---------------------------------------------------------------------------
// Workers
// ---------------------------------------------------------------------------

void DecodeWorkerPool::finishJob(std::unique_lock<std::mutex>& lock) {
    (void)lock; // documents that the caller holds the lock
    if (inFlight_ > 0) --inFlight_;
    if (jobs_.empty() && inFlight_ == 0) idle_.notify_all();
}

void DecodeWorkerPool::workerLoop() {
    for (;;) {
        std::unique_lock<std::mutex> lock(mutex_);
        jobsAvailable_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
        if (stopping_) return;

        Job job = jobs_.front();
        jobs_.pop_front();
        ++inFlight_;

        const std::size_t queued = [&] {
            const auto it = clipQueues_.find(job.clipId);
            return it == clipQueues_.end() ? 0u : it->second.size();
        }();
        if (queued >= options_.clipQueueCapacity) {
            ++stats_.prefetchDrops;
            finishJob(lock);
            continue;
        }

        AssetEntry* entry = claimAsset(lock, job.assetId);
        if (entry == nullptr) {
            if (stopping_) {
                finishJob(lock);
                return;
            }
            ++stats_.prefetchDrops;
            finishJob(lock);
            continue;
        }

        const bool seekFirst =
            !entry->hasExpected || !satisfies(entry->nextExpected, job.position, job.step);

        lock.unlock();
        Result<DecodedClipFrame> decoded = decodeClaimed(*entry, job.position, seekFirst);
        lock.lock();

        if (seekFirst) ++stats_.seeks;

        if (decoded.isOk()) {
            entry->hasExpected  = true;
            entry->nextExpected = job.position + job.step;
            auto& queue         = clipQueues_[job.clipId];
            if (queue.size() < options_.clipQueueCapacity) {
                queue.push_back(std::move(decoded).value());
                ++stats_.prefetchedFrames;
            } else {
                ++stats_.prefetchDrops;
            }
        } else {
            entry->hasExpected = false;
            ++stats_.decodeFailures;
            // Speculative work: the failure is counted, not reported. The next
            // synchronous decodeFor() re-attempts and returns the error.
            ++stats_.prefetchDrops;
        }

        releaseAsset(*entry);
        finishJob(lock);
    }
}

bool DecodeWorkerPool::drainFor(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return idle_.wait_for(lock, timeout, [this] { return jobs_.empty() && inFlight_ == 0; });
}

void DecodeWorkerPool::shutdown() {
    std::vector<std::thread> workers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ && workers_.empty()) return;
        stopping_ = true;
        jobs_.clear();
        workers.swap(workers_);
    }
    jobsAvailable_.notify_all();
    assetFree_.notify_all();
    idle_.notify_all();

    for (auto& worker : workers) {
        if (worker.joinable()) worker.join();
    }

    // Every decoder still resident is retired through the teardown queue, so
    // even shutdown does not run an FFmpeg close on this thread.
    std::vector<std::unique_ptr<MediaDecoder>> retiring;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        clipQueues_.clear();
        for (auto& [assetId, entry] : assets_) {
            (void)assetId;
            if (entry->decoder) retiring.push_back(std::move(entry->decoder));
        }
        stats_.decodersRetired += retiring.size();
        assets_.clear();
    }
    for (auto& decoder : retiring) teardown_.retire(std::move(decoder));
}

DecodeWorkerPool::Stats DecodeWorkerPool::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

} // namespace palmier::media
