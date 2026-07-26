// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/media/playback_frame_fidelity_property_test.cpp — Property 20 for the
// decode -> composite -> present pipeline (task 7.4 of the
// end-to-end-editor-integration spec; Requirement 5.1), plus the unit tests for
// media::DecoderClipFrameProvider and media::DecodeWorkerPool (task 7.3).
//
// Exactly one property lives here:
//
//   * Property 20 — presented frames match the decoded source frames: the
//     composited frame presented for a timeline position inside a clip matches
//     the source frame that the clip's (sourceIn, timelineStart) mapping selects,
//     within 2 of 255 levels per channel.
//
// How the property gets a *ground truth* to compare against. The subject is a
// chain — timeline position -> source position -> decoder -> gpu::SourceFrame ->
// gpu::Compositor::renderAt -> presented pixels — so the assertion needs an
// independent answer to "which source frame should this be?". That comes from
// MediaDecoder's existing DecodeBackendFactory / IDecodeBackend injection seam:
// a synthetic backend produces frames whose pixel values are a deterministic
// function of the *frame index*, so a presented frame identifies exactly which
// source frame reached the canvas. The test computes the expected index itself
// from `sourceIn + (position - timelineStart)` — it never asks the code under
// test what the mapping is — and then compares whole buffers.
//
// Consequences of that design worth stating:
//
//   * No real media, no FFmpeg, no GPU and no temporary files are involved. The
//     "paths" are opaque keys the synthetic factory dispatches on, and the
//     compositor runs on gpu::GpuContext::softwareFallback(), so nothing in this
//     file touches the filesystem or a device.
//   * A wrong mapping, a missing seek, a stale queued frame, a decoder mixed up
//     between two clips sharing an asset, or an LRU eviction that loses a
//     decoder's cursor all show up as a *content* mismatch with a shrunk
//     counterexample, never as a hang: every wait in the file is bounded.
//   * Timeline and source positions are exact multiples of the source frame
//     interval, which is what a frame-accurate playhead produces; the frame
//     interval itself is derived from the asset's declared FrameRate, including
//     an NTSC-style rational rate whose nanosecond interval does not divide a
//     second evenly.
//
// The generated cases cover the situations the task calls out: sequential
// playback, forward and backward seeks, interleaved access, clips whose sourceIn
// is non-zero, several clips sharing one asset, and more distinct assets than the
// decoder cache holds (so eviction pressure is real). Canvas sizes stay small in
// the property so 100+ cases stay fast; the 1920x1080 end of the documented
// generator range is covered by a dedicated unit test below.

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include "core/Clip.hpp"
#include "core/Duration.hpp"
#include "core/Error.hpp"
#include "core/FrameRate.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/Project.hpp"
#include "core/Resolution.hpp"
#include "core/Result.hpp"
#include "core/Track.hpp"
#include "core/Uuid.hpp"
#include "gpu/Compositor.hpp"
#include "gpu/GpuContext.hpp"
#include "media/DecodeWorkerPool.hpp"
#include "media/DecoderClipFrameProvider.hpp"
#include "media/DecoderTeardownQueue.hpp"
#include "media/MediaDecoder.hpp"
#include "media/MediaInfo.hpp"

namespace palmier::media {
namespace {

using namespace std::chrono_literals;

/// Requirement 5.1's tolerance: 2 of 255 levels per channel.
constexpr int kChannelTolerance = 2;

/// Generous ceiling for any bounded wait. Present so a regression fails an
/// assertion instead of hanging under the harness's per-test limit.
constexpr auto kWaitCeiling = 60s;

// ---------------------------------------------------------------------------
// The synthetic source: content derived from the frame index
// ---------------------------------------------------------------------------

/// One synthetic media asset. `salt` distinguishes assets, so a frame decoded
/// from the wrong decoder is detectable, and `frameCount` bounds its stream.
struct SyntheticAsset {
    Uuid          assetId{};
    std::string   path{};
    std::uint32_t width{0};
    std::uint32_t height{0};
    FrameRate     rate{};
    int           frameCount{0};
    std::uint32_t salt{0};

