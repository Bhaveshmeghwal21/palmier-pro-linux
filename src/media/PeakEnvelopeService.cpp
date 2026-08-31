// SPDX-License-Identifier: GPL-3.0-or-later
//
// media/PeakEnvelopeService.cpp — implementation of off-thread envelope
// production.

#include "media/PeakEnvelopeService.hpp"

#include <utility>

namespace palmier::media {

PeakEnvelopeService::PeakEnvelopeService(DecodePrefs prefs, DecodeBackendFactory factory,
                                        Options options)
    : prefs_(std::move(prefs)),
      factory_(std::move(factory)),
      options_(options),
      cache_(options.cacheCapacity) {
    const std::size_t count = options_.workerCount == 0 ? 1 : options_.workerCount;
    workers_.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        workers_.emplace_back([this] { workerLoop(); });
    }
}

PeakEnvelopeService::~PeakEnvelopeService() { shutdown(); }

void PeakEnvelopeService::setReadyCallback(ReadyCallback callback) {
    const std::lock_guard<std::mutex> lock(mutex_);
    ready_ = std::move(callback);
}

EnvelopeLookup PeakEnvelopeService::lookupFrom(const EnvelopeCacheEntry& entry) {
    EnvelopeLookup out;
    out.pending = false;
    out.failed = entry.failed;
    out.failure = entry.failure;
    out.envelope = entry.envelope;
    return out;
}

EnvelopeLookup PeakEnvelopeService::lookup(const Uuid& assetId,
                                           const std::filesystem::path& path) {
    std::unique_lock<std::mutex> lock(mutex_);

    if (const EnvelopeCacheEntry* entry = cache_.find(assetId); entry != nullptr) {
        if (entry->failed) {
            // Requirement 2.7: remembered, not retried. Counted so a repainting
            // caller drives this number up while the decoder is never touched.
            ++stats_.failuresNotRetried;
        }
        return lookupFrom(*entry);
    }

    EnvelopeLookup pending;
    pending.pending = true;

    if (stopping_) {
        // Shutting down: report pending rather than queueing work that will never
        // run, so a late repaint draws nothing instead of waiting on a dead worker.
        return pending;
    }

    if (inFlight_.find(assetId) != inFlight_.end()) {
        // Already queued or running. This is the common case during a repaint
        // storm and the reason one asset is decoded once however many clips
        // reference it.
        ++stats_.duplicatesSuppressed;
        return pending;
    }

    inFlight_.insert(assetId);
    jobs_.push_back(Job{assetId, path});
    ++stats_.scheduled;
    lock.unlock();
    jobsAvailable_.notify_one();
    return pending;
}

EnvelopeLookup PeakEnvelopeService::peek(const Uuid& assetId) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (const EnvelopeCacheEntry* entry = cache_.peek(assetId); entry != nullptr) {
        return lookupFrom(*entry);
    }
    EnvelopeLookup pending;
    // Pending only if work is actually outstanding; otherwise this asset is simply
    // unknown, and saying "pending" would promise an answer nobody is computing.
    pending.pending = inFlight_.find(assetId) != inFlight_.end();
    return pending;
}

bool PeakEnvelopeService::forget(const Uuid& assetId) {
    const std::lock_guard<std::mutex> lock(mutex_);
    return cache_.forget(assetId);
}

bool PeakEnvelopeService::drainFor(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return idle_.wait_for(lock, timeout,
                          [this] { return jobs_.empty() && running_ == 0; });
}

void PeakEnvelopeService::shutdown() {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ && workers_.empty()) return;
        stopping_ = true;
        jobs_.clear();
        inFlight_.clear();
    }
    jobsAvailable_.notify_all();
    for (std::thread& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
    workers_.clear();
    idle_.notify_all();
}

PeakEnvelopeServiceStats PeakEnvelopeService::stats() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

EnvelopeCacheStats PeakEnvelopeService::cacheStats() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return cache_.stats();
}

std::size_t PeakEnvelopeService::cachedAssetCount() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return cache_.size();
}

std::size_t PeakEnvelopeService::outstandingJobs() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return jobs_.size() + running_;
}

void PeakEnvelopeService::workerLoop() {
    while (true) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            jobsAvailable_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
            if (stopping_) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
            ++running_;
        }

        // The decode runs with the mutex RELEASED — that is the whole point of the
        // service. A lookup on the UI thread proceeds while this is in progress.
        Result<PeakEnvelope> extracted =
            extractPeakEnvelope(job.path, options_.bucketDuration, prefs_, factory_,
                                options_.limits);

        ReadyCallback ready;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (extracted.isError()) {
                cache_.storeFailure(job.assetId, std::string(extracted.error().message()));
                ++stats_.failed;
            } else {
                PeakEnvelope envelope = std::move(extracted).value();
                const bool   silent = envelope.empty();
                cache_.store(job.assetId, std::move(envelope));
                if (silent) {
                    ++stats_.silent;
                } else {
                    ++stats_.completed;
                }
            }
            inFlight_.erase(job.assetId);
            --running_;
            ready = ready_;
        }

        // Fired with the mutex released so a callee that synchronously calls back
        // into peek()/lookup() cannot deadlock.
        if (ready) ready(job.assetId);
        idle_.notify_all();
    }
}

} // namespace palmier::media
