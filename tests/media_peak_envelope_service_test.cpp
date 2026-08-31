// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/media_peak_envelope_service_test.cpp — tests for off-thread envelope
// production (monitoring-and-grading Requirement 2.2, 2.5, 2.6, 2.7).
//
// The service's contract is a negative one, so the tests are built to catch its
// violation: `lookup()` must NEVER block and never decode on the calling thread.
// A repaint calls it once per visible audio clip, many times a second, and must
// proceed immediately whatever the decoder is doing.
//
// That makes deduplication load-bearing rather than an optimisation. Without it a
// single two-second decode would be queued dozens of times over, and twenty clips
// sharing one interview would each queue their own copy of the same work. Both
// suppressions — an asset already in flight, and an asset already known to have
// failed — are asserted as NUMBERS, so "not retried on every repaint" is measured
// instead of assumed.
//
// Where a test needs the worker parked, the gate genuinely BLOCKS (with a bounded
// wait, so a mistake fails the test rather than hanging the suite). A gate that
// only signals would let the worker run to completion and turn every "while in
// flight" assertion into a coin toss — the exact defect recorded as CI incident 1
// of this spec.
//
// No FFmpeg, no media file, no device, no sleeping on a wall clock: the decoder is
// a synthetic backend and the worker is observed with drainFor().

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/Duration.hpp"
#include "core/Error.hpp"
#include "core/Result.hpp"
#include "core/Uuid.hpp"
#include "media/MediaDecoder.hpp"
#include "media/MediaInfo.hpp"
#include "media/PeakEnvelope.hpp"
#include "media/PeakEnvelopeService.hpp"

namespace palmier::media {
namespace {

using namespace std::chrono_literals;

constexpr int         kRate = 48'000;
constexpr std::size_t kFramesPerBucket = 480;  // 10 ms at 48 kHz — the default bucket
constexpr auto        kDrainBudget = 30'000ms;

/// What the synthetic source claims and produces.
struct SourceSpec {
    bool hasAudioStream = true;
    bool failDecode = false;
    /// Buckets' worth of audio to emit.
    std::size_t buckets = 3;
};

class SyntheticBackend final : public IDecodeBackend {
public:
    explicit SyntheticBackend(SourceSpec spec) : spec_(spec) {
        MediaStreamInfo video;
        video.index = 0;
        video.type = MediaStreamType::Video;
        video.codec = MediaCodecId::H264;
        video.codecName = "h264";
        video.resolution = Resolution{64, 64};
        info_.streams.push_back(video);

        if (spec_.hasAudioStream) {
            MediaStreamInfo audio;
            audio.index = 1;
            audio.type = MediaStreamType::Audio;
            audio.codec = MediaCodecId::AAC;
            audio.codecName = "aac";
            audio.sampleRate = kRate;
            audio.channels = 1;
            info_.streams.push_back(audio);
        }
    }

    [[nodiscard]] const MediaInfo& info() const override { return info_; }
    [[nodiscard]] Result<BackendFrame> decode(bool /*hw*/) override {
        return BackendFrame::eos();
    }
    [[nodiscard]] Result<void> seek(Duration /*ts*/) override { return ok(); }

    [[nodiscard]] Result<BackendAudioFrame> decodeAudio(int /*streamIndex*/) override {
        if (spec_.failDecode) {
            return err<BackendAudioFrame>(
                makeError(ErrorCode::Io, "synthetic audio decode failure"));
        }
        if (emittedBuckets_ >= spec_.buckets) return BackendAudioFrame::eos();

        BackendAudioFrame out;
        out.buffer = AudioBuffer::interleaved(kRate, 1,
                                              std::vector<float>(kFramesPerBucket, 0.8f));
        out.timestamp = frameToSourceTime(
            static_cast<std::int64_t>(emittedBuckets_ * kFramesPerBucket), kRate);
        ++emittedBuckets_;
        return out;
    }

    [[nodiscard]] Result<void> seekAudio(Duration /*ts*/, int /*idx*/) override { return ok(); }

private:
    SourceSpec  spec_;
    MediaInfo   info_{};
    std::size_t emittedBuckets_ = 0;
};

/// A factory that counts how many decoders it built, so "decoded once" is checked
/// at the decoder rather than only at the service's own counters.
class CountingFactory {
public:
    explicit CountingFactory(SourceSpec spec) : spec_(spec) {}