    [[nodiscard]] Duration frameStep() const noexcept { return rate.frameDuration(); }
};

/// The identifiable pixel content of frame `index` of `asset`. R is a bijection
/// of the frame index modulo 256, so two different frames of the same asset never
/// share a red channel for the frame counts used here; G mixes index and pixel
/// position; B identifies the asset.
[[nodiscard]] std::vector<std::uint8_t> syntheticPixels(const SyntheticAsset& asset, int index) {
    const std::size_t pixels = static_cast<std::size_t>(asset.width) * asset.height;
    std::vector<std::uint8_t> rgba(pixels * 4u);
    for (std::size_t p = 0; p < pixels; ++p) {
        const std::uint32_t i = static_cast<std::uint32_t>(index);
        rgba[p * 4u + 0] = static_cast<std::uint8_t>((i * 37u + asset.salt) & 0xFFu);
        rgba[p * 4u + 1] = static_cast<std::uint8_t>((i * 11u + (p & 0x3Fu)) & 0xFFu);
        rgba[p * 4u + 2] = static_cast<std::uint8_t>(((p >> 2) + asset.salt) & 0xFFu);
        rgba[p * 4u + 3] = 255u;
    }
    return rgba;
}

/// Backend counters, shared with the test so it can assert on decode behaviour.
struct BackendCounters {
    std::mutex    mutex{};
    std::uint64_t decodes{0};
    std::uint64_t seeks{0};
    std::uint64_t backendsOpened{0};

    void noteDecode() {
        std::lock_guard<std::mutex> lock(mutex);
        ++decodes;
    }
    void noteSeek() {
        std::lock_guard<std::mutex> lock(mutex);
        ++seeks;
    }
    void noteOpen() {
        std::lock_guard<std::mutex> lock(mutex);
        ++backendsOpened;
    }
    [[nodiscard]] std::uint64_t seekCount() {
        std::lock_guard<std::mutex> lock(mutex);
        return seeks;
    }
    [[nodiscard]] std::uint64_t openCount() {
        std::lock_guard<std::mutex> lock(mutex);
        return backendsOpened;
    }
};

/// A deterministic video decode backend over a SyntheticAsset. It behaves like a
/// real one where it matters: it is a *cursor*, nextFrame() advances it, and a
/// seek repositions it to the frame nearest the requested timestamp.
class SyntheticVideoBackend final : public IDecodeBackend {
public:
    SyntheticVideoBackend(SyntheticAsset asset, std::shared_ptr<BackendCounters> counters)
        : asset_(std::move(asset)), counters_(std::move(counters)) {
        MediaStreamInfo video;
        video.index      = 0;
        video.type       = MediaStreamType::Video;
        video.codec      = MediaCodecId::H264;
        video.codecName  = "h264";
        video.resolution = Resolution{asset_.width, asset_.height};
        video.frameRate  = asset_.rate;
        video.duration   = asset_.frameStep() * static_cast<std::int64_t>(asset_.frameCount);
        info_.containerFormat = "synthetic";
        info_.duration        = video.duration;
        info_.streams.push_back(video);
        if (counters_) counters_->noteOpen();
    }

    [[nodiscard]] const MediaInfo& info() const override { return info_; }

    [[nodiscard]] Result<BackendFrame> decode(bool) override {
        if (cursor_ >= asset_.frameCount) return BackendFrame::eos();

        BackendFrame frame;
        frame.timestamp   = asset_.frameStep() * static_cast<std::int64_t>(cursor_);
        frame.desc        = gpu::FrameDesc{asset_.width, asset_.height, gpu::FrameFormat::RGBA8};
        frame.hardware    = false;
        const std::vector<std::uint8_t> pixels = syntheticPixels(asset_, cursor_);
        frame.cpuPixels.resize(pixels.size());
        for (std::size_t i = 0; i < pixels.size(); ++i) {
            frame.cpuPixels[i] = static_cast<std::byte>(pixels[i]);
        }
        ++cursor_;
        if (counters_) counters_->noteDecode();
        return frame;
    }

