// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/media/decoder_teardown_property_test.cpp — Property 75 for
// media::DecoderTeardownQueue (task 7.2 of the end-to-end-editor-integration
// spec; Requirement 14.8, upstream PR 405).
//
// Exactly one property lives here:
//
//   * Property 75 — decoder teardown never deadlocks or stalls: retiring a
//     decoder returns to the caller regardless of how slow that decoder's
//     destructor is, every retired decoder is eventually destroyed exactly once,
//     and destroying the queue itself terminates.
//
// How the property avoids wall-clock guessing. The subject under test is a slow
// destructor, so the fake decoder's destructor *does* block — but it blocks on a
// gate the test controls, not on a sleep the test hopes is long enough:
//
//   1. the gate starts closed, so no destructor can complete;
//   2. every retire() call is made and returns;
//   3. the test asserts that *zero* destructors have completed — pure ordering.
//      A synchronous hand-over would have had to run a destructor to return, so
//      this single assertion is what proves the caller is not waiting on FFmpeg;
//   4. the gate opens, the queue drains, and the ledger is checked for
//      exactly-once destruction.
//
// Every blocking wait in the test is bounded (the destructor's gate wait uses
// the 2-second bound Requirement 14.8 names, the drain uses a generous ceiling),
// so a regression that reintroduces synchronous teardown *fails* with a
// counterexample instead of hanging CI under the harness's 600-second limit.
//
// The decoder double. No new seam was needed for the property: the queue's
// retire() is templated over the owned type, so a fake with a deliberately slow
// destructor is retired directly. The real MediaDecoder overload is covered by
// the unit tests at the bottom of this file through MediaDecoder's existing
// IDecodeBackend / DecodeBackendFactory seam, with a mock backend whose own
// destructor blocks — which is where the FFmpeg close cost actually lives. No
// FFmpeg, GPU or real media is required by anything in this file.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include "core/Duration.hpp"
#include "core/Error.hpp"
#include "core/Result.hpp"
#include "media/DecoderTeardownQueue.hpp"
#include "media/MediaDecoder.hpp"
#include "media/MediaInfo.hpp"

namespace palmier::media {
namespace {

using namespace std::chrono_literals;

/// The bound Requirement 14.8 puts on a single stop/seek operation. Used as the
/// destructor's own gate-wait ceiling so a stuck destructor reports rather than
/// hangs.
constexpr auto kRequirementBound = 2000ms;

/// Ceiling for waiting on a drain that is expected to complete promptly. Wildly
/// generous relative to the work involved (microseconds of simulated close per
/// decoder); it exists only so a deadlock surfaces as a failed assertion.
constexpr auto kDrainCeiling = 60s;

// ---------------------------------------------------------------------------
// Teardown ledger + the slow-destructor decoder double
// ---------------------------------------------------------------------------

/// Shared record of what the teardown thread did. Destructors are the only
/// writers of `destroyedIds`.
class TeardownLedger {
public:
    /// Called from a retired object's destructor: wait for the gate (bounded),
    /// then simulate the slow close, then record the destruction.
    void recordDestruction(int id, std::chrono::microseconds closeCost) {
        bool releasedByGate = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            releasedByGate = gate_.wait_for(lock, kRequirementBound, [this] { return open_; });
        }
        if (!releasedByGate) timedOutWaitingForGate_.fetch_add(1, std::memory_order_relaxed);

        // The deliberate, controlled slow close — this stands in for FFmpeg's
        // avcodec_free_context / avformat_close_input cost. It is the subject of
        // the property, never the basis of an assertion.
        if (closeCost.count() > 0) std::this_thread::sleep_for(closeCost);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            destroyedIds_.push_back(id);
        }
        destroyed_.fetch_add(1, std::memory_order_release);
    }

    void openGate() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            open_ = true;
        }
        gate_.notify_all();
    }

    [[nodiscard]] std::size_t destroyedCount() const {
        return destroyed_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t gateTimeouts() const {
        return timedOutWaitingForGate_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::vector<int> destroyedIdsSorted() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<int> ids = destroyedIds_;
        std::sort(ids.begin(), ids.end());
        return ids;
    }

private:
    mutable std::mutex       mutex_;
    std::condition_variable  gate_{};
    bool                     open_{false};
    std::vector<int>         destroyedIds_{};
    std::atomic<std::size_t> destroyed_{0};
    std::atomic<std::size_t> timedOutWaitingForGate_{0};
};

/// The decoder double: a resource whose destructor blocks exactly the way a
/// MediaDecoder closing an FFmpeg context does.
class SlowClosingDecoder {
public:
    SlowClosingDecoder(TeardownLedger& ledger, int id, std::chrono::microseconds closeCost)
        : ledger_(ledger), id_(id), closeCost_(closeCost) {}

