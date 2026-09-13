// SPDX-License-Identifier: GPL-3.0-or-later
//
// The playback_frame_fidelity_property_test cases that stayed in this file after it was split.
//
// Suites here: DecoderClipFrameProvider.
// Compiled into the same target as its siblings, so every case keeps the name CTest
// already knew it by.

#include "playback_frame_fidelity_property_test_support.hpp"

namespace palmier::media {

using namespace palmier::media::
    testsupport_playbackframefidelitypropertytest;  // NOLINT(google-build-using-namespace)

TEST(DecoderClipFrameProvider, NonZeroSourceInSelectsTheMappedSourceFrame) {
    Fixture              fx;
    DecoderTeardownQueue teardown;
    DecoderClipFrameProvider provider(teardown, fx.factory(), unitOptions());

    // sourceIn = frame 4, clip placed at timeline second 1: the frame presented
    // at timelineStart + 2 steps must be source frame 6.
    const Clip clip = fx.clipAt(/*sourceInFrames=*/4, /*lengthFrames=*/5,
                                Duration::fromSeconds(1.0));

    auto frame = provider.frameFor(clip, clip.timelineStart + fx.step() * 2);
    ASSERT_TRUE(frame.isOk()) << frame.error().toString();
    EXPECT_TRUE(framesEqual(frame.value(), syntheticPixels(fx.asset, 6)));

    // And the clip's first frame is source frame 4, not frame 0.
    auto first = provider.frameFor(clip, clip.timelineStart);
    ASSERT_TRUE(first.isOk()) << first.error().toString();
    EXPECT_TRUE(framesEqual(first.value(), syntheticPixels(fx.asset, 4)));
}
TEST(DecoderClipFrameProvider, SequentialPlaybackDecodesForwardWithoutSeeking) {
    Fixture              fx;
    DecoderTeardownQueue teardown;
    DecoderClipFrameProvider provider(teardown, fx.factory(), unitOptions());

    const Clip clip = fx.clipAt(/*sourceInFrames=*/2, /*lengthFrames=*/6);

    for (int offset = 0; offset < 6; ++offset) {
        auto frame = provider.frameFor(clip, clip.timelineStart +
                                                 fx.step() * static_cast<std::int64_t>(offset));
        ASSERT_TRUE(frame.isOk()) << frame.error().toString();
        EXPECT_TRUE(framesEqual(frame.value(), syntheticPixels(fx.asset, 2 + offset)))
            << "offset " << offset;
    }

    // Exactly one seek: the initial positioning onto sourceIn. Everything after
    // it was the decoder's next sequential frame.
    EXPECT_EQ(fx.counters->seekCount(), 1u);
    EXPECT_EQ(provider.poolStats().seeks, 1u);
    EXPECT_EQ(provider.poolStats().sequentialDecodes, 5u);
}
TEST(DecoderClipFrameProvider, BackwardSeekReturnsTheEarlierSourceFrame) {
    Fixture              fx;
    DecoderTeardownQueue teardown;
    DecoderClipFrameProvider provider(teardown, fx.factory(), unitOptions());

    const Clip clip = fx.clipAt(/*sourceInFrames=*/0, /*lengthFrames=*/8);

    auto forward = provider.frameFor(clip, clip.timelineStart + fx.step() * 6);
    ASSERT_TRUE(forward.isOk());
    EXPECT_TRUE(framesEqual(forward.value(), syntheticPixels(fx.asset, 6)));

    auto backward = provider.frameFor(clip, clip.timelineStart + fx.step() * 1);
    ASSERT_TRUE(backward.isOk());
    EXPECT_TRUE(framesEqual(backward.value(), syntheticPixels(fx.asset, 1)));

    // Both requests needed a seek: neither was the decoder's next frame.
    EXPECT_GE(provider.poolStats().seeks, 2u);
}
TEST(DecoderClipFrameProvider, TwoClipsSharingOneAssetUseOneDecoderAndStayCorrect) {
    Fixture              fx;
    DecoderTeardownQueue teardown;
    DecoderClipFrameProvider provider(teardown, fx.factory(), unitOptions());

    const Clip early = fx.clipAt(/*sourceInFrames=*/0, /*lengthFrames=*/4, Duration::zero());
    const Clip late  = fx.clipAt(/*sourceInFrames=*/6, /*lengthFrames=*/4,
                                 Duration::fromSeconds(2.0));

    for (int offset = 0; offset < 4; ++offset) {
        auto a = provider.frameFor(early, early.timelineStart +
                                              fx.step() * static_cast<std::int64_t>(offset));
        ASSERT_TRUE(a.isOk());
        EXPECT_TRUE(framesEqual(a.value(), syntheticPixels(fx.asset, offset)));

        auto b = provider.frameFor(late, late.timelineStart +
                                             fx.step() * static_cast<std::int64_t>(offset));
        ASSERT_TRUE(b.isOk());
        EXPECT_TRUE(framesEqual(b.value(), syntheticPixels(fx.asset, 6 + offset)));
    }

    // One asset, one decoder — the backend was opened exactly once.
    EXPECT_EQ(fx.counters->openCount(), 1u);
    EXPECT_EQ(provider.residentDecoderCount(), 1u);
}
TEST(DecoderClipFrameProvider, LruEvictionRetiresTheDecoderThroughTheTeardownQueue) {
    // Three assets, a cache that holds one: touching them in turn must evict.
    std::vector<SyntheticAsset>      assets;
    auto                             counters = std::make_shared<BackendCounters>();
    for (int a = 0; a < 3; ++a) {
        SyntheticAsset asset;
        asset.assetId    = Uuid::generateV4();
        asset.path       = "synthetic://lru-" + asset.assetId.toString();
        asset.width      = 16;
        asset.height     = 16;
        asset.rate       = FrameRate::fps30();
        asset.frameCount = 6;
        asset.salt       = static_cast<std::uint32_t>(a * 31);
        assets.push_back(asset);
    }

    DecoderTeardownQueue     teardown;
    DecoderClipFrameProvider provider(teardown, syntheticFactory(assets, counters),
                                      unitOptions(/*capacity=*/1));

    for (std::size_t a = 0; a < assets.size(); ++a) {
        Clip clip;
        clip.id            = Uuid::generateV4();
        clip.assetRef      = MediaAssetRef{assets[a].assetId, assets[a].path};
        clip.timelineStart = Duration::zero();
        clip.sourceIn      = Duration::zero();
        clip.sourceOut     = assets[a].frameStep() * 4;

        auto frame = provider.frameFor(clip, Duration::zero());
        ASSERT_TRUE(frame.isOk()) << frame.error().toString();
        EXPECT_TRUE(framesEqual(frame.value(), syntheticPixels(assets[a], 0)));
        EXPECT_LE(provider.residentDecoderCount(), 1u);
    }

    EXPECT_EQ(provider.stats().evictions, 2u);
    // The retired decoders went to the teardown queue rather than being closed
    // inline on the calling thread, and the queue drains to empty.
    EXPECT_GE(teardown.acceptedCount(), 2u);
    ASSERT_TRUE(teardown.drainFor(std::chrono::duration_cast<std::chrono::milliseconds>(
        kWaitCeiling)));
    EXPECT_EQ(teardown.pending(), 0u);
}
TEST(DecoderClipFrameProvider, ReleaseAllRetiresEveryResidentDecoder) {
    Fixture              fx;
    DecoderTeardownQueue teardown;
    DecoderClipFrameProvider provider(teardown, fx.factory(), unitOptions());

    const Clip clip = fx.clipAt(0, 4);
    ASSERT_TRUE(provider.frameFor(clip, Duration::zero()).isOk());
    ASSERT_EQ(provider.residentDecoderCount(), 1u);

    provider.releaseAll();
    EXPECT_EQ(provider.residentDecoderCount(), 0u);
    EXPECT_GE(teardown.acceptedCount(), 1u);
    ASSERT_TRUE(teardown.drainFor(std::chrono::duration_cast<std::chrono::milliseconds>(
        kWaitCeiling)));
}
TEST(DecoderClipFrameProvider, DecodeFailureIsReturnedAsAnErrorNamingTheAssetAndNoFrameIsPresented) {
    const Uuid  assetId = Uuid::generateV4();
    const std::string path = "synthetic://failing-" + assetId.toString();

    DecodeBackendFactory factory =
        [](const std::filesystem::path&, const DecodePrefs&)
            -> Result<std::unique_ptr<IDecodeBackend>> {
        return std::unique_ptr<IDecodeBackend>(std::make_unique<FailingBackend>(0));
    };

    DecoderTeardownQueue     teardown;
    DecoderClipFrameProvider provider(teardown, factory, unitOptions());

    Clip clip;
    clip.id            = Uuid::generateV4();
    clip.assetRef      = MediaAssetRef{assetId, path};
    clip.timelineStart = Duration::zero();
    clip.sourceIn      = Duration::zero();
    clip.sourceOut     = Duration::fromSeconds(1.0);

    auto direct = provider.frameFor(clip, Duration::zero());
    ASSERT_TRUE(direct.isError());
    EXPECT_NE(direct.error().message().find(path), std::string::npos)
        << "the error must name the asset: " << direct.error().message();

    // Requirement 5.5: the compositor propagates the error and emits no frame.
    Track track;
    track.id   = Uuid::generateV4();
    track.kind = TrackKind::Video;
    track.clips.push_back(clip);

    Project project;
    project.id          = Uuid::generateV4();
    project.name        = "decode-failure";
    project.timelineFps = FrameRate::fps30();
    project.canvas      = Resolution{16, 16};
    project.tracks.push_back(std::move(track));

    gpu::GpuContext context = gpu::GpuContext::softwareFallback();
    gpu::Compositor compositor(context);
    compositor.setFrameProvider(provider.asProvider());

    auto rendered = compositor.renderAt(project, Duration::zero(),
                                        gpu::RenderTarget{16, 16, gpu::RgbaColor::opaqueBlack()});
    EXPECT_TRUE(rendered.isError());
    EXPECT_GE(provider.stats().failures, 1u);
}
TEST(DecoderClipFrameProvider, PositionsOutsideTheClipSpanAreRejected) {
    Fixture              fx;
    DecoderTeardownQueue teardown;
    DecoderClipFrameProvider provider(teardown, fx.factory(), unitOptions());

    const Clip clip = fx.clipAt(/*sourceInFrames=*/1, /*lengthFrames=*/3,
                                Duration::fromSeconds(1.0));

    auto before = provider.frameFor(clip, clip.timelineStart - fx.step());
    ASSERT_TRUE(before.isError());
    EXPECT_EQ(before.error().code(), ErrorCode::OutOfRange);

    auto after = provider.frameFor(clip, clip.timelineEnd());
    ASSERT_TRUE(after.isError());
    EXPECT_EQ(after.error().code(), ErrorCode::OutOfRange);

    Clip assetless      = clip;
    assetless.assetRef  = MediaAssetRef{};
    auto missing        = provider.frameFor(assetless, assetless.timelineStart);
    ASSERT_TRUE(missing.isError());
    EXPECT_EQ(missing.error().code(), ErrorCode::InvalidArgument);
}
TEST(DecoderClipFrameProvider, FullHdFrameIsPresentedUnchanged) {
    // The 1920x1080 end of Property 20's documented generator range, as a single
    // example so the property itself stays fast.
    Fixture              fx(1920, 1080, /*frameCount=*/3, /*salt=*/9);
    DecoderTeardownQueue teardown;
    DecoderClipFrameProvider provider(teardown, fx.factory(), unitOptions());

    const Clip clip = fx.clipAt(/*sourceInFrames=*/1, /*lengthFrames=*/2);

    Track track;
    track.id   = Uuid::generateV4();
    track.kind = TrackKind::Video;
    track.clips.push_back(clip);

    Project project;
    project.id          = Uuid::generateV4();
    project.name        = "full-hd";
    project.timelineFps = FrameRate::fps30();
    project.canvas      = Resolution{1920, 1080};
    project.tracks.push_back(std::move(track));

    gpu::GpuContext context = gpu::GpuContext::softwareFallback();
    gpu::Compositor compositor(context);
    compositor.setFrameProvider(provider.asProvider());

    auto rendered = compositor.renderAt(project, Duration::zero(),
                                        gpu::RenderTarget{1920, 1080});
    ASSERT_TRUE(rendered.isOk()) << rendered.error().toString();
    EXPECT_LE(maxChannelDifference(rendered.value(), syntheticPixels(fx.asset, 1), 1920, 1080),
              kChannelTolerance);
}

}  // namespace palmier::media