    [[nodiscard]] Result<void> seek(Duration ts) override {
        const std::int64_t step = asset_.frameStep().ticks();
        std::int64_t       index = step > 0 ? (ts.ticks() + step / 2) / step : 0;
        index                    = std::clamp<std::int64_t>(index, 0, asset_.frameCount);
        cursor_                  = static_cast<int>(index);
        if (counters_) counters_->noteSeek();
        return ok();
    }

private:
    SyntheticAsset                   asset_;
    std::shared_ptr<BackendCounters> counters_;
    MediaInfo                        info_{};
    int                              cursor_{0};
};

/// A factory that dispatches on the (virtual) path each asset was registered
/// under. This is MediaDecoder's documented injection seam — no new seam was
/// added for the tests, and nothing here reads the filesystem.
[[nodiscard]] DecodeBackendFactory syntheticFactory(std::vector<SyntheticAsset> assets,
                                                    std::shared_ptr<BackendCounters> counters) {
    auto byPath = std::make_shared<std::unordered_map<std::string, SyntheticAsset>>();
    for (const auto& asset : assets) byPath->emplace(asset.path, asset);

    return [byPath, counters](const std::filesystem::path& path,
                              const DecodePrefs&) -> Result<std::unique_ptr<IDecodeBackend>> {
        const auto it = byPath->find(path.string());
        if (it == byPath->end()) {
            return err<std::unique_ptr<IDecodeBackend>>(
                notFound("no synthetic asset registered at " + path.string()));
        }
        return std::unique_ptr<IDecodeBackend>(
            std::make_unique<SyntheticVideoBackend>(it->second, counters));
    };
}

/// Software-only decode preferences: the synthetic backend produces CPU frames,
/// and playback in this suite must not depend on a device being present.
[[nodiscard]] DecodePrefs softwarePrefs() {
    DecodePrefs prefs;
    prefs.preferHardware = false;
    return prefs;
}

// ---------------------------------------------------------------------------
// Timeline construction
// ---------------------------------------------------------------------------

/// A clip plus the bookkeeping the test needs to predict its content.
struct PlacedClip {
    Clip     clip{};
    int      assetIndex{0};
    int      sourceInFrames{0};
    int      lengthFrames{0};
    Duration step{};
};

[[nodiscard]] Project projectWith(const std::vector<PlacedClip>& placed, Resolution canvas,
                                  FrameRate timelineFps) {
    Track track;
    track.id   = Uuid::generateV4();
    track.kind = TrackKind::Video;
    for (const auto& p : placed) track.clips.push_back(p.clip);

    Project project;
    project.id          = Uuid::generateV4();
    project.name        = "playback-fidelity";
    project.timelineFps = timelineFps;
    project.canvas      = canvas;
    project.tracks.push_back(std::move(track));
    return project;
}

/// Largest absolute per-channel difference between a rendered frame and the
/// expected source pixels, over the region the source covers on the canvas.
[[nodiscard]] int maxChannelDifference(const gpu::RenderedFrame& rendered,
                                      const std::vector<std::uint8_t>& expected,
                                      std::uint32_t sourceWidth, std::uint32_t sourceHeight) {
    const auto* out = static_cast<const std::uint8_t*>(rendered.hostData());
    if (out == nullptr) return 255;

    const std::uint32_t w = std::min(rendered.width(), sourceWidth);
    const std::uint32_t h = std::min(rendered.height(), sourceHeight);

    int worst = 0;
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::size_t s = (static_cast<std::size_t>(y) * sourceWidth + x) * 4u;
            const std::size_t d = (static_cast<std::size_t>(y) * rendered.width() + x) * 4u;
            for (int c = 0; c < 4; ++c) {
                const int diff = static_cast<int>(out[d + c]) - static_cast<int>(expected[s + c]);
                worst          = std::max(worst, diff < 0 ? -diff : diff);
            }
        }
    }
    return worst;
}

