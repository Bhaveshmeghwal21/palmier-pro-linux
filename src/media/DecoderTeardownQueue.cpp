// SPDX-License-Identifier: GPL-3.0-or-later
//
// media/DecoderTeardownQueue.cpp — the single-threaded teardown worker
// (task 7.1; Requirement 14.8).
//
// The whole point of this file is that no lock is held while a retired object is
// destroyed: the worker pops the front item under the mutex, releases the mutex,
// then lets the item go out of scope. A destructor that blocks inside FFmpeg for
// hundreds of milliseconds therefore blocks nothing except the worker's own next
// pop — retire(), pending(), acceptedCount() and retiredCount() all stay
// responsive, which is what makes 100 successive stop/seek operations each
// complete promptly (Requirement 14.8).

#include "media/DecoderTeardownQueue.hpp"

#include "media/MediaDecoder.hpp"

namespace palmier::media {

DecoderTeardownQueue::DecoderTeardownQueue()
    : worker_([this] { workerLoop(); }) {}

DecoderTeardownQueue::~DecoderTeardownQueue() {
    shutdown();
}

void DecoderTeardownQueue::retire(std::unique_ptr<MediaDecoder> decoder) {
    if (!decoder) return;
    submit(std::make_unique<detail::TypedTeardownItem<MediaDecoder>>(std::move(decoder)));
}

void DecoderTeardownQueue::submit(std::unique_ptr<detail::TeardownItem> item) {
    if (!item) return;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++accepted_;
        if (!stopping_) {
            queue_.push_back(std::move(item));
            work_.notify_one();
            return;
        }
    }

    // The worker has already been joined (shutdown() ran, or is running on
    // another thread). Destroying the object inline is the only way to keep the
    // "nothing queued is ever lost" guarantee, and it cannot deadlock: no lock
    // is held here.
    item.reset();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++retired_;
        if (accepted_ == retired_) idle_.notify_all();
    }
}

void DecoderTeardownQueue::workerLoop() {
    for (;;) {
        std::unique_ptr<detail::TeardownItem> item;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            work_.wait(lock, [this] { return !queue_.empty() || stopping_; });

            if (queue_.empty()) {
                // Stopping with nothing left to destroy: the queue has drained.
                if (stopping_) return;
                continue;
            }

            item = std::move(queue_.front());
            queue_.pop_front();
            busy_ = true;
        }

        // The slow part, deliberately outside the mutex.
        item.reset();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            busy_ = false;
            ++retired_;
            if (accepted_ == retired_) idle_.notify_all();
        }
    }
}

std::size_t DecoderTeardownQueue::pending() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<std::size_t>(accepted_ - retired_);
}

std::uint64_t DecoderTeardownQueue::acceptedCount() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return accepted_;
}

std::uint64_t DecoderTeardownQueue::retiredCount() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return retired_;
}

void DecoderTeardownQueue::drain() {
    std::unique_lock<std::mutex> lock(mutex_);
    idle_.wait(lock, [this] { return accepted_ == retired_; });
}

bool DecoderTeardownQueue::drainFor(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return idle_.wait_for(lock, timeout, [this] { return accepted_ == retired_; });
}

void DecoderTeardownQueue::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ && !worker_.joinable()) return;
        stopping_ = true;
        // Notified under the lock: the worker is the only waiter, and signalling
        // while holding the associated mutex keeps race detectors quiet without
        // changing the (already correct) wakeup guarantee.
        work_.notify_all();
    }

    if (worker_.joinable()) worker_.join();

    // The worker only returns once the queue is empty, so this is belt and
    // braces for the pathological case of a retire() that raced the join.
    std::deque<std::unique_ptr<detail::TeardownItem>> leftovers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        leftovers.swap(queue_);
    }
    const std::uint64_t destroyed = static_cast<std::uint64_t>(leftovers.size());
    leftovers.clear();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        retired_ += destroyed;
        idle_.notify_all();
    }
}

bool DecoderTeardownQueue::running() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return !stopping_;
}

} // namespace palmier::media