    [[nodiscard]] DecodeBackendFactory get() {
        return [this](const std::filesystem::path&,
                      const DecodePrefs&) -> Result<std::unique_ptr<IDecodeBackend>> {
            opens_.fetch_add(1);
            if (gate_ != nullptr) {
                // Genuinely BLOCK, bounded: this is what keeps the worker parked
                // while the test makes its "while in flight" assertions.
                (void)gate_->wait_for(kDrainBudget);
            }
            return std::unique_ptr<IDecodeBackend>(new SyntheticBackend(spec_));
        };
    }

    void gateOn(std::shared_future<void> gate) { gate_ = std::make_unique<std::shared_future<void>>(std::move(gate)); }
    [[nodiscard]] int opens() const { return opens_.load(); }

private:
    SourceSpec                                spec_;
    std::atomic<int>                          opens_{0};
    std::unique_ptr<std::shared_future<void>> gate_{};
};

[[nodiscard]] PeakEnvelopeServiceOptions options() {
    PeakEnvelopeServiceOptions opts;
    opts.bucketDuration = Duration::fromMilliseconds(10);
    return opts;
}

// ---------------------------------------------------------------------------
// The happy path
// ---------------------------------------------------------------------------

TEST(PeakEnvelopeService, ALookupMissesWithoutBlockingAndTheAnswerArrivesOffThread) {
    CountingFactory     factory(SourceSpec{});
    PeakEnvelopeService service(DecodePrefs{}, factory.get(), options());
    const Uuid          asset = Uuid::generateV4();

    // Requirement 2.2: the first lookup returns immediately, pending, having
    // decoded nothing on this thread.
    const EnvelopeLookup first = service.lookup(asset, "a.mov");
    EXPECT_TRUE(first.pending);
    EXPECT_FALSE(first.failed);
    EXPECT_EQ(first.envelope, nullptr);

    ASSERT_TRUE(service.drainFor(kDrainBudget));

    const EnvelopeLookup ready = service.lookup(asset, "a.mov");
    EXPECT_FALSE(ready.pending);
    EXPECT_FALSE(ready.failed);
    ASSERT_NE(ready.envelope, nullptr);
    EXPECT_EQ(ready.envelope->buckets.size(), 3u);
    EXPECT_FLOAT_EQ(ready.envelope->buckets[0].max, 0.8f);

    EXPECT_EQ(service.stats().scheduled, 1u);
    EXPECT_EQ(service.stats().completed, 1u);
    EXPECT_EQ(factory.opens(), 1);
}

TEST(PeakEnvelopeService, TheReadyCallbackNamesTheAssetWhoseAnswerArrived) {
    CountingFactory     factory(SourceSpec{});
    PeakEnvelopeService service(DecodePrefs{}, factory.get(), options());

    std::mutex        seenMutex;
    std::vector<Uuid> seen;
    service.setReadyCallback([&seenMutex, &seen](const Uuid& id) {
        const std::lock_guard<std::mutex> lock(seenMutex);
        seen.push_back(id);
    });

    const Uuid asset = Uuid::generateV4();
    (void)service.lookup(asset, "a.mov");
    ASSERT_TRUE(service.drainFor(kDrainBudget));
    // Drain returns when the job is done; the callback fires just after the
    // bookkeeping, so shutting down first makes the observation deterministic.
    service.shutdown();

    const std::lock_guard<std::mutex> lock(seenMutex);
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0], asset);
}

// ---------------------------------------------------------------------------
// Deduplication (Requirement 2.5's practical half)
// ---------------------------------------------------------------------------

TEST(PeakEnvelopeService, ManyClipsSharingOneAssetScheduleTheWorkExactlyOnce) {
    // The worker is parked in the factory, so every lookup below genuinely happens
    // WHILE the work is in flight — which is the only state in which duplicate
    // suppression can be observed at all.
    std::promise<void>       release;
    std::shared_future<void> gate(release.get_future());
    CountingFactory          factory(SourceSpec{});
    factory.gateOn(gate);

    PeakEnvelopeService service(DecodePrefs{}, factory.get(), options());
    const Uuid          asset = Uuid::generateV4();

    // Twenty clips referencing one asset, repainting.
    for (int clip = 0; clip < 20; ++clip) {
        const EnvelopeLookup pending = service.lookup(asset, "shared.mov");
        EXPECT_TRUE(pending.pending) << "lookup " << clip << " must not block";
    }

    EXPECT_EQ(service.stats().scheduled, 1u) << "one asset, one job";
    EXPECT_EQ(service.stats().duplicatesSuppressed, 19u);

    release.set_value();
    ASSERT_TRUE(service.drainFor(kDrainBudget));

    EXPECT_EQ(factory.opens(), 1) << "the decoder was opened once, not twenty times";
    EXPECT_EQ(service.cachedAssetCount(), 1u);
}