/// The frame rates the generator draws from: three integer rates and one
/// NTSC-style rational whose frame interval is not a whole number of
/// nanoseconds, so the mapping cannot rely on a tidy divisor.
[[nodiscard]] FrameRate rateFor(int choice) {
    switch (choice) {
        case 0: return FrameRate::fps24();
        case 1: return FrameRate::fps30();
        case 2: return FrameRate::fps60();
        default: return FrameRate::fps23_976();
    }
}

/// Access patterns over a clip's frames: the shapes real playback produces.
enum class AccessPattern { Sequential, ForwardSeek, BackwardSeek, Interleaved };

} // namespace

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
RC_GTEST_PROP(PlaybackFrameFidelityProperty,
              Property20PresentedFramesMatchTheDecodedSourceFrames,
              ()) {
    // --- generators ---------------------------------------------------------
    // Distinct assets on the timeline. More than the decoder cache holds is the
    // interesting case, so the cache capacity is generated independently below.
    const int assetCount = *rc::gen::inRange(1, 5);

    // Source geometry. Kept small so 100+ cases stay quick; the 1920x1080 end of
    // the documented range is covered by a unit test below.
    const std::uint32_t width  = static_cast<std::uint32_t>(*rc::gen::inRange(16, 65));
    const std::uint32_t height = static_cast<std::uint32_t>(*rc::gen::inRange(16, 49));

    // Decoder residency and prefetch depth: eviction pressure and worker
    // involvement are both part of the subject.
    const std::size_t cacheCapacity = static_cast<std::size_t>(*rc::gen::inRange(1, 5));
    const std::size_t prefetchDepth = static_cast<std::size_t>(*rc::gen::inRange(0, 4));

    // Clips on the timeline. With clipCount > assetCount, clips share an asset —
    // and therefore share one decoder cursor.
    const int clipCount = *rc::gen::inRange(1, 5);

    const auto pattern = *rc::gen::element(AccessPattern::Sequential, AccessPattern::ForwardSeek,
                                           AccessPattern::BackwardSeek,
                                           AccessPattern::Interleaved);

    // --- the synthetic library ----------------------------------------------
    std::vector<SyntheticAsset> assets;
    assets.reserve(static_cast<std::size_t>(assetCount));
    for (int a = 0; a < assetCount; ++a) {
        SyntheticAsset asset;
        // Uuid::generateV4() — never drawn byte-wise, so no nil/duplicate ids.
        asset.assetId    = Uuid::generateV4();
        asset.path       = "synthetic://asset-" + std::to_string(a) + "-" + asset.assetId.toString();
        asset.width      = width;
        asset.height     = height;
        asset.rate       = rateFor(*rc::gen::inRange(0, 4));
        asset.frameCount = *rc::gen::inRange(6, 25);
        asset.salt       = static_cast<std::uint32_t>(*rc::gen::inRange(0, 256));
        assets.push_back(asset);
    }

    // --- the timeline -------------------------------------------------------
    std::vector<PlacedClip> placed;
    placed.reserve(static_cast<std::size_t>(clipCount));
    Duration cursor = Duration::zero();
    for (int c = 0; c < clipCount; ++c) {
        const int             assetIndex = c % assetCount;
        const SyntheticAsset& asset      = assets[static_cast<std::size_t>(assetIndex)];
        const Duration        step       = asset.frameStep();

        // A non-zero sourceIn is the case the mapping most easily gets wrong.
        const int maxIn         = asset.frameCount / 2;
        const int sourceInFrame = maxIn > 0 ? *rc::gen::inRange(0, maxIn + 1) : 0;
        const int available     = asset.frameCount - sourceInFrame;
        const int lengthFrames  = *rc::gen::inRange(1, available + 1);

        // Clips are laid end to end on one video track, so exactly one clip is
        // visible at any position and the composited output is that clip's frame.
        Clip clip;
        clip.id            = Uuid::generateV4();
        clip.assetRef      = MediaAssetRef{asset.assetId, asset.path};
        clip.timelineStart = cursor;
        clip.sourceIn      = step * static_cast<std::int64_t>(sourceInFrame);
        clip.sourceOut     = step * static_cast<std::int64_t>(sourceInFrame + lengthFrames);
        clip.opacity       = 1.0;
        cursor             = clip.timelineEnd();

        placed.push_back(PlacedClip{clip, assetIndex, sourceInFrame, lengthFrames, step});
    }

    const std::uint32_t canvasW = width;
    const std::uint32_t canvasH = height;
    const Project       project =
        projectWith(placed, Resolution{canvasW, canvasH}, assets.front().rate);

    // --- the pipeline under test -------------------------------------------
    auto counters = std::make_shared<BackendCounters>();

    DecoderTeardownQueue teardown;
    ClipFrameProviderOptions options;
    options.decoderCacheCapacity = cacheCapacity;
    options.prefetchDepth        = prefetchDepth;
    options.prefs                = softwarePrefs();
    options.pool.workerCount     = 2; // the design's N = 2
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
        const PlacedClip&     p     = placed[clipIndex];
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
    RC_ASSERT(provider.pool().drainFor(std::chrono::duration_cast<std::chrono::milliseconds>(
        kWaitCeiling)));
    RC_ASSERT(teardown.drainFor(
        std::chrono::duration_cast<std::chrono::milliseconds>(kWaitCeiling)));
    RC_ASSERT(teardown.acceptedCount() >= provider.stats().evictions);
}

