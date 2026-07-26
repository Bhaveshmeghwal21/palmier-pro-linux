// SPDX-License-Identifier: GPL-3.0-or-later
//
// media/DecoderTeardownQueue.hpp — hand a decoder over for destruction on a
// dedicated thread so the caller never blocks (task 7.1; Requirement 14.8).
//
// This is the Linux adaptation of upstream PR 405. Closing a MediaDecoder tears
// down the FFmpeg decode context (avcodec_free_context / avformat_close_input,
// plus any hardware frames context), which can block for hundreds of
// milliseconds. That must never happen on the playback thread, the audio
// callback thread or the Qt main thread: 100 successive stop/seek operations
// each have to complete within 2 seconds and the whole sequence has to finish
// without deadlock (Requirement 14.8).
//
// The queue owns exactly one worker thread. A retired object is *moved in* as a
// std::unique_ptr and the calling thread returns immediately; the destructor
// runs later, on the worker, outside every lock the queue holds.
//
// Design notes (see design.md D5 "Threading model", the "Decoder teardown" row):
//
//   * retire() is the only hand-over point and is safe to call concurrently from
//     any number of threads.
//   * Draining is *observable* — pending(), retiredCount() and drain() /
//     drainFor() let a caller (and a test) wait for the queue to reach empty
//     without sleeping and without polling a wall clock.
//   * Destruction stops accepting work, destroys everything still queued and
//     joins the worker. Nothing queued is ever leaked or lost, and a blocking
//     destructor delays the join rather than deadlocking it.
//   * retire() is templated over the owned type, so the audio decoder (stage 8),
//     DecoderClipFrameProvider's LRU evictions (task 7.3) and the export path
//     (task 9.4) reuse one queue for any resource whose destructor is slow. The
//     MediaDecoder overload is the documented primary API.

#ifndef PALMIER_MEDIA_DECODERTEARDOWNQUEUE_HPP
#define PALMIER_MEDIA_DECODERTEARDOWNQUEUE_HPP

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace palmier::media {

class MediaDecoder;

namespace detail {

/// Type-erased owner of one retired object. Destroying the holder destroys the
/// owned object, which is how the worker thread runs the slow destructor.
class TeardownItem {
public:
    virtual ~TeardownItem() = default;
};

template <class T>
class TypedTeardownItem final : public TeardownItem {
public:
    explicit TypedTeardownItem(std::unique_ptr<T> owned) noexcept : owned_(std::move(owned)) {}

private:
    std::unique_ptr<T> owned_;
};

} // namespace detail

/// A single-threaded destruction service for objects whose destructors block.
///
/// Not copyable and not movable: callers hold a reference to the one instance
/// the composition root owns.
class DecoderTeardownQueue {
public:
    DecoderTeardownQueue();
    ~DecoderTeardownQueue();

    DecoderTeardownQueue(const DecoderTeardownQueue&)            = delete;
    DecoderTeardownQueue& operator=(const DecoderTeardownQueue&) = delete;
    DecoderTeardownQueue(DecoderTeardownQueue&&)                 = delete;
    DecoderTeardownQueue& operator=(DecoderTeardownQueue&&)      = delete;

    /// Hand `decoder` over for destruction and return immediately. A null
    /// pointer is ignored (and is not counted as accepted work).
    ///
    /// After shutdown() the worker is gone, so the object is destroyed inline on
    /// the calling thread instead of being lost — the accounting still balances.
    void retire(std::unique_ptr<MediaDecoder> decoder);

    /// The generic form: retire any owned object whose destructor may block.
    /// Same contract as the MediaDecoder overload.
    template <class T>
    void retire(std::unique_ptr<T> owned) {
        if (!owned) return;
        submit(std::make_unique<detail::TypedTeardownItem<T>>(std::move(owned)));
    }

    /// Number of objects accepted but not yet destroyed — queued plus the one
    /// currently being destroyed. Zero means the queue has drained to empty.
    [[nodiscard]] std::size_t pending() const noexcept;

    /// Objects accepted over this queue's lifetime (monotonic).
    [[nodiscard]] std::uint64_t acceptedCount() const noexcept;

    /// Objects fully destroyed over this queue's lifetime (monotonic). Equals
    /// acceptedCount() exactly when pending() is zero.
    [[nodiscard]] std::uint64_t retiredCount() const noexcept;

    /// Block until pending() reaches zero. Must not be called from a destructor
    /// running on the worker thread.
    void drain();

    /// Bounded drain: waits up to `timeout` for pending() to reach zero and
    /// reports whether it got there. Lets a caller bound its wait instead of
    /// hanging when a destructor misbehaves.
    [[nodiscard]] bool drainFor(std::chrono::milliseconds timeout);

    /// Stop accepting work, destroy everything still queued, join the worker.
    /// Idempotent; called by the destructor.
    void shutdown();

    /// True while the worker thread is accepting work.
    [[nodiscard]] bool running() const noexcept;

private:
    void submit(std::unique_ptr<detail::TeardownItem> item);
    void workerLoop();

    mutable std::mutex                                mutex_;
    std::condition_variable                           work_{};
    std::condition_variable                           idle_{};
    std::deque<std::unique_ptr<detail::TeardownItem>> queue_{};
    bool                                              stopping_{false};
    bool                                              busy_{false};
    std::uint64_t                                     accepted_{0};
    std::uint64_t                                     retired_{0};
    std::thread                                       worker_{};
};

} // namespace palmier::media

#endif // PALMIER_MEDIA_DECODERTEARDOWNQUEUE_HPP
