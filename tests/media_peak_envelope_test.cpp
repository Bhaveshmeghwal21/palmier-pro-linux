// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/media_peak_envelope_test.cpp — unit tests for the peak-envelope
// reduction and the bounded LRU envelope cache (monitoring-and-grading
// Requirement 2.1, 2.3, 2.5, 2.6, 2.7).
//
// Two things have to be exactly right for a timeline waveform to be usable, and
// both are asserted here against hand-computable content:
//
//   1. **Bucket boundaries must not depend on how the audio arrived.** A decoder
//      hands over ragged blocks whose sizes are an artefact of the container. If
//      bucket boundaries were accumulated per buffer instead of derived from the
//      absolute frame counter, the same asset would draw differently depending on
//      block size — so the identical-stream-different-chunking case is asserted
//      directly rather than assumed from the implementation's shape.
//   2. **Presence in the cache must distinguish three outcomes, not two.** Not
//      computed, computed-and-silent (Requirement 2.6), and failed
//      (Requirement 2.7) are three different answers; collapsing silence into
//      absence would re-decode a silent asset on every repaint forever.
//
// No FFmpeg, no media file, no device, no Qt.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/Duration.hpp"
#include "core/Uuid.hpp"
#include "media/AudioGraph.hpp"
#include "media/PeakEnvelope.hpp"
#include "media/PeakEnvelopeCache.hpp"