// ---------------------------------------------------------------------------
// Unit tests — the mapping, the seek policy, eviction/teardown, failure
// ---------------------------------------------------------------------------

namespace {

/// A one-asset fixture: 30 fps, `frameCount` frames, `size` x `size` pixels.
struct Fixture {
    SyntheticAsset                   asset{};
    std::shared_ptr<BackendCounters> counters{std::make_shared<BackendCounters>()};

    explicit Fixture(std::uint32_t width = 32, std::uint32_t height = 24, int frameCount = 12,
                     std::uint32_t salt = 5) {
        asset.assetId    = Uuid::generateV4();
        asset.path       = "synthetic://fixture-" + asset.assetId.toString();
        asset.width      = width;
        asset.height     = height;
        asset.rate       = FrameRate::fps30();
        asset.frameCount = frameCount;
        asset.salt       = salt;
    }

    [[nodiscard]] Duration step() const { return asset.frameStep(); }

    [[nodiscard]] Clip clipAt(int sourceInFrames, int lengthFrames,
                              Duration timelineStart = Duration::zero()) const {
        Clip clip;
        clip.id            = Uuid::generateV4();
        clip.assetRef      = MediaAssetRef{asset.assetId, asset.path};
        clip.timelineStart = timelineStart;
        clip.sourceIn      = step() * static_cast<std::int64_t>(sourceInFrames);
        clip.sourceOut     = step() * static_cast<std::int64_t>(sourceInFrames + lengthFrames);
        return clip;
    }

    [[nodiscard]] DecodeBackendFactory factory() const {
        return syntheticFactory({asset}, counters);
    }
};

[[nodiscard]] ClipFrameProviderOptions unitOptions(std::size_t capacity = 2,
                                                   std::size_t prefetch = 0) {
    ClipFrameProviderOptions options;
    options.decoderCacheCapacity   = capacity;
    options.prefetchDepth          = prefetch;
    options.prefs                  = softwarePrefs();
    options.pool.workerCount       = 2;
    options.pool.clipQueueCapacity = 3;
    return options;
}

[[nodiscard]] bool framesEqual(const gpu::SourceFrame& frame,
                               const std::vector<std::uint8_t>& expected) {
    return frame.rgba == expected;
}

} // namespace

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