TEST(PeakEnvelopeService, ARepaintStormAfterCompletionCostsNoFurtherWork) {
    CountingFactory     factory(SourceSpec{});
    PeakEnvelopeService service(DecodePrefs{}, factory.get(), options());
    const Uuid          asset = Uuid::generateV4();

    (void)service.lookup(asset, "a.mov");
    ASSERT_TRUE(service.drainFor(kDrainBudget));

    for (int repaint = 0; repaint < 100; ++repaint) {
        const EnvelopeLookup hit = service.lookup(asset, "a.mov");
        ASSERT_NE(hit.envelope, nullptr);
    }
    EXPECT_EQ(service.stats().scheduled, 1u);
    EXPECT_EQ(factory.opens(), 1);
    EXPECT_EQ(service.outstandingJobs(), 0u);
}

// ---------------------------------------------------------------------------
// Silent, not failed (Requirement 2.6)
// ---------------------------------------------------------------------------

TEST(PeakEnvelopeService, AnAssetWithNoAudioIsCachedAsSilentAndNeverRecomputed) {
    SourceSpec spec;
    spec.hasAudioStream = false;
    CountingFactory     factory(spec);
    PeakEnvelopeService service(DecodePrefs{}, factory.get(), options());
    const Uuid          asset = Uuid::generateV4();

    (void)service.lookup(asset, "silent.mov");
    ASSERT_TRUE(service.drainFor(kDrainBudget));

    for (int repaint = 0; repaint < 25; ++repaint) {
        const EnvelopeLookup answer = service.lookup(asset, "silent.mov");
        EXPECT_FALSE(answer.pending);
        EXPECT_FALSE(answer.failed) << "no soundtrack is not a failure";
        EXPECT_TRUE(answer.isSilent());
        EXPECT_TRUE(answer.failure.empty());
    }

    EXPECT_EQ(service.stats().silent, 1u);
    EXPECT_EQ(service.stats().failed, 0u);
    EXPECT_EQ(service.stats().scheduled, 1u) << "silence is an answer, so it is not re-asked";
    EXPECT_EQ(factory.opens(), 1);
}

// ---------------------------------------------------------------------------
// Failure is remembered, not retried (Requirement 2.7)
// ---------------------------------------------------------------------------

TEST(PeakEnvelopeService, AFailingAssetIsReportedOnceAndNeverRetriedPerRepaint) {
    SourceSpec spec;
    spec.failDecode = true;
    CountingFactory     factory(spec);
    PeakEnvelopeService service(DecodePrefs{}, factory.get(), options());
    const Uuid          asset = Uuid::generateV4();

    (void)service.lookup(asset, "broken.mov");
    ASSERT_TRUE(service.drainFor(kDrainBudget));

    for (int repaint = 0; repaint < 50; ++repaint) {
        const EnvelopeLookup answer = service.lookup(asset, "broken.mov");
        EXPECT_FALSE(answer.pending);
        EXPECT_TRUE(answer.failed);
        EXPECT_FALSE(answer.failure.empty()) << "the reason is kept for the single report";
        EXPECT_EQ(answer.envelope, nullptr);
    }

    EXPECT_EQ(service.stats().failed, 1u);
    EXPECT_EQ(service.stats().scheduled, 1u) << "50 repaints, still one attempt";
    EXPECT_EQ(service.stats().failuresNotRetried, 50u);
    EXPECT_EQ(factory.opens(), 1) << "the decoder was never reopened";
}