namespace palmier::media {
namespace {

constexpr int kRate = 48'000;
/// 1 ms at 48 kHz is exactly 48 frames — chosen so every boundary below is an
/// integer and the expected values need no tolerance.
const Duration kBucket = Duration::fromMilliseconds(1);
constexpr std::size_t kFramesPerBucket = 48;

[[nodiscard]] AudioBuffer mono(std::vector<float> samples) {
    return AudioBuffer::interleaved(kRate, 1, std::move(samples));
}

/// `frames` frames of constant `value`, mono.
[[nodiscard]] AudioBuffer monoConstant(std::size_t frames, float value) {
    return mono(std::vector<float>(frames, value));
}

// ---------------------------------------------------------------------------
// frameToSourceTime
// ---------------------------------------------------------------------------

TEST(FrameToSourceTime, ConvertsExactlyAtWholeAndFractionalSeconds) {
    EXPECT_EQ(frameToSourceTime(0, kRate).ticks(), 0);
    EXPECT_EQ(frameToSourceTime(kRate, kRate), Duration::fromSeconds(1.0));
    EXPECT_EQ(frameToSourceTime(kRate / 2, kRate), Duration::fromMilliseconds(500));
    EXPECT_EQ(frameToSourceTime(48, kRate), Duration::fromMilliseconds(1));
}

TEST(FrameToSourceTime, DoesNotOverflowOnALongAsset) {
    // The naive form (frame * 1e9 / rate) overflows int64 here and wraps negative.
    // Ten hours at 48 kHz.
    const std::int64_t frames = static_cast<std::int64_t>(kRate) * 3600 * 10;
    const Duration     at = frameToSourceTime(frames, kRate);
    EXPECT_GT(at.ticks(), 0);
    EXPECT_NEAR(at.seconds(), 36'000.0, 1.0e-6);
}

TEST(FrameToSourceTime, ANonPositiveRateIsZeroRatherThanADivisionByZero) {
    EXPECT_EQ(frameToSourceTime(1'000, 0).ticks(), 0);
    EXPECT_EQ(frameToSourceTime(1'000, -1).ticks(), 0);
}

// ---------------------------------------------------------------------------
// PeakEnvelopeBuilder
// ---------------------------------------------------------------------------

TEST(PeakEnvelopeBuilder, ADegenerateConfigurationIsSilentRatherThanAnError) {
    // Requirement 2.6's spirit: an asset that declares no usable audio format is
    // silent, not broken, so the builder refuses to build rather than failing.
    for (const auto& [bucket, rate] : std::vector<std::pair<Duration, int>>{
             {Duration::zero(), kRate},
             {Duration::fromMilliseconds(-1), kRate},
             {kBucket, 0},
             {kBucket, -48'000}}) {
        PeakEnvelopeBuilder builder(bucket, rate);
        EXPECT_FALSE(builder.isValid());
        builder.add(monoConstant(1'000, 1.0f));
        EXPECT_EQ(builder.framesConsumed(), 0u);
        EXPECT_TRUE(builder.finish().empty());
    }
}

TEST(PeakEnvelopeBuilder, AConstantSignalFillsEveryBucketWithThatConstant) {
    PeakEnvelopeBuilder builder(kBucket, kRate);
    builder.add(monoConstant(kFramesPerBucket * 3, 0.5f));

    const PeakEnvelope envelope = builder.finish();
    ASSERT_EQ(envelope.buckets.size(), 3u);
    EXPECT_EQ(envelope.sampleRate, kRate);
    EXPECT_EQ(envelope.bucketDuration, kBucket);
    for (const EnvelopeBucket& bucket : envelope.buckets) {
        EXPECT_FLOAT_EQ(bucket.min, 0.5f);
        EXPECT_FLOAT_EQ(bucket.max, 0.5f);
        EXPECT_FLOAT_EQ(bucket.magnitude(), 0.5f);
        EXPECT_FALSE(bucket.isSilent());
    }
    EXPECT_EQ(envelope.duration(), Duration::fromMilliseconds(3));
}

TEST(PeakEnvelopeBuilder, BucketBoundariesLandExactlyWhereTheSourceTimeSaysTheyDo) {
    // Bucket 0 is +1.0, bucket 1 is -1.0, bucket 2 is 0.25. If the boundary were
    // off by even one frame, the neighbouring bucket would pick up the other
    // value and both mins/maxes below would widen.
    std::vector<float> samples;
    samples.insert(samples.end(), kFramesPerBucket, 1.0f);
    samples.insert(samples.end(), kFramesPerBucket, -1.0f);
    samples.insert(samples.end(), kFramesPerBucket, 0.25f);

    PeakEnvelopeBuilder builder(kBucket, kRate);
    builder.add(mono(samples));
    const PeakEnvelope envelope = builder.finish();

    ASSERT_EQ(envelope.buckets.size(), 3u);
    EXPECT_FLOAT_EQ(envelope.buckets[0].min, 1.0f);
    EXPECT_FLOAT_EQ(envelope.buckets[0].max, 1.0f);
    EXPECT_FLOAT_EQ(envelope.buckets[1].min, -1.0f);
    EXPECT_FLOAT_EQ(envelope.buckets[1].max, -1.0f);
    EXPECT_FLOAT_EQ(envelope.buckets[2].min, 0.25f);
    EXPECT_FLOAT_EQ(envelope.buckets[2].max, 0.25f);
}

TEST(PeakEnvelopeBuilder, TheSameStreamBucketsIdenticallyHoweverRaggedlyItArrives) {
    // The property that makes the drawn waveform a function of the asset rather
    // than of the decoder's block sizes.
    std::vector<float> samples(kFramesPerBucket * 5);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        // A deterministic, non-constant, sign-changing signal.
        samples[i] = static_cast<float>((static_cast<int>(i) % 17) - 8) / 8.0f;
    }

    PeakEnvelopeBuilder single(kBucket, kRate);
    single.add(mono(samples));

    PeakEnvelopeBuilder ragged(kBucket, kRate);
    const std::size_t   chunks[] = {1, 7, 3, 48, 2, 60, 11, 5};
    std::size_t         offset = 0;
    std::size_t         chunk = 0;
    while (offset < samples.size()) {
        const std::size_t take =
            std::min(chunks[chunk % std::size(chunks)], samples.size() - offset);
        ragged.add(mono(std::vector<float>(samples.begin() + static_cast<std::ptrdiff_t>(offset),
                                          samples.begin() +
                                              static_cast<std::ptrdiff_t>(offset + take))));
        offset += take;
        ++chunk;
    }

    ASSERT_EQ(single.framesConsumed(), ragged.framesConsumed());
    const PeakEnvelope a = single.finish();
    const PeakEnvelope b = ragged.finish();
    ASSERT_EQ(a.buckets.size(), b.buckets.size());
    for (std::size_t i = 0; i < a.buckets.size(); ++i) {
        EXPECT_FLOAT_EQ(a.buckets[i].min, b.buckets[i].min) << "bucket " << i;
        EXPECT_FLOAT_EQ(a.buckets[i].max, b.buckets[i].max) << "bucket " << i;
    }
}

TEST(PeakEnvelopeBuilder, ChannelsAreHulledTogetherSoNeitherSideIsHidden) {
    // Left pinned high, right pinned low. A builder that read only channel 0, or
    // that averaged, would report a narrower shape than the signal occupies.
    std::vector<float> samples;
    for (std::size_t frame = 0; frame < kFramesPerBucket; ++frame) {
        samples.push_back(0.9f);
        samples.push_back(-0.4f);
    }
    PeakEnvelopeBuilder builder(kBucket, kRate);
    builder.add(AudioBuffer::interleaved(kRate, 2, std::move(samples)));

    const PeakEnvelope envelope = builder.finish();
    ASSERT_EQ(envelope.buckets.size(), 1u);
    EXPECT_FLOAT_EQ(envelope.buckets[0].max, 0.9f);
    EXPECT_FLOAT_EQ(envelope.buckets[0].min, -0.4f);
    EXPECT_FLOAT_EQ(envelope.buckets[0].magnitude(), 0.9f);
}

TEST(PeakEnvelopeBuilder, APartialFinalBucketIsKeptRatherThanDiscarded) {
    PeakEnvelopeBuilder builder(kBucket, kRate);
    builder.add(monoConstant(kFramesPerBucket + 5, 1.0f));

    const PeakEnvelope envelope = builder.finish();
    ASSERT_EQ(envelope.buckets.size(), 2u) << "the 5 leftover frames still form a bucket";
    EXPECT_FLOAT_EQ(envelope.buckets[1].max, 1.0f);
}

TEST(PeakEnvelopeBuilder, EmptyAndChannellessBuffersAreNoOps) {
    PeakEnvelopeBuilder builder(kBucket, kRate);
    builder.add(AudioBuffer{});
    builder.add(AudioBuffer(kRate, 2, 0));
    EXPECT_EQ(builder.framesConsumed(), 0u);
    EXPECT_TRUE(builder.finish().empty());
}

TEST(PeakEnvelopeBuilder, FinishIsRepeatableSoAPartialResultCanBePublished) {
    PeakEnvelopeBuilder builder(kBucket, kRate);
    builder.add(monoConstant(kFramesPerBucket, 1.0f));
    const PeakEnvelope first = builder.finish();
    ASSERT_EQ(first.buckets.size(), 1u);

    builder.add(monoConstant(kFramesPerBucket, 0.5f));
    const PeakEnvelope second = builder.finish();
    ASSERT_EQ(second.buckets.size(), 2u);
    // The already-published bucket is unchanged by the continuation.
    EXPECT_FLOAT_EQ(second.buckets[0].max, 1.0f);
    EXPECT_FLOAT_EQ(second.buckets[1].max, 0.5f);
}

// ---------------------------------------------------------------------------
// PeakEnvelope::hullOver — the per-pixel-column drawing primitive
// ---------------------------------------------------------------------------

/// Three buckets: +1, -1, +0.25 (one millisecond each).
[[nodiscard]] PeakEnvelope threeBucketEnvelope() {
    std::vector<float> samples;
    samples.insert(samples.end(), kFramesPerBucket, 1.0f);
    samples.insert(samples.end(), kFramesPerBucket, -1.0f);
    samples.insert(samples.end(), kFramesPerBucket, 0.25f);
    PeakEnvelopeBuilder builder(kBucket, kRate);
    builder.add(mono(samples));
    return builder.finish();
}

TEST(PeakEnvelopeHull, ARangeSpanningSeveralBucketsReportsTheOuterHull) {
    const PeakEnvelope envelope = threeBucketEnvelope();

    const EnvelopeBucket all = envelope.hullOver(Duration::zero(), Duration::fromMilliseconds(3));
    EXPECT_FLOAT_EQ(all.max, 1.0f);
    EXPECT_FLOAT_EQ(all.min, -1.0f);

    // Only the second and third buckets: the +1 must NOT leak in.
    const EnvelopeBucket tail =
        envelope.hullOver(Duration::fromMilliseconds(1), Duration::fromMilliseconds(3));
    EXPECT_FLOAT_EQ(tail.max, 0.25f);
    EXPECT_FLOAT_EQ(tail.min, -1.0f);
}

TEST(PeakEnvelopeHull, ASingleBucketRangeIsExactlyThatBucket) {
    const PeakEnvelope envelope = threeBucketEnvelope();
    const EnvelopeBucket first =
        envelope.hullOver(Duration::zero(), Duration::fromMilliseconds(1));
    EXPECT_FLOAT_EQ(first.min, 1.0f);
    EXPECT_FLOAT_EQ(first.max, 1.0f);
}

TEST(PeakEnvelopeHull, ASubBucketRangeHoldsItsBucketRatherThanReportingSilence) {
    // Zooming in past the envelope's resolution must not produce gaps.
    const PeakEnvelope envelope = threeBucketEnvelope();
    const EnvelopeBucket held = envelope.hullOver(Duration::fromMicroseconds(1'200),
                                                  Duration::fromMicroseconds(1'300));
    EXPECT_FLOAT_EQ(held.min, -1.0f) << "1.2ms-1.3ms lies inside bucket 1";
    EXPECT_FLOAT_EQ(held.max, -1.0f);
}

TEST(PeakEnvelopeHull, AZeroWidthRangeReportsTheBucketItLandsIn) {
    const PeakEnvelope envelope = threeBucketEnvelope();
    const Duration     at = Duration::fromMicroseconds(2'500);
    const EnvelopeBucket point = envelope.hullOver(at, at);
    EXPECT_FLOAT_EQ(point.max, 0.25f);
}

TEST(PeakEnvelopeHull, ARangePastTheEndIsSilentSoATrimmedClipDrawsFlat) {
    const PeakEnvelope envelope = threeBucketEnvelope();
    const EnvelopeBucket beyond = envelope.hullOver(Duration::fromMilliseconds(5),
                                                    Duration::fromMilliseconds(9));
    EXPECT_TRUE(beyond.isSilent());
}

TEST(PeakEnvelopeHull, ARangeStraddlingTheEndIsClampedRatherThanReadingPastIt) {
    const PeakEnvelope envelope = threeBucketEnvelope();
    const EnvelopeBucket straddle =
        envelope.hullOver(Duration::fromMilliseconds(2), Duration::fromMilliseconds(99));
    EXPECT_FLOAT_EQ(straddle.max, 0.25f);
    EXPECT_FLOAT_EQ(straddle.min, 0.25f);
}

TEST(PeakEnvelopeHull, NegativeAndInvertedRangesAreHandledWithoutReadingOutOfBounds) {
    // A renderer computing source times from a partly-scrolled clip can hand over
    // a negative or reversed range; neither may index past the buckets.
    const PeakEnvelope envelope = threeBucketEnvelope();

    EXPECT_TRUE(envelope.hullOver(Duration::fromMilliseconds(-9),
                                  Duration::fromMilliseconds(-4)).isSilent());

    // Straddling zero clamps to the start rather than wrapping.
    const EnvelopeBucket fromBeforeZero =
        envelope.hullOver(Duration::fromMilliseconds(-5), Duration::fromMilliseconds(1));
    EXPECT_FLOAT_EQ(fromBeforeZero.max, 1.0f);

    // Inverted arguments describe the same range.
    const EnvelopeBucket inverted =
        envelope.hullOver(Duration::fromMilliseconds(3), Duration::fromMilliseconds(1));
    const EnvelopeBucket ordered =
        envelope.hullOver(Duration::fromMilliseconds(1), Duration::fromMilliseconds(3));
    EXPECT_FLOAT_EQ(inverted.min, ordered.min);
    EXPECT_FLOAT_EQ(inverted.max, ordered.max);

    // A fully negative inverted range is still silent, not an out-of-bounds read.
    EXPECT_TRUE(envelope.hullOver(Duration::fromMilliseconds(-1),
                                  Duration::fromMilliseconds(-9)).isSilent());
}

TEST(PeakEnvelopeHull, AnEmptyEnvelopeIsSilentEverywhere) {
    const PeakEnvelope empty;
    EXPECT_TRUE(empty.empty());
    EXPECT_EQ(empty.duration().ticks(), 0);
    EXPECT_TRUE(empty.hullOver(Duration::zero(), Duration::fromMilliseconds(1)).isSilent());
    EXPECT_TRUE(empty.bucketAt(Duration::zero()).isSilent());
}

TEST(PeakEnvelopeHull, BucketAtSelectsBySourceTimeAndClampsOutOfRange) {
    const PeakEnvelope envelope = threeBucketEnvelope();
    EXPECT_FLOAT_EQ(envelope.bucketAt(Duration::fromMicroseconds(500)).max, 1.0f);
    EXPECT_FLOAT_EQ(envelope.bucketAt(Duration::fromMicroseconds(1'500)).max, -1.0f);
    EXPECT_FLOAT_EQ(envelope.bucketAt(Duration::fromMicroseconds(2'500)).max, 0.25f);
    EXPECT_TRUE(envelope.bucketAt(Duration::fromMilliseconds(3)).isSilent());
    EXPECT_TRUE(envelope.bucketAt(Duration::fromMilliseconds(-1)).isSilent());
}

// ---------------------------------------------------------------------------
// PeakEnvelopeCache
// ---------------------------------------------------------------------------

TEST(PeakEnvelopeCache, AnUnknownAssetMissesSoTheCallerKnowsToScheduleTheWork) {
    PeakEnvelopeCache cache;
    const Uuid        asset = Uuid::generateV4();

    EXPECT_EQ(cache.find(asset), nullptr);
    EXPECT_EQ(cache.stats().misses, 1u);
    EXPECT_EQ(cache.stats().hits, 0u);
    EXPECT_FALSE(cache.contains(asset));
}

TEST(PeakEnvelopeCache, AStoredEnvelopeIsReturnedAndSharedByEveryLookup) {
    PeakEnvelopeCache cache;
    const Uuid        asset = Uuid::generateV4();
    cache.store(asset, threeBucketEnvelope());

    // Requirement 2.5: one envelope, reused by every clip referencing the asset —
    // modelled here as repeated lookups all seeing the same content.
    for (int lookup = 0; lookup < 3; ++lookup) {
        const EnvelopeCacheEntry* entry = cache.find(asset);
        ASSERT_NE(entry, nullptr);
        EXPECT_FALSE(entry->failed);
        ASSERT_EQ(entry->envelope.buckets.size(), 3u);
        EXPECT_FLOAT_EQ(entry->envelope.buckets[0].max, 1.0f);
    }
    EXPECT_EQ(cache.stats().hits, 3u);
    EXPECT_EQ(cache.size(), 1u);
}

TEST(PeakEnvelopeCache, AnAudiolessAssetCachesAsSilentAndIsNeverRecomputed) {
    // Requirement 2.6. The distinction that matters: a cached EMPTY envelope is an
    // answer, not an absence, so the caller never schedules the work again.
    PeakEnvelopeCache cache;
    const Uuid        asset = Uuid::generateV4();
    cache.store(asset, PeakEnvelope{});

    const EnvelopeCacheEntry* entry = cache.find(asset);
    ASSERT_NE(entry, nullptr) << "silence must be cached, not absent";
    EXPECT_TRUE(entry->isSilent());
    EXPECT_FALSE(entry->failed);
    EXPECT_TRUE(entry->envelope.empty());
    EXPECT_TRUE(entry->failure.empty());
}

TEST(PeakEnvelopeCache, AFailureIsRememberedWithItsReasonAndNotRetriedPerRepaint) {
    // Requirement 2.7. Every repaint looks the asset up; none of them may turn
    // into a fresh attempt, which is what the failureHits counter records.
    PeakEnvelopeCache cache;
    const Uuid        asset = Uuid::generateV4();
    cache.storeFailure(asset, "decoder refused the stream");

    for (int repaint = 0; repaint < 50; ++repaint) {
        const EnvelopeCacheEntry* entry = cache.find(asset);
        ASSERT_NE(entry, nullptr);
        EXPECT_TRUE(entry->failed);
        EXPECT_FALSE(entry->isSilent()) << "failed is not the same answer as silent";
        EXPECT_EQ(entry->failure, "decoder refused the stream");
    }
    EXPECT_EQ(cache.stats().failureHits, 50u);
    EXPECT_EQ(cache.stats().misses, 0u) << "not one repaint re-asked for the work";
}

TEST(PeakEnvelopeCache, AReimportCanReplaceAFailureWithARealEnvelope) {
    PeakEnvelopeCache cache;
    const Uuid        asset = Uuid::generateV4();
    cache.storeFailure(asset, "file was missing");
    ASSERT_TRUE(cache.find(asset)->failed);

    cache.store(asset, threeBucketEnvelope());
    const EnvelopeCacheEntry* entry = cache.find(asset);
    ASSERT_NE(entry, nullptr);
    EXPECT_FALSE(entry->failed);
    EXPECT_TRUE(entry->failure.empty());
    EXPECT_EQ(entry->envelope.buckets.size(), 3u);
    EXPECT_EQ(cache.size(), 1u) << "replaced, not duplicated";
}

TEST(PeakEnvelopeCache, TheCacheIsBoundedAndEvictsTheLeastRecentlyUsedEntry) {
    PeakEnvelopeCache cache(2);
    const Uuid        a = Uuid::generateV4();
    const Uuid        b = Uuid::generateV4();
    const Uuid        c = Uuid::generateV4();

    cache.store(a, threeBucketEnvelope());
    cache.store(b, threeBucketEnvelope());
    cache.store(c, threeBucketEnvelope());

    EXPECT_EQ(cache.size(), 2u);
    EXPECT_EQ(cache.capacity(), 2u);
    EXPECT_EQ(cache.stats().evictions, 1u);
    EXPECT_FALSE(cache.contains(a)) << "a was least recently used";
    EXPECT_TRUE(cache.contains(b));
    EXPECT_TRUE(cache.contains(c));
}

TEST(PeakEnvelopeCache, ALookupRefreshesRecencySoAnInUseAssetSurvivesEviction) {
    // The behaviour that makes the bound useful rather than merely present: the
    // asset being drawn every frame must not be the one evicted.
    PeakEnvelopeCache cache(2);
    const Uuid        a = Uuid::generateV4();
    const Uuid        b = Uuid::generateV4();
    const Uuid        c = Uuid::generateV4();

    cache.store(a, threeBucketEnvelope());
    cache.store(b, threeBucketEnvelope());
    ASSERT_NE(cache.find(a), nullptr);  // a is now the most recently used
    cache.store(c, threeBucketEnvelope());

    EXPECT_TRUE(cache.contains(a)) << "a was touched, so b must go instead";
    EXPECT_FALSE(cache.contains(b));
    EXPECT_TRUE(cache.contains(c));
}

TEST(PeakEnvelopeCache, RecencyOrderIsObservableSoEvictionOrderIsAssertableNotGuessed) {
    PeakEnvelopeCache cache(3);
    const Uuid        a = Uuid::generateV4();
    const Uuid        b = Uuid::generateV4();
    const Uuid        c = Uuid::generateV4();

    cache.store(a, PeakEnvelope{});
    cache.store(b, PeakEnvelope{});
    cache.store(c, PeakEnvelope{});
    ASSERT_EQ(cache.recencyOrder(), (std::vector<Uuid>{c, b, a}));

    ASSERT_NE(cache.find(a), nullptr);
    EXPECT_EQ(cache.recencyOrder(), (std::vector<Uuid>{a, c, b}));

    // peek() must NOT disturb the order, so diagnostics cannot change behaviour.
    ASSERT_NE(cache.peek(b), nullptr);
    EXPECT_EQ(cache.recencyOrder(), (std::vector<Uuid>{a, c, b}));
}

TEST(PeakEnvelopeCache, PeekNeitherDisturbsRecencyNorMovesTheCounters) {
    PeakEnvelopeCache cache;
    const Uuid        asset = Uuid::generateV4();
    cache.store(asset, threeBucketEnvelope());
    const EnvelopeCacheStats before = cache.stats();

    ASSERT_NE(cache.peek(asset), nullptr);
    EXPECT_EQ(cache.peek(Uuid::generateV4()), nullptr);

    const EnvelopeCacheStats after = cache.stats();
    EXPECT_EQ(before.hits, after.hits);
    EXPECT_EQ(before.misses, after.misses);
}

TEST(PeakEnvelopeCache, ForgetAndClearDropEntries) {
    PeakEnvelopeCache cache;
    const Uuid        a = Uuid::generateV4();
    const Uuid        b = Uuid::generateV4();
    cache.store(a, threeBucketEnvelope());
    cache.store(b, threeBucketEnvelope());

    EXPECT_TRUE(cache.forget(a));
    EXPECT_FALSE(cache.forget(a)) << "forgetting twice reports nothing was there";
    EXPECT_EQ(cache.size(), 1u);
    EXPECT_EQ(cache.recencyOrder(), (std::vector<Uuid>{b}));

    cache.clear();
    EXPECT_EQ(cache.size(), 0u);
    EXPECT_TRUE(cache.recencyOrder().empty());
}

TEST(PeakEnvelopeCache, AZeroCapacityIsRaisedToOneSoTheCacheNeverSilentlyCachesNothing) {
    PeakEnvelopeCache cache(0);
    EXPECT_EQ(cache.capacity(), 1u);

    const Uuid asset = Uuid::generateV4();
    cache.store(asset, threeBucketEnvelope());
    EXPECT_NE(cache.find(asset), nullptr);
}

} // namespace
} // namespace palmier::media