namespace {

/// A backend that decodes `goodFrames` frames and then fails, standing in for a
/// corrupt packet mid-stream.
class FailingBackend final : public IDecodeBackend {
public:
    explicit FailingBackend(int goodFrames) : goodFrames_(goodFrames) {
        MediaStreamInfo video;
        video.index      = 0;
        video.type       = MediaStreamType::Video;
        video.codec      = MediaCodecId::H264;
        video.codecName  = "h264";
        video.resolution = Resolution{16, 16};
        video.frameRate  = FrameRate::fps30();
        info_.streams.push_back(video);
    }

    [[nodiscard]] const MediaInfo& info() const override { return info_; }

    [[nodiscard]] Result<BackendFrame> decode(bool) override {
        if (produced_ >= goodFrames_) {
            return err<BackendFrame>(makeError(ErrorCode::Io, "synthetic decode failure"));
        }
        ++produced_;
        BackendFrame frame;
        frame.desc = gpu::FrameDesc{16, 16, gpu::FrameFormat::RGBA8};
        frame.cpuPixels.assign(16u * 16u * 4u, std::byte{0x40});
        return frame;
    }

    [[nodiscard]] Result<void> seek(Duration) override { return ok(); }

private:
    MediaInfo info_{};
    int       goodFrames_;
    int       produced_{0};
};

} // namespace

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

TEST(DecodeWorkerPool, PrefetchedFramesSatisfyLaterRequestsAndTheQueueIsBounded) {
    Fixture              fx(16, 16, /*frameCount=*/16);
    DecoderTeardownQueue teardown;

    DecodeWorkerPoolOptions poolOptions;
    poolOptions.workerCount       = 2;
    poolOptions.clipQueueCapacity = 2;
    DecodeWorkerPool pool(teardown, poolOptions);
    EXPECT_EQ(pool.workerCount(), 2u);

    ASSERT_TRUE(pool.activateAsset(fx.asset.assetId, fx.asset.path, softwarePrefs(), fx.factory())
                    .isOk());

    const ClipId clipId = Uuid::generateV4();
    const Duration step = fx.step();

    // Ask for more than the queue holds; the surplus is dropped, never blocking.
    pool.prefetch(fx.asset.assetId, clipId, Duration::zero(), step, /*frameCount=*/8);
    ASSERT_TRUE(pool.drainFor(std::chrono::duration_cast<std::chrono::milliseconds>(kWaitCeiling)));
    EXPECT_LE(pool.queuedFrames(clipId), 2u);

    // The queued frames are the right ones, in order.
    for (int index = 0; index < 2; ++index) {
        auto frame = pool.decodeFor(fx.asset.assetId, clipId,
                                    step * static_cast<std::int64_t>(index), step);
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
    ASSERT_TRUE(teardown.drainFor(std::chrono::duration_cast<std::chrono::milliseconds>(
        kWaitCeiling)));
}

TEST(DecodeWorkerPool, ToSourceFrameRejectsUnusableDecodedFrames) {
    EXPECT_TRUE(toSourceFrame(DecodedFrame::endOfStream()).isError());

    // A CPU frame whose buffer is shorter than its declared geometry.
    DecodedFrame short_ = DecodedFrame::cpu(Duration::zero(),
                                            gpu::FrameDesc{4, 4, gpu::FrameFormat::RGBA8},
                                            std::vector<std::byte>(8, std::byte{0}));
    EXPECT_TRUE(toSourceFrame(short_).isError());

    // A well-formed CPU frame converts to an equally-sized source frame.
    DecodedFrame good = DecodedFrame::cpu(Duration::zero(),
                                          gpu::FrameDesc{4, 4, gpu::FrameFormat::RGBA8},
                                          std::vector<std::byte>(4u * 4u * 4u, std::byte{0x7F}));
    auto         converted = toSourceFrame(good);
    ASSERT_TRUE(converted.isOk());
    EXPECT_EQ(converted.value().width, 4u);
    EXPECT_EQ(converted.value().height, 4u);
    EXPECT_TRUE(converted.value().valid());
    EXPECT_EQ(converted.value().rgba[0], 0x7Fu);
}

} // namespace palmier::media