    SlowClosingDecoder(const SlowClosingDecoder&)            = delete;
    SlowClosingDecoder& operator=(const SlowClosingDecoder&) = delete;

    ~SlowClosingDecoder() { ledger_.recordDestruction(id_, closeCost_); }

private:
    TeardownLedger&           ledger_;
    int                       id_;
    std::chrono::microseconds closeCost_;
};

} // namespace

// ---------------------------------------------------------------------------
// Property 75
// ---------------------------------------------------------------------------

// Feature: end-to-end-editor-integration, Property 75: Decoder teardown never
// deadlocks or stalls — for any sequence of retirements in any arrival pattern,
// each hand-over completes without waiting for a destructor, the whole sequence
// completes with the teardown queue draining to empty, and every retired decoder
// is destroyed exactly once.
//
// **Validates: Requirements 14.8**
RC_GTEST_PROP(DecoderTeardownProperty,
              Property75TeardownNeverDeadlocksOrStalls,
              ()) {
    // --- generators ---------------------------------------------------------
    // Retirement count: the requirement's sequence length is 100 stop/seek
    // operations, so the generated range brackets it (a stop or a seek retires
    // at most one decoder per active asset, 1-8 assets).
    const int retirements = *rc::gen::inRange(1, 101);

    // Arrival pattern: a burst (every retirement back to back, the stop-playback
    // shape) versus spread out (yielding between retirements, the seek shape).
    const bool burst = *rc::gen::arbitrary<bool>();

    // Retirement from several threads at once — playback, audio and the UI
    // thread can all retire a decoder concurrently.
    const int producers = *rc::gen::inRange(1, 5);

    // How slow the fake close is. The property must hold for any value here,
    // which is the point: the caller must not pay it.
    const int closeCostUs = *rc::gen::inRange(0, 401);

    // Whether the test drains explicitly before destroying the queue, or leaves
    // the outstanding work to the queue's own destructor.
    const bool drainBeforeDestroy = *rc::gen::arbitrary<bool>();

    // --- act ----------------------------------------------------------------
    TeardownLedger ledger;
    auto           queue = std::make_unique<DecoderTeardownQueue>();

    std::atomic<int>         nextId{0};
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(producers));

    for (int t = 0; t < producers; ++t) {
        threads.emplace_back([&, t] {
            for (int i = t; i < retirements; i += producers) {
                const int id = nextId.fetch_add(1, std::memory_order_relaxed);
                queue->retire(std::make_unique<SlowClosingDecoder>(
                    ledger, id, std::chrono::microseconds{closeCostUs}));
                if (!burst) std::this_thread::yield();
            }
        });
    }
    for (auto& thread : threads) thread.join();

    // --- assert: the hand-over never waited on a destructor -----------------
    // The gate is still closed, so no destructor can have completed. Had
    // retire() destroyed the object on the calling thread, reaching this line
    // would have required at least one completed destruction.
    RC_ASSERT(ledger.destroyedCount() == 0u);
    RC_ASSERT(queue->acceptedCount() == static_cast<std::uint64_t>(retirements));
    RC_ASSERT(queue->pending() == static_cast<std::size_t>(retirements));
    RC_ASSERT(queue->retiredCount() == 0u);

    // --- assert: the queue drains to empty, exactly once per decoder --------
    ledger.openGate();

    if (drainBeforeDestroy) {
        RC_ASSERT(queue->drainFor(kDrainCeiling));
        RC_ASSERT(queue->pending() == 0u);
        RC_ASSERT(queue->retiredCount() == static_cast<std::uint64_t>(retirements));
    }

    // --- assert: destroying the queue terminates and loses nothing ----------
    queue.reset();

    const std::vector<int> destroyed = ledger.destroyedIdsSorted();
    RC_ASSERT(destroyed.size() == static_cast<std::size_t>(retirements));
    for (int i = 0; i < retirements; ++i) {
        RC_ASSERT(destroyed[static_cast<std::size_t>(i)] == i); // exactly once, none lost
    }
    RC_ASSERT(ledger.gateTimeouts() == 0u);
}

// ---------------------------------------------------------------------------
// Unit tests — the real MediaDecoder overload and the queue's own contract
// ---------------------------------------------------------------------------

namespace {

/// A decode backend whose *destructor* blocks, which is where an FFmpeg
/// decoder's close cost lives. This is MediaDecoder's existing IDecodeBackend
/// seam; no new seam was introduced for these tests.
class SlowClosingBackend final : public IDecodeBackend {
public:
    SlowClosingBackend(TeardownLedger& ledger, int id, std::chrono::microseconds closeCost)
        : ledger_(ledger), id_(id), closeCost_(closeCost) {
        MediaStreamInfo video;
        video.index      = 0;
        video.type       = MediaStreamType::Video;
        video.codec      = MediaCodecId::H264;
        video.codecName  = "mock";
        video.resolution = Resolution{16, 16};
        info_.streams.push_back(video);
    }

