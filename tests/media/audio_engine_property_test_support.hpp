// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shared fixtures and helpers for the audio_engine_property_test cases, split out so that no single file
// exceeds this repository's 1000-line ceiling.
//
// The support namespace opens INSIDE the original enclosing namespace, because these
// helpers name the types they use unqualified. `inline` and named rather than an
// anonymous namespace: an anonymous namespace in a header gives every translation unit
// its own copy, and a helper only some partitions use would warn as unused.

#ifndef PALMIER_TESTS_MEDIA_AUDIO_ENGINE_PROPERTY_TEST_SUPPORT_HPP_
#define PALMIER_TESTS_MEDIA_AUDIO_ENGINE_PROPERTY_TEST_SUPPORT_HPP_

#include "media/AudioEngine.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>
#include "core/Duration.hpp"
#include "core/Error.hpp"
#include "core/Project.hpp"
#include "core/Result.hpp"
#include "core/Track.hpp"
#include "core/Uuid.hpp"
#include "media/AudioGraph.hpp"
#include "media/AudioSink.hpp"
#include "media/DecoderTeardownQueue.hpp"
#include "media/MediaDecoder.hpp"

namespace palmier::media {
namespace testsupport_audioenginepropertytest {

namespace {

// ---------------------------------------------------------------------------
// TestClock — the injected steady clock every timing assertion is driven by
// ---------------------------------------------------------------------------

class TestClock {
public:
    [[nodiscard]] SteadyClock fn() {
        return [this] { return now_; };
    }
    void advance(std::chrono::nanoseconds by) { now_ += by; }
    void advance(Duration by) {
        now_ += std::chrono::nanoseconds{by.nanoseconds() > 0 ? by.nanoseconds() : 0};
    }
    [[nodiscard]] std::chrono::steady_clock::time_point now() const { return now_; }

private:
    std::chrono::steady_clock::time_point now_{};
};

// ---------------------------------------------------------------------------
// Sink fakes
// ---------------------------------------------------------------------------

/// A sink that records every submitted sample while behaving exactly like
/// NullAudioSink with respect to the clock (it delegates the clock arithmetic, so
/// the recorded run and the master clock cannot disagree).
class RecordingSink final : public IAudioSink {
public:
    explicit RecordingSink(SteadyClock clock) : inner_(std::move(clock)) {}

    [[nodiscard]] std::string_view name() const noexcept override { return "recording"; }

    [[nodiscard]] Result<void> start(const AudioSinkConfig& config) override {
        samples.clear();
        bufferFrames.clear();
        return inner_.start(config);
    }
    void stop() noexcept override { inner_.stop(); }
    [[nodiscard]] bool running() const noexcept override { return inner_.running(); }
    [[nodiscard]] AudioFormat format() const noexcept override { return inner_.format(); }
    [[nodiscard]] std::size_t quantumFrames() const noexcept override {
        return inner_.quantumFrames();
    }
    [[nodiscard]] Result<void> submit(const AudioBuffer& buffer) override {
        if (auto accepted = inner_.submit(buffer); accepted.isError()) return accepted;
        samples.insert(samples.end(), buffer.samples().begin(), buffer.samples().end());
        bufferFrames.push_back(buffer.frameCount());
        return ok();
    }
    [[nodiscard]] std::uint64_t playedFrames() const noexcept override {
        return inner_.playedFrames();
    }
    [[nodiscard]] std::uint64_t submittedFrames() const noexcept override {
        return inner_.submittedFrames();
    }