TEST(PeakEnvelopeService, ForgettingAnAssetAllowsARecomputeAfterAReimport) {
    SourceSpec spec;
    spec.failDecode = true;
    CountingFactory     factory(spec);
    PeakEnvelopeService service(DecodePrefs{}, factory.get(), options());
    const Uuid          asset = Uuid::generateV4();

    (void)service.lookup(asset, "broken.mov");
    ASSERT_TRUE(service.drainFor(kDrainBudget));
    ASSERT_TRUE(service.lookup(asset, "broken.mov").failed);

    EXPECT_TRUE(service.forget(asset));
    EXPECT_FALSE(service.forget(asset)) << "forgetting twice reports nothing was there";

    // Now it is unknown again, so a lookup schedules real work.
    EXPECT_TRUE(service.lookup(asset, "broken.mov").pending);
    ASSERT_TRUE(service.drainFor(kDrainBudget));
    EXPECT_EQ(service.stats().scheduled, 2u);
    EXPECT_EQ(factory.opens(), 2);
}

// ---------------------------------------------------------------------------
// peek, drain and shutdown
// ---------------------------------------------------------------------------

TEST(PeakEnvelopeService, PeekNeverSchedulesAnything) {
    CountingFactory     factory(SourceSpec{});
    PeakEnvelopeService service(DecodePrefs{}, factory.get(), options());
    const Uuid          asset = Uuid::generateV4();

    const EnvelopeLookup unknown = service.peek(asset);
    EXPECT_FALSE(unknown.pending) << "nobody is computing it, so it is unknown, not pending";
    EXPECT_EQ(unknown.envelope, nullptr);

    EXPECT_EQ(service.stats().scheduled, 0u);
    EXPECT_EQ(service.outstandingJobs(), 0u);
    EXPECT_EQ(factory.opens(), 0);

    // After a real lookup completes, peek sees the answer.
    (void)service.lookup(asset, "a.mov");
    ASSERT_TRUE(service.drainFor(kDrainBudget));
    ASSERT_NE(service.peek(asset).envelope, nullptr);
    EXPECT_EQ(service.stats().scheduled, 1u);
}

TEST(PeakEnvelopeService, PeekReportsPendingWhileWorkIsGenuinelyInFlight) {
    std::promise<void>       release;
    std::shared_future<void> gate(release.get_future());
    CountingFactory          factory(SourceSpec{});
    factory.gateOn(gate);

    PeakEnvelopeService service(DecodePrefs{}, factory.get(), options());
    const Uuid          asset = Uuid::generateV4();

    (void)service.lookup(asset, "a.mov");
    EXPECT_TRUE(service.peek(asset).pending);
    EXPECT_GE(service.outstandingJobs(), 1u);

    release.set_value();
    ASSERT_TRUE(service.drainFor(kDrainBudget));
    EXPECT_FALSE(service.peek(asset).pending);
}

TEST(PeakEnvelopeService, DrainingAnIdleServiceSucceedsImmediately) {
    CountingFactory     factory(SourceSpec{});
    PeakEnvelopeService service(DecodePrefs{}, factory.get(), options());
    EXPECT_TRUE(service.drainFor(kDrainBudget));
    EXPECT_EQ(service.outstandingJobs(), 0u);
}

TEST(PeakEnvelopeService, ShutdownIsIdempotentAndDoesNotHang) {
    CountingFactory     factory(SourceSpec{});
    PeakEnvelopeService service(DecodePrefs{}, factory.get(), options());
    (void)service.lookup(Uuid::generateV4(), "a.mov");

    service.shutdown();
    service.shutdown();  // must not hang, must not throw
    EXPECT_EQ(service.workerCount(), 0u);

    // A late repaint after shutdown draws nothing rather than waiting on a dead
    // worker or queueing work that will never run.
    const EnvelopeLookup late = service.lookup(Uuid::generateV4(), "b.mov");
    EXPECT_TRUE(late.pending);
    EXPECT_EQ(late.envelope, nullptr);
    EXPECT_EQ(service.outstandingJobs(), 0u);
}

TEST(PeakEnvelopeService, TheCacheBoundIsHonouredSoManyAssetsDoNotGrowWithoutLimit) {
    PeakEnvelopeServiceOptions opts = options();
    opts.cacheCapacity = 2;
    CountingFactory     factory(SourceSpec{});
    PeakEnvelopeService service(DecodePrefs{}, factory.get(), opts);

    for (int i = 0; i < 5; ++i) {
        (void)service.lookup(Uuid::generateV4(), "a.mov");
        ASSERT_TRUE(service.drainFor(kDrainBudget));
    }

    EXPECT_EQ(service.cachedAssetCount(), 2u);
    EXPECT_GE(service.cacheStats().evictions, 3u);
}

} // namespace
} // namespace palmier::media
