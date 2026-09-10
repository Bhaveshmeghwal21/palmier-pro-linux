// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/ExportProgress.cpp - how an export reports itself to the rest of the
// application: offering progress without blocking the worker, pumping queued
// notifications on the caller's thread, and the waits that let a caller join a
// running export.
//
// Split out of services/ExportCoordinator.cpp; the shared contract is
// services/ExportCoordinatorInternal.hpp. Every definition was moved, not rewritten.

#include "services/ExportCoordinatorInternal.hpp"

namespace palmier::services {

using namespace palmier::services::detail;  // NOLINT(google-build-using-namespace)

void ExportCoordinator::offerProgress(int percent, std::size_t framesEncoded,
                                      std::size_t totalFrames, Duration position, bool force) {
    // Monotonic and bounded: a percentage never regresses and never leaves
    // [0, 100], whatever the engine reported.
    int clamped = std::clamp(percent, 0, 100);
    if (clamped < lastPercent_) clamped = lastPercent_;

    const auto now = options_.clock();
    // The interval is a CEILING on the gap between reports: reporting less often
    // than it is what Requirement 7.3 forbids, so a report that is not yet due is
    // simply dropped. Because the engine reports every frame, the next due frame
    // carries the newest percentage — nothing is lost by dropping this one.
    if (!force && lastEmitAt_.has_value() && (now - *lastEmitAt_) < options_.progressInterval) {
        return;
    }

    lastPercent_ = clamped;
    lastEmitAt_ = now;

    Notifier notifier;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        progressQueue_.push_back(
            ExportProgressReport{clamped, framesEncoded, totalFrames, position});
        notifier = notifier_;
    }
    cv_.notify_all();
    if (notifier) notifier();
}

void ExportCoordinator::finishExport(QueuedOutcome outcome) {
    Notifier notifier;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        outcomeQueue_ = std::move(outcome);
        notifier = notifier_;
    }
    // Cleared only after the outcome is queued, so an owner that observes
    // running() == false always finds the outcome available.
    running_.store(false);
    cv_.notify_all();
    if (notifier) notifier();
}

std::size_t ExportCoordinator::pump() {
    std::deque<ExportProgressReport> progress;
    std::optional<QueuedOutcome> outcome;
    ProgressSink progressSink;
    CompletionSink completionSink;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        progress.swap(progressQueue_);
        outcome = std::move(outcomeQueue_);
        outcomeQueue_.reset();
        progressSink = progressSink_;
        completionSink = completionSink_;
    }

    std::size_t delivered = 0;
    for (const ExportProgressReport& report : progress) {
        deliveredProgress_.push_back(report);
        if (progressSink) progressSink(report);
        ++delivered;
    }

    if (outcome.has_value()) {
        lastSelection_ = outcome->selection;
        if (outcome->succeeded) {
            lastOutcome_ = outcome->outcome;
            lastError_.reset();
            if (completionSink) {
                completionSink(Result<ExportOutcome>(outcome->outcome));
            }
        } else {
            lastOutcome_ = outcome->outcome;
            lastError_ = outcome->error;
            if (completionSink) {
                completionSink(err<ExportOutcome>(outcome->error));
            }
        }
        ++delivered;
    }
    return delivered;
}

bool ExportCoordinator::waitForCompletion(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    // Bounded by construction: a coordinator that never finishes returns false
    // rather than blocking a caller (or a test) forever.
    return cv_.wait_for(lock, timeout,
                        [this]() { return outcomeQueue_.has_value() || !running_.load(); });
}

std::size_t ExportCoordinator::awaitCompletion(std::chrono::milliseconds timeout) {
    if (!waitForCompletion(timeout)) {
        return 0;
    }
    joinWorker();
    return pump();
}

}  // namespace palmier::services