    /// Every sample delivered in this run, interleaved, in delivery order.
    std::vector<float> samples{};
    std::vector<std::size_t> bufferFrames{};

private:
    NullAudioSink inner_;
};

/// A sink that cannot be opened — the "audio output device is unavailable" case of
/// Requirement 6.7.
class UnavailableSink final : public IAudioSink {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "unavailable"; }
    [[nodiscard]] Result<void> start(const AudioSinkConfig&) override {
        ++startAttempts;
        return makeError(ErrorCode::Io, "no audio device could be opened");
    }
    void stop() noexcept override {}
    [[nodiscard]] bool running() const noexcept override { return false; }
    [[nodiscard]] AudioFormat format() const noexcept override { return AudioFormat{}; }
    [[nodiscard]] std::size_t quantumFrames() const noexcept override { return 0; }
    [[nodiscard]] Result<void> submit(const AudioBuffer&) override {
        return makeError(ErrorCode::FailedPrecondition, "the sink is not open");
    }
    [[nodiscard]] std::uint64_t playedFrames() const noexcept override { return 0; }
    [[nodiscard]] std::uint64_t submittedFrames() const noexcept override { return 0; }

    int startAttempts = 0;
};

// ---------------------------------------------------------------------------
// Synthetic audio source
// ---------------------------------------------------------------------------

/// What one synthetic asset decodes. A constant DC level makes the correct mixed
/// value knowable in closed form (see the file comment).
struct AssetScript {
    bool hasAudioStream = true;
    bool declareParameters = true;
    int sampleRate = 48'000;
    int channels = 2;
    float level = 0.25f;  ///< Every sample of every block.
    std::size_t blockFrames = 512;
    Duration duration = Duration::fromSeconds(120.0);
    /// Source position at which decodeAudio() starts failing (Requirement 6.9).
    std::optional<Duration> failFrom{};
};

/// Backend that emits `AssetScript`'s constant blocks, honours seekAudio, ends the
/// stream at the scripted duration and fails from the scripted offset onward.
class ScriptedAudioBackend final : public IDecodeBackend {
public:
    explicit ScriptedAudioBackend(AssetScript script) : script_(script) {
        int index = 0;
        MediaStreamInfo video;
        video.index = index++;
        video.type = MediaStreamType::Video;
        video.codec = MediaCodecId::H264;
        video.codecName = "h264";
        video.resolution = Resolution{64, 64};
        video.duration = script_.duration;
        info_.streams.push_back(video);

        if (script_.hasAudioStream) {
            MediaStreamInfo audio;
            audio.index = index++;
            audio.type = MediaStreamType::Audio;
            audio.codec = MediaCodecId::Pcm;
            audio.codecName = "pcm_f32le";
            audio.duration = script_.duration;
            audio.sampleRate = script_.declareParameters ? script_.sampleRate : 0;
            audio.channels = script_.declareParameters ? script_.channels : 0;
            info_.streams.push_back(audio);
        }
        info_.duration = script_.duration;
    }

    [[nodiscard]] const MediaInfo& info() const override { return info_; }

    [[nodiscard]] Result<BackendFrame> decode(bool) override { return BackendFrame::eos(); }
    [[nodiscard]] Result<void> seek(Duration) override { return ok(); }

    [[nodiscard]] Result<BackendAudioFrame> decodeAudio(int) override {
        if (script_.failFrom.has_value() && position_ >= *script_.failFrom) {
            return err<BackendAudioFrame>(
                makeError(ErrorCode::Io, "simulated audio decode failure"));
        }
        if (position_ >= script_.duration) return BackendAudioFrame::eos();

        const std::uint64_t remaining =
            durationToFrames(script_.duration - position_, script_.sampleRate);
        const std::size_t frames =
            std::min<std::size_t>(script_.blockFrames, static_cast<std::size_t>(remaining));
        if (frames == 0) return BackendAudioFrame::eos();

        std::vector<float> samples(frames * static_cast<std::size_t>(script_.channels),
                                   script_.level);
        BackendAudioFrame frame;
        frame.timestamp = position_;
        frame.buffer =
            AudioBuffer::interleaved(script_.sampleRate, script_.channels, std::move(samples));
        position_ =
            position_ + framesToDuration(static_cast<std::uint64_t>(frames), script_.sampleRate);
        return frame;
    }

