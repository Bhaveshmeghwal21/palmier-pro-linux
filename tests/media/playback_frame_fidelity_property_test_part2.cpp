// SPDX-License-Identifier: GPL-3.0-or-later
//
// Part 2 of the playback_frame_fidelity_property_test cases, split out of playback_frame_fidelity_property_test.cpp.
//
// Suites here: DecodeWorkerPool, PlaybackFrameFidelityProperty.
// Compiled into the same target as its siblings, so every case keeps the name CTest
// already knew it by.

#include "playback_frame_fidelity_property_test_support.hpp"

namespace palmier::media {

using namespace palmier::media::
    testsupport_playbackframefidelitypropertytest;  // NOLINT(google-build-using-namespace)

// ---------------------------------------------------------------------------
// Property 20
// ---------------------------------------------------------------------------

// Feature: end-to-end-editor-integration, Property 20: Presented frames match
// the decoded source frames — for any source frame content and any single-clip
// timeline, the composited frame presented for a timeline position inside the
// clip matches, within 2 of 255 levels per channel, the frame the decoder-backed
// clip frame provider returns for that clip's corresponding source position.
//
// **Validates: Requirements 5.1**
RC_GTEST_PROP(PlaybackFrameFidelityProperty, Property20PresentedFramesMatchTheDecodedSourceFrames,
              ()) {
    // --- generators ---------------------------------------------------------
    // Distinct assets on the timeline. More than the decoder cache holds is the
    // interesting case, so the cache capacity is generated independently below.
    const int assetCount = *rc::gen::inRange(1, 5);

    // Source geometry. Kept small so 100+ cases stay quick; the 1920x1080 end of
    // the documented range is covered by a unit test below.
    const std::uint32_t width = static_cast<std::uint32_t>(*rc::gen::inRange(16, 65));
    const std::uint32_t height = static_cast<std::uint32_t>(*rc::gen::inRange(16, 49));

    // Decoder residency and prefetch depth: eviction pressure and worker
    // involvement are both part of the subject.
    const std::size_t cacheCapacity = static_cast<std::size_t>(*rc::gen::inRange(1, 5));
    const std::size_t prefetchDepth = static_cast<std::size_t>(*rc::gen::inRange(0, 4));

    // Clips on the timeline. With clipCount > assetCount, clips share an asset —
    // and therefore share one decoder cursor.
    const int clipCount = *rc::gen::inRange(1, 5);

    const auto pattern = *rc::gen::element(AccessPattern::Sequential, AccessPattern::ForwardSeek,
                                           AccessPattern::BackwardSeek, AccessPattern::Interleaved);

    // --- the synthetic library ----------------------------------------------
    std::vector<SyntheticAsset> assets;
    assets.reserve(static_cast<std::size_t>(assetCount));
    for (int a = 0; a < assetCount; ++a) {
        SyntheticAsset asset;
        // Uuid::generateV4() — never drawn byte-wise, so no nil/duplicate ids.
        asset.assetId = Uuid::generateV4();
        asset.path = "synthetic://asset-" + std::to_string(a) + "-" + asset.assetId.toString();
        asset.width = width;
        asset.height = height;
        asset.rate = rateFor(*rc::gen::inRange(0, 4));
        asset.frameCount = *rc::gen::inRange(6, 25);
        asset.salt = static_cast<std::uint32_t>(*rc::gen::inRange(0, 256));
        assets.push_back(asset);
    }

    // --- the timeline -------------------------------------------------------
    std::vector<PlacedClip> placed;
    placed.reserve(static_cast<std::size_t>(clipCount));
    Duration cursor = Duration::zero();
    for (int c = 0; c < clipCount; ++c) {
        const int assetIndex = c % assetCount;
        const SyntheticAsset& asset = assets[static_cast<std::size_t>(assetIndex)];
        const Duration step = asset.frameStep();

        // A non-zero sourceIn is the case the mapping most easily gets wrong.
        const int maxIn = asset.frameCount / 2;
        const int sourceInFrame = maxIn > 0 ? *rc::gen::inRange(0, maxIn + 1) : 0;
        const int available = asset.frameCount - sourceInFrame;
        const int lengthFrames = *rc::gen::inRange(1, available + 1);

        // Clips are laid end to end on one video track, so exactly one clip is
        // visible at any position and the composited output is that clip's frame.
        Clip clip;
        clip.id = Uuid::generateV4();
        clip.assetRef = MediaAssetRef{asset.assetId, asset.path};
        clip.timelineStart = cursor;
        clip.sourceIn = step * static_cast<std::int64_t>(sourceInFrame);
        clip.sourceOut = step * static_cast<std::int64_t>(sourceInFrame + lengthFrames);
        clip.opacity = 1.0;
        cursor = clip.timelineEnd();

        placed.push_back(PlacedClip{clip, assetIndex, sourceInFrame, lengthFrames, step});
    }

    const std::uint32_t canvasW = width;
    const std::uint32_t canvasH = height;
    const Project project = projectWith(placed, Resolution{canvasW, canvasH}, assets.front().rate);

    // --- the pipeline under test -------------------------------------------
    auto counters = std::make_shared<BackendCounters>();

    DecoderTeardownQueue teardown;
    ClipFrameProviderOptions options;
    options.decoderCacheCapacity = cacheCapacity;
    options.prefetchDepth = prefetchDepth;
    options.prefs = softwarePrefs();
    options.pool.workerCount = 2;  // the design's N = 2
    options.pool.clipQueueCapacity = 3;

    DecoderClipFrameProvider provider(teardown, syntheticFactory(assets, counters), options);

    gpu::GpuContext context = gpu::GpuContext::softwareFallback();
    gpu::Compositor compositor(context);
    compositor.setFrameProvider(provider.asProvider());

    const gpu::RenderTarget target{canvasW, canvasH, gpu::RgbaColor::transparent()};

    // --- the request sequence ----------------------------------------------
    // Each entry is (clip index, frame offset within the clip).
    std::vector<std::pair<std::size_t, int>> requests;
    for (std::size_t c = 0; c < placed.size(); ++c) {
        const int length = placed[c].lengthFrames;
        switch (pattern) {
            case AccessPattern::Sequential:
                for (int f = 0; f < length; ++f) requests.emplace_back(c, f);
                break;
            case AccessPattern::ForwardSeek:
                for (int f = 0; f < length; f += 2) requests.emplace_back(c, f);
                break;
            case AccessPattern::BackwardSeek:
                for (int f = length - 1; f >= 0; --f) requests.emplace_back(c, f);
                break;
            case AccessPattern::Interleaved:
                // Alternate ends, which forces a seek on nearly every request.
                for (int f = 0; f < length; ++f) {
                    requests.emplace_back(c, (f % 2 == 0) ? f / 2 : length - 1 - f / 2);
                }
                break;
        }
    }
    if (pattern == AccessPattern::Interleaved && placed.size() > 1) {
        // Interleave across clips too, so clips sharing one asset contend.
        std::vector<std::pair<std::size_t, int>> woven;
        for (int f = 0; f < 3; ++f) {
            for (std::size_t c = 0; c < placed.size(); ++c) {
                woven.emplace_back(c, f % placed[c].lengthFrames);
            }
        }
        requests.insert(requests.end(), woven.begin(), woven.end());
    }

    // --- act + assert -------------------------------------------------------
    for (const auto& [clipIndex, offset] : requests) {
        const PlacedClip& p = placed[clipIndex];
        const SyntheticAsset& asset = assets[static_cast<std::size_t>(p.assetIndex)];

        const Duration position = p.clip.timelineStart + p.step * static_cast<std::int64_t>(offset);
        RC_ASSERT(position >= p.clip.timelineStart);
        RC_ASSERT(position < p.clip.timelineEnd());

        auto rendered = compositor.renderAt(project, position, target);
        RC_ASSERT(rendered.isOk());

        // The oracle: the mapping the requirement states, computed here rather
        // than taken from the code under test.
        const int expectedIndex = p.sourceInFrames + offset;
        const std::vector<std::uint8_t> expected = syntheticPixels(asset, expectedIndex);

        RC_ASSERT(rendered.value().layerCount() == 1u);
        RC_ASSERT(maxChannelDifference(rendered.value(), expected, asset.width, asset.height) <=
                  kChannelTolerance);
    }

    // --- structural invariants ---------------------------------------------
    // The LRU never exceeds its capacity, and eviction pressure really occurred
    // when more assets were touched than the cache can hold.
    RC_ASSERT(provider.residentDecoderCount() <= cacheCapacity);
    const std::size_t distinctAssetsTouched =
        std::min<std::size_t>(static_cast<std::size_t>(assetCount), placed.size());
    if (distinctAssetsTouched > cacheCapacity) {
        RC_ASSERT(provider.stats().evictions > 0u);
    }
    RC_ASSERT(provider.stats().delivered == static_cast<std::uint64_t>(requests.size()));
    RC_ASSERT(provider.stats().failures == 0u);

    // Every decoder retired so far reached the teardown queue, and the queue
    // drains to empty — the eviction path never leaked a decoder.
    RC_ASSERT(provider.pool().drainFor(
        std::chrono::duration_cast<std::chrono::milliseconds>(kWaitCeiling)));
    RC_ASSERT(
        teardown.drainFor(std::chrono::duration_cast<std::chrono::milliseconds>(kWaitCeiling)));
    RC_ASSERT(teardown.acceptedCount() >= provider.stats().evictions);
}
TEST(DecodeWorkerPool, PrefetchedFramesSatisfyLaterRequestsAndTheQueueIsBounded) {
    Fixture fx(16, 16, /*frameCount=*/16);
    DecoderTeardownQueue teardown;

    DecodeWorkerPoolOptions poolOptions;
    poolOptions.workerCount = 2;
    poolOptions.clipQueueCapacity = 2;
    DecodeWorkerPool pool(teardown, poolOptions);
    EXPECT_EQ(pool.workerCount(), 2u);

    ASSERT_TRUE(
        pool.activateAsset(fx.asset.assetId, fx.asset.path, softwarePrefs(), fx.factory()).isOk());

    const ClipId clipId = Uuid::generateV4();
    const Duration step = fx.step();

    // Ask for more than the queue holds; the surplus is dropped, never blocking.
    pool.prefetch(fx.asset.assetId, clipId, Duration::zero(), step, /*frameCount=*/8);
    ASSERT_TRUE(pool.drainFor(std::chrono::duration_cast<std::chrono::milliseconds>(kWaitCeiling)));
    EXPECT_LE(pool.queuedFrames(clipId), 2u);

    // The queued frames are the right ones, in order.
    for (int index = 0; index < 2; ++index) {
        auto frame =
            pool.decodeFor(fx.asset.assetId, clipId, step * static_cast<std::int64_t>(index), step);
        ASSERT_TRUE(frame.isOk()) << frame.error().toString();
        EXPECT_TRUE(framesEqual(frame.value().image, syntheticPixels(fx.asset, index)));
    }
    EXPECT_GE(pool.stats().queueHits, 1u);

    // An unknown asset is reported, not decoded.
    auto unknown = pool.decodeFor(Uuid::generateV4(), clipId, Duration::zero(), step);
    ASSERT_TRUE(unknown.isError());
    EXPECT_EQ(unknown.error().code(), ErrorCode::NotFound);

    pool.retireAsset(fx.asset.assetId);
    EXPECT_EQ(pool.activeAssetCount(), 0u);
    ASSERT_TRUE(
        teardown.drainFor(std::chrono::duration_cast<std::chrono::milliseconds>(kWaitCeiling)));
}
TEST(DecodeWorkerPool, ToSourceFrameRejectsUnusableDecodedFrames) {
    EXPECT_TRUE(toSourceFrame(DecodedFrame::endOfStream()).isError());

    // A CPU frame whose buffer is shorter than its declared geometry.
    DecodedFrame short_ =
        DecodedFrame::cpu(Duration::zero(), gpu::FrameDesc{4, 4, gpu::FrameFormat::RGBA8},
                          std::vector<std::byte>(8, std::byte{0}));
    EXPECT_TRUE(toSourceFrame(short_).isError());

    // A well-formed CPU frame converts to an equally-sized source frame.
    DecodedFrame good =
        DecodedFrame::cpu(Duration::zero(), gpu::FrameDesc{4, 4, gpu::FrameFormat::RGBA8},
                          std::vector<std::byte>(4u * 4u * 4u, std::byte{0x7F}));
    auto converted = toSourceFrame(good);
    ASSERT_TRUE(converted.isOk());
    EXPECT_EQ(converted.value().width, 4u);
    EXPECT_EQ(converted.value().height, 4u);
    EXPECT_TRUE(converted.value().valid());
    EXPECT_EQ(converted.value().rgba[0], 0x7Fu);
}
// ---------------------------------------------------------------------------
// Frame accuracy across a keyframe-granular seek
// ---------------------------------------------------------------------------
//
// A backward seek reaches a keyframe, not the frame asked for. These tests pin the three
// behaviours that make a request nonetheless yield its own frame. Before they existed the
// pool returned whatever the seek landed on and labelled it with the requested position,
// so an export repeated frames wherever anything moved the shared cursor: measured at 31,
// 45, 52 repeats out of 672 across runs of ONE unchanged 22.4 s project, and zero
// afterwards.

TEST(DecodeWorkerPool, SeekIntoAKeyframeIntervalDeliversTheRequestedFrame) {
    Fixture fx(16, 16, /*frameCount=*/16);
    fx.asset.keyframeInterval = 12;  // frames 0 and 12 are keyframes; 7 is not
    DecoderTeardownQueue teardown;
    DecodeWorkerPool pool(teardown, DecodeWorkerPoolOptions{});
    ASSERT_TRUE(
        pool.activateAsset(fx.asset.assetId, fx.asset.path, softwarePrefs(), fx.factory()).isOk());

    const ClipId clipId = Uuid::generateV4();
    const Duration step = fx.step();
    const int wanted = 7;

    auto frame =
        pool.decodeFor(fx.asset.assetId, clipId, step * static_cast<std::int64_t>(wanted), step);
    ASSERT_TRUE(frame.isOk()) << frame.error().toString();

    // The frame asked for, not the keyframe the seek could reach.
    EXPECT_TRUE(framesEqual(frame.value().image, syntheticPixels(fx.asset, wanted)));
    EXPECT_FALSE(framesEqual(frame.value().image, syntheticPixels(fx.asset, 0)));

    // Tagged with where it is, which is what lets a later request be judged against it.
    EXPECT_EQ(frame.value().sourcePosition, step * static_cast<std::int64_t>(wanted));

    // Getting there meant decoding through the frames between the keyframe and the
    // target, and that work is reported.
    EXPECT_EQ(pool.stats().seeks, 1u);
    EXPECT_EQ(pool.stats().seekSkippedFrames, static_cast<std::uint64_t>(wanted));

    pool.retireAsset(fx.asset.assetId);
    ASSERT_TRUE(
        teardown.drainFor(std::chrono::duration_cast<std::chrono::milliseconds>(kWaitCeiling)));
}
TEST(DecodeWorkerPool, TwoClipsAlternatingOnOneAssetEachGetTheirOwnFrame) {
    // The picture-in-picture case: an inset over a clip that reads the same file. Both
    // readers share one decoder cursor, so every alternation is random access. Each must
    // still receive its own frame -- this is exactly where repeats survived after
    // prefetch was quietened.
    Fixture fx(16, 16, /*frameCount=*/24);
    fx.asset.keyframeInterval = 12;
    DecoderTeardownQueue teardown;
    DecodeWorkerPool pool(teardown, DecodeWorkerPoolOptions{});
    ASSERT_TRUE(
        pool.activateAsset(fx.asset.assetId, fx.asset.path, softwarePrefs(), fx.factory()).isOk());

    const ClipId below = Uuid::generateV4();
    const ClipId inset = Uuid::generateV4();
    const Duration step = fx.step();

    for (int i = 0; i < 4; ++i) {
        const int belowIndex = 2 + i;   // one run of the source
        const int insetIndex = 17 + i;  // a different part of the same source
        for (const auto& [clipId, index] : {std::pair<ClipId, int>{below, belowIndex},
                                            std::pair<ClipId, int>{inset, insetIndex}}) {
            auto frame = pool.decodeFor(fx.asset.assetId, clipId,
                                        step * static_cast<std::int64_t>(index), step);
            ASSERT_TRUE(frame.isOk()) << frame.error().toString();
            EXPECT_TRUE(framesEqual(frame.value().image, syntheticPixels(fx.asset, index)))
                << "clip asked for frame " << index << " and did not get it";
            EXPECT_EQ(frame.value().sourcePosition, step * static_cast<std::int64_t>(index));
        }
    }

    pool.retireAsset(fx.asset.assetId);
    ASSERT_TRUE(
        teardown.drainFor(std::chrono::duration_cast<std::chrono::milliseconds>(kWaitCeiling)));
}
TEST(DecodeWorkerPool, PrefetchDeclinesWorkThatWouldNeedASeek) {
    // Speculation must stay cheap. A seek costs a decode from the preceding keyframe, and
    // repositioning the cursor turns the render thread's next sequential request into
    // random access too, so both sides pay. A job that is not a continuation is dropped.
    Fixture fx(16, 16, /*frameCount=*/32);
    fx.asset.keyframeInterval = 16;
    DecoderTeardownQueue teardown;
    DecodeWorkerPoolOptions options;
    options.workerCount = 2;
    options.clipQueueCapacity = 4;
    DecodeWorkerPool pool(teardown, options);
    ASSERT_TRUE(
        pool.activateAsset(fx.asset.assetId, fx.asset.path, softwarePrefs(), fx.factory()).isOk());

    const ClipId clipId = Uuid::generateV4();
    const Duration step = fx.step();

    // Establish the cursor at the head of the stream: no seek needed for frame 0.
    ASSERT_TRUE(pool.decodeFor(fx.asset.assetId, clipId, Duration::zero(), step).isOk());
    EXPECT_EQ(pool.stats().seeks, 0u);

    // Ask it to run ahead somewhere the cursor is not. Every one of these needs a seek.
    pool.prefetch(fx.asset.assetId, clipId, step * 20, step, /*frameCount=*/4);
    ASSERT_TRUE(pool.drainFor(std::chrono::duration_cast<std::chrono::milliseconds>(kWaitCeiling)));

    EXPECT_EQ(pool.stats().seeks, 0u) << "prefetch sought, which is the expensive mistake";
    EXPECT_EQ(pool.queuedFrames(clipId), 0u);
    EXPECT_GE(pool.stats().prefetchDrops, 1u);

    // A continuation is still prefetched, so declining a seek has not disabled the queue.
    pool.prefetch(fx.asset.assetId, clipId, step, step, /*frameCount=*/2);
    ASSERT_TRUE(pool.drainFor(std::chrono::duration_cast<std::chrono::milliseconds>(kWaitCeiling)));
    EXPECT_GE(pool.stats().prefetchedFrames, 1u);
    EXPECT_EQ(pool.stats().seeks, 0u);

    pool.retireAsset(fx.asset.assetId);
    ASSERT_TRUE(
        teardown.drainFor(std::chrono::duration_cast<std::chrono::milliseconds>(kWaitCeiling)));
}

}  // namespace palmier::media
