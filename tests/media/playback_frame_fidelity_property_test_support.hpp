// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shared fixtures and helpers for the playback_frame_fidelity_property_test cases, split out so that no single file
// exceeds this repository's 1000-line ceiling.
//
// The support namespace opens INSIDE the original enclosing namespace, because these
// helpers name the types they use unqualified. `inline` and named rather than an
// anonymous namespace: an anonymous namespace in a header gives every translation unit
// its own copy, and a helper only some partitions use would warn as unused.

#ifndef PALMIER_TESTS_MEDIA_PLAYBACK_FRAME_FIDELITY_PROPERTY_TEST_SUPPORT_HPP_
#define PALMIER_TESTS_MEDIA_PLAYBACK_FRAME_FIDELITY_PROPERTY_TEST_SUPPORT_HPP_

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
namespace testsupport_playbackframefidelitypropertytest {

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
    /// Frames between keyframes, as in a real encoded stream. A backward seek can
    /// only land on a keyframe, so this is what makes a seek inexact.
    int keyframeInterval{12};

    [[nodiscard]] Duration frameStep() const noexcept { return rate.frameDuration(); }

    /// The keyframe at or before `index` -- where av_seek_frame(AVSEEK_FLAG_BACKWARD)
    /// actually leaves the cursor.
    [[nodiscard]] int keyframeAtOrBefore(int index) const noexcept {
        const int gop = keyframeInterval > 0 ? keyframeInterval : 1;
        return (index / gop) * gop;
    }
};

/// The identifiable pixel content of frame `index` of `asset`. R is a bijection
/// of the frame index modulo 256, so two different frames of the same asset never
/// share a red channel for the frame counts used here; G mixes index and pixel
/// position; B identifies the asset.
[[nodiscard]] inline std::vector<std::uint8_t> syntheticPixels(const SyntheticAsset& asset, int index) {
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
/// real one where it matters: it is a *cursor*, nextFrame() advances it, and a seek
/// lands on the KEYFRAME at or before the requested timestamp -- never on the
/// requested frame itself unless that frame happens to be a keyframe.
///
/// That last property is deliberate and load-bearing. FFmpeg seeks with
/// AVSEEK_FLAG_BACKWARD and then flushes the codec, so the first frame decoded after a
/// seek is the preceding keyframe. A double that repositioned to the nearest frame
/// would be more accurate than the decoder it stands in for, and this suite -- whose
/// whole subject is whether the frame delivered is the frame requested -- would be
/// unable to observe an inexact seek at all.
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
        // Not `index`: a backward seek can only reach a keyframe. Everything between
        // the keyframe and the frame that was asked for has to be decoded through.
        cursor_ = asset_.keyframeAtOrBefore(static_cast<int>(index));
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
[[nodiscard]] inline DecodeBackendFactory syntheticFactory(std::vector<SyntheticAsset> assets,
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
[[nodiscard]] inline DecodePrefs softwarePrefs() {
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

[[nodiscard]] inline Project projectWith(const std::vector<PlacedClip>& placed, Resolution canvas,
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
[[nodiscard]] inline int maxChannelDifference(const gpu::RenderedFrame& rendered,
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
[[nodiscard]] inline FrameRate rateFor(int choice) {
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


// Helpers that sat between the cases in the single-file original.

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

[[nodiscard]] inline ClipFrameProviderOptions unitOptions(std::size_t capacity = 2,
                                                   std::size_t prefetch = 0) {
    ClipFrameProviderOptions options;
    options.decoderCacheCapacity   = capacity;
    options.prefetchDepth          = prefetch;
    options.prefs                  = softwarePrefs();
    options.pool.workerCount       = 2;
    options.pool.clipQueueCapacity = 3;
    return options;
}

[[nodiscard]] inline bool framesEqual(const gpu::SourceFrame& frame,
                               const std::vector<std::uint8_t>& expected) {
    return frame.rgba == expected;
}

} // namespace


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


}  // namespace testsupport_playbackframefidelitypropertytest
}  // namespace palmier::media

#endif  // PALMIER_TESTS_MEDIA_PLAYBACK_FRAME_FIDELITY_PROPERTY_TEST_SUPPORT_HPP_