    [[nodiscard]] Result<void> seekAudio(Duration ts, int) override {
        position_ = ts.isNegative() ? Duration::zero() : ts;
        return ok();
    }

private:
    AssetScript script_;
    MediaInfo info_{};
    Duration position_{};
};

/// Registry of scripted assets keyed by the path a clip's asset reference carries.
class AssetLibrary {
public:
    /// Register `script` under a fresh asset id and return the reference clips use.
    [[nodiscard]] MediaAssetRef add(AssetScript script) {
        const Uuid id = Uuid::generateV4();
        std::string path = "synthetic://asset-" + id.toString();
        scripts_.emplace(path, script);
        return MediaAssetRef{id, path};
    }

    [[nodiscard]] DecodeBackendFactory factory() const {
        // The registry outlives every engine in each property case.
        const auto* scripts = &scripts_;
        return [scripts](const std::filesystem::path& path,
                         const DecodePrefs&) -> Result<std::unique_ptr<IDecodeBackend>> {
            auto it = scripts->find(path.string());
            if (it == scripts->end()) {
                return err<std::unique_ptr<IDecodeBackend>>(
                    makeError(ErrorCode::NotFound, "no scripted asset at " + path.string()));
            }
            return std::unique_ptr<IDecodeBackend>(new ScriptedAudioBackend(it->second));
        };
    }

private:
    std::map<std::string, AssetScript> scripts_{};
};

// ---------------------------------------------------------------------------
// Project construction helpers
// ---------------------------------------------------------------------------

[[nodiscard]] inline Project makeProject() {
    Project project;
    project.id = Uuid::generateV4();
    project.name = "audio-engine-property";
    project.timelineFps = FrameRate{30, 1};
    project.canvas = Resolution{1920, 1080};
    return project;
}

/// Append an audio track holding exactly one clip, and return the clip id.
inline ClipId addAudioClip(Project& project, const MediaAssetRef& asset, Duration start,
                           Duration length, double gain, bool muted,
                           Duration sourceIn = Duration::zero()) {
    Track track;
    track.id = Uuid::generateV4();
    track.kind = TrackKind::Audio;
    track.muted = muted;

    Clip clip;
    clip.id = Uuid::generateV4();
    clip.assetRef = asset;
    clip.timelineStart = start;
    clip.sourceIn = sourceIn;
    clip.sourceOut = sourceIn + length;
    clip.gain = gain;

    const ClipId id = clip.id;
    track.clips.push_back(std::move(clip));
    project.tracks.push_back(std::move(track));
    project.assets.push_back(asset);
    return id;
}

/// Peak absolute sample of an interleaved buffer.
[[nodiscard]] inline float peak(const std::vector<float>& samples) {
    float highest = 0.0f;
    for (const float s : samples) highest = std::max(highest, std::abs(s));
    return highest;
}

[[nodiscard]] inline float clampUnit(double value) {
    return static_cast<float>(std::clamp(value, -1.0, 1.0));
}

/// The sample rates the generators draw from: the engine's own rate first (the
/// graph's exact identity path) then rates that force a real conversion.
[[nodiscard]] inline int rateFor(int index) {
    switch (index) {
        case 0:
            return 48'000;
        case 1:
            return 44'100;
        case 2:
            return 32'000;
        case 3:
            return 96'000;
        default:
            return 48'000;
    }
}

/// A configured engine plus everything it borrows, so a property can build one in
/// a line and let it die at scope exit.
struct EngineFixture {
    TestClock clock{};
    DecoderTeardownQueue teardown{};
    AssetLibrary library{};
    Project project{makeProject()};
    RecordingSink* sink{nullptr};

    [[nodiscard]] std::unique_ptr<AudioEngine> engine(std::size_t quantumFrames = 256) {
        auto owned = std::make_unique<RecordingSink>(clock.fn());
        sink = owned.get();
        AudioEngineOptions options;
        options.quantumFrames = quantumFrames;
        options.decoderCacheCapacity = 8;
        return std::make_unique<AudioEngine>([this] { return &project; }, std::move(owned),
                                             teardown, library.factory(), options, clock.fn());
    }
};

}  // namespace

}  // namespace testsupport_audioenginepropertytest
}  // namespace palmier::media

#endif  // PALMIER_TESTS_MEDIA_AUDIO_ENGINE_PROPERTY_TEST_SUPPORT_HPP_