    ~SlowClosingBackend() override { ledger_.recordDestruction(id_, closeCost_); }

    [[nodiscard]] const MediaInfo& info() const override { return info_; }

    [[nodiscard]] Result<BackendFrame> decode(bool) override { return BackendFrame::eos(); }

    [[nodiscard]] Result<void> seek(Duration) override { return ok(); }

private:
    TeardownLedger&           ledger_;
    int                       id_;
    std::chrono::microseconds closeCost_;
    MediaInfo                 info_{};
};

std::unique_ptr<MediaDecoder> openSlowDecoder(TeardownLedger& ledger, int id) {
    DecodeBackendFactory factory =
        [&ledger, id](const std::filesystem::path&, const DecodePrefs&)
            -> Result<std::unique_ptr<IDecodeBackend>> {
        return std::unique_ptr<IDecodeBackend>(
            std::make_unique<SlowClosingBackend>(ledger, id, std::chrono::microseconds{200}));
    };
    auto opened = MediaDecoder::open("clip.mp4", DecodePrefs{}, factory);
    if (!opened.isOk()) return nullptr;
    return std::make_unique<MediaDecoder>(std::move(opened).value());
}

} // namespace

TEST(DecoderTeardownQueue, RetiringRealDecodersDoesNotWaitForTheirDestructors) {
    TeardownLedger       ledger;
    DecoderTeardownQueue queue;

    constexpr int kDecoders = 4;
    for (int id = 0; id < kDecoders; ++id) {
        auto decoder = openSlowDecoder(ledger, id);
        ASSERT_NE(decoder, nullptr);
        queue.retire(std::move(decoder)); // the MediaDecoder overload
    }

    // The gate is closed, so nothing can have been destroyed: every retire()
    // returned without running an FFmpeg-style close.
    EXPECT_EQ(ledger.destroyedCount(), 0u);
    EXPECT_EQ(queue.pending(), static_cast<std::size_t>(kDecoders));

    ledger.openGate();
    ASSERT_TRUE(queue.drainFor(kDrainCeiling));

    EXPECT_EQ(queue.pending(), 0u);
    EXPECT_EQ(queue.retiredCount(), static_cast<std::uint64_t>(kDecoders));
    EXPECT_EQ(ledger.destroyedCount(), static_cast<std::size_t>(kDecoders));
    EXPECT_EQ(ledger.gateTimeouts(), 0u);
}

TEST(DecoderTeardownQueue, DestructorDestroysEverythingStillQueuedAndJoins) {
    TeardownLedger ledger;
    ledger.openGate(); // no gating here: the point is the join, not the ordering

    constexpr int kDecoders = 16;
    {
        DecoderTeardownQueue queue;
        for (int id = 0; id < kDecoders; ++id) {
            queue.retire(std::make_unique<SlowClosingDecoder>(ledger, id,
                                                             std::chrono::microseconds{100}));
        }
        // No drain: the destructor must finish the outstanding work.
    }

    EXPECT_EQ(ledger.destroyedCount(), static_cast<std::size_t>(kDecoders));
    EXPECT_EQ(ledger.destroyedIdsSorted().size(), static_cast<std::size_t>(kDecoders));
}

TEST(DecoderTeardownQueue, RetiringNullIsIgnoredAndDrainOfAnEmptyQueueReturns) {
    DecoderTeardownQueue queue;

    queue.retire(std::unique_ptr<MediaDecoder>{});
    queue.retire(std::unique_ptr<SlowClosingDecoder>{});

    EXPECT_EQ(queue.acceptedCount(), 0u);
    EXPECT_EQ(queue.pending(), 0u);
    EXPECT_TRUE(queue.running());
    queue.drain(); // returns immediately on an empty queue
    EXPECT_EQ(queue.retiredCount(), 0u);
}

TEST(DecoderTeardownQueue, ShutdownIsIdempotentAndLaterRetirementsAreStillDestroyed) {
    TeardownLedger ledger;
    ledger.openGate();

    DecoderTeardownQueue queue;
    queue.retire(std::make_unique<SlowClosingDecoder>(ledger, 0, std::chrono::microseconds{0}));
    queue.shutdown();
    EXPECT_FALSE(queue.running());
    queue.shutdown(); // idempotent

    // The worker is gone; the object is destroyed inline rather than leaked.
    queue.retire(std::make_unique<SlowClosingDecoder>(ledger, 1, std::chrono::microseconds{0}));
    EXPECT_EQ(ledger.destroyedCount(), 2u);
    EXPECT_EQ(queue.pending(), 0u);
    EXPECT_EQ(queue.retiredCount(), 2u);
}

} // namespace palmier::media
