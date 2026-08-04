// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/media/export_audio_stream_test.cpp — the encoder's audio stream and the
// export engine's audio interleave (task 9.3; Requirements 6.5, 6.11).
//
// Two surfaces are under test here, and they are tested separately because they
// fail for different reasons:
//
//   * media::MediaEncoder's audio stream — EncodeSpec::audio, submitAudio()'s
//     format and ordering guards, the "no audio stream configured" refusal, and
//     finish() flushing BOTH streams before the trailer. All of it runs against a
//     scriptable IEncodeBackend, so the contract is asserted without FFmpeg, a
//     GPU or a vendor SDK.
//   * media::ExportEngine's interleave — for each video frame: render, submit the
//     frame, then submit THAT frame interval's audio. The assertions are about
//     the tiling of the intervals (no gap, no overlap, spanning the timeline) and
//     about the two streams' presentation times marching together, because those
//     are the facts a muxer depends on.
//
// Requirement 6.11 — "no clip on an unmuted audio-bearing track still gets one
// silent stream spanning the full timeline duration" — is checked twice over: once
// through the fallback silent renderer (no audio source bound at all) and once
// through media::AudioEngine::renderRange driving a project whose only audio track
// is muted, which is the production path. Reusing renderRange rather than a second
// mixer is the point: there is one definition of the export mix, and this file
// exercises it.
//
// Nothing here sleeps, opens a device, or needs a media fixture: the encode
// backend is a mock, the compositor runs the software reference on
// gpu::GpuContext::softwareFallback(), and the audio engine mixes through the
// always-available linear resampler with a NullAudioSink it never even starts.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/Duration.hpp"
#include "core/Error.hpp"
#include "core/FrameRate.hpp"
#include "core/Project.hpp"
#include "core/Resolution.hpp"
#include "core/Result.hpp"
#include "core/Uuid.hpp"
#include "gpu/Compositor.hpp"
#include "gpu/GpuContext.hpp"
#include "gpu/GpuTypes.hpp"
#include "media/AudioEngine.hpp"
#include "media/AudioGraph.hpp"
#include "media/AudioSink.hpp"
#include "media/DecoderTeardownQueue.hpp"
#include "media/ExportEngine.hpp"
#include "media/MediaEncoder.hpp"

namespace palmier::media {
namespace {

constexpr Resolution kRes{4, 4};

// --- A scriptable encode backend that records BOTH streams ------------------

/// One recorded audio submission: enough to reconstruct the stream's timing and
/// length without keeping the samples themselves.
struct AudioSubmission {
    Duration    presentation{Duration::zero()};
    std::size_t frames{0};
    int         sampleRate{0};
    int         channels{0};
    /// True when every sample in the block was exactly 0.0 — which is how
    /// "silence" is asserted rather than assumed (Requirement 6.11).
    bool        silent{true};
    /// The largest absolute sample value in the block, so the [-1, 1] range of
    /// Requirement 6.8 is checkable at the encoder boundary.
    float       peak{0.0f};
};

struct RecordingState {
    std::vector<Duration>        videoPresentations{};
    std::vector<AudioSubmission> audio{};
    int                          finishCalls{0};
    /// Set when finish() ran; used to assert that the flush happens exactly once
    /// and after every submission.
    std::size_t audioBlocksAtFinish{0};
    std::size_t videoFramesAtFinish{0};
    /// Scripted failures.
    int  failAudioOnBlock{-1};   ///< 0-based block index to fail, or -1.
    bool failFinish{false};
};

class RecordingBackend final : public IEncodeBackend {
public:
    explicit RecordingBackend(RecordingState* state) : state_(state) {}

    [[nodiscard]] Result<void> encode(const EncoderInputFrame& frame) override {
        state_->videoPresentations.push_back(frame.presentation);
        return ok();
    }

    [[nodiscard]] Result<void> encodeAudio(const EncoderInputAudio& audio) override {
        if (audio.buffer == nullptr) {
            return err(invalidArgument("mock backend received no audio buffer"));
        }
        const int index = static_cast<int>(state_->audio.size());
        if (state_->failAudioOnBlock == index) {
            return err(makeError(ErrorCode::Io, "mock audio encode failure"));
        }
        AudioSubmission record;
        record.presentation = audio.presentation;
        record.frames = audio.buffer->frameCount();
        record.sampleRate = audio.buffer->sampleRate();
        record.channels = audio.buffer->channels();
        for (float sample : audio.buffer->samples()) {
            if (sample != 0.0f) record.silent = false;
            record.peak = std::max(record.peak, sample < 0.0f ? -sample : sample);
        }
        state_->audio.push_back(record);
        return ok();
    }

    [[nodiscard]] Result<void> finish() override {
        ++state_->finishCalls;
        state_->audioBlocksAtFinish = state_->audio.size();
        state_->videoFramesAtFinish = state_->videoPresentations.size();
        if (state_->failFinish) return err(makeError(ErrorCode::Io, "mock finish failure"));
        return ok();
    }

private:
    RecordingState* state_;
};

EncodeBackendFactory recordingFactory(RecordingState* state) {
    return [state](const EncodeSpec&, const gpu::CodecRoute&)
               -> Result<std::unique_ptr<IEncodeBackend>> {
        return std::unique_ptr<IEncodeBackend>(std::make_unique<RecordingBackend>(state));
    };
}

/// A backend with NO audio support at all: it inherits IEncodeBackend's default
/// encodeAudio(), which is what every pre-audio backend and every video-only mock
/// in the tree does.
class VideoOnlyBackend final : public IEncodeBackend {
public:
    [[nodiscard]] Result<void> encode(const EncoderInputFrame&) override { return ok(); }
    [[nodiscard]] Result<void> finish() override { return ok(); }
};

EncodeBackendFactory videoOnlyFactory() {
    return [](const EncodeSpec&, const gpu::CodecRoute&)
               -> Result<std::unique_ptr<IEncodeBackend>> {
        return std::unique_ptr<IEncodeBackend>(std::make_unique<VideoOnlyBackend>());
    };
}

// --- Spec / project helpers -------------------------------------------------

EncodeSpec videoSpec() {
    EncodeSpec spec;
    spec.codec = gpu::CodecId::H264;
    spec.resolution = kRes;
    spec.frameRate = FrameRate::fps30();
    spec.preferHardware = false;
    spec.outputPath = "unused-by-the-mock.mp4";
    spec.containerFormat = "mp4";
    return spec;
}

EncodeSpec audioSpec() {
    EncodeSpec spec = videoSpec();
    spec.audio = AudioEncodeSpec{}; // 48 kHz / 2ch / aac — the Audio_Engine format.
    return spec;
}

AudioBuffer silence(std::size_t frames, int rate = 48'000, int channels = 2) {
    return AudioBuffer(rate, channels, frames);
}

AudioBuffer tone(std::size_t frames, float amplitude) {
    AudioBuffer buffer(48'000, 2, frames);
    for (std::size_t i = 0; i < buffer.samples().size(); ++i) {
        buffer.samples()[i] = (i % 2 == 0) ? amplitude : -amplitude;
    }
    return buffer;
}

/// A video track with one clip covering `frameCount` frames at 30 fps.
Project videoProject(int frameCount) {
    Clip clip;
    clip.id = Uuid::generateV4();
    clip.timelineStart = Duration::zero();
    clip.sourceIn = Duration::zero();
    clip.sourceOut = FrameRate::fps30().durationForFrames(frameCount);
    clip.opacity = 1.0;

    Track track;
    track.id = Uuid::generateV4();
    track.kind = TrackKind::Video;
    track.clips = {clip};

    Project project;
    project.id = Uuid::generateV4();
    project.name = "export-audio-test";
    project.timelineFps = FrameRate::fps30();
    project.canvas = kRes;
    project.tracks = {track};
    return project;
}

/// The same timeline plus an audio track whose single clip is on a MUTED track —
/// i.e. a timeline with no clip on an unmuted audio-bearing track, which is
/// exactly Requirement 6.11's premise.
Project videoProjectWithMutedAudio(int frameCount) {
    Project project = videoProject(frameCount);

    Clip clip;
    clip.id = Uuid::generateV4();
    clip.timelineStart = Duration::zero();
    clip.sourceIn = Duration::zero();
    clip.sourceOut = FrameRate::fps30().durationForFrames(frameCount);
    clip.gain = 1.0;
    clip.assetRef.assetId = Uuid::generateV4();
    clip.assetRef.sourcePath = "/nonexistent/muted-audio.wav";

    Track track;
    track.id = Uuid::generateV4();
    track.kind = TrackKind::Audio;
    track.muted = true; // muted ⇒ contributes nothing (Requirement 6.2).
    track.clips = {clip};

    project.tracks.push_back(track);
    return project;
}

std::unique_ptr<gpu::Compositor> makeCompositor(gpu::GpuContext& ctx) {
    auto compositor = std::make_unique<gpu::Compositor>(ctx);
    compositor->setFrameProvider([](const Clip&, Duration) -> Result<gpu::SourceFrame> {
        return gpu::SourceFrame::solid(kRes.width, kRes.height, gpu::RgbaColor{7, 9, 11, 255});
    });
    return compositor;
}

ExportRequest audioExportRequest() {
    ExportRequest request;
    request.codec = gpu::CodecId::H264;
    request.resolution = kRes;
    request.frameRate = FrameRate::fps30();
    request.bitrateBitsPerSecond = 2'000'000;
    request.preferHardware = false;
    request.outputPath = "unused-by-the-mock.mp4";
    request.containerFormat = "mp4";
    request.progressInterval = std::chrono::milliseconds{0};
    request.includeAudio = true;
    return request;
}

// ===========================================================================
// MediaEncoder — the audio stream (Requirement 6.5)
// ===========================================================================

TEST(MediaEncoderAudio, AVideoOnlyEncoderReportsNoAudioStream) {
    RecordingState state;
    auto encoder = MediaEncoder::create(videoSpec(), recordingFactory(&state));
    ASSERT_TRUE(encoder.isOk());
    EXPECT_FALSE(encoder.value().hasAudioStream());
    EXPECT_FALSE(encoder.value().audioSpec().has_value());
}

TEST(MediaEncoderAudio, AnAudioSpecIsCarriedOntoTheEncoder) {
    RecordingState state;
    EncodeSpec spec = audioSpec();
    spec.audio->sampleRate = 48'000;
    spec.audio->channels = 2;
    spec.audio->codecName = "aac";
    spec.audio->bitrateBitsPerSecond = 192'000;

    auto encoder = MediaEncoder::create(spec, recordingFactory(&state));
    ASSERT_TRUE(encoder.isOk());
    ASSERT_TRUE(encoder.value().hasAudioStream());
    ASSERT_TRUE(encoder.value().audioSpec().has_value());
    EXPECT_EQ(encoder.value().audioSpec()->sampleRate, 48'000);
    EXPECT_EQ(encoder.value().audioSpec()->channels, 2);
    EXPECT_EQ(encoder.value().audioSpec()->codecName, "aac");
    EXPECT_EQ(encoder.value().audioSpec()->bitrateBitsPerSecond, 192'000);
    // The default AudioEncodeSpec IS the Audio_Engine output format.
    EXPECT_EQ(AudioEncodeSpec{}.sampleRate, AudioEngine::kOutputSampleRate);
    EXPECT_EQ(AudioEncodeSpec{}.channels, AudioEngine::kOutputChannels);
}

TEST(MediaEncoderAudio, SubmitAudioForwardsBlocksAndCountsFrames) {
    RecordingState state;
    auto encoder = MediaEncoder::create(audioSpec(), recordingFactory(&state));
    ASSERT_TRUE(encoder.isOk());
    MediaEncoder enc = std::move(encoder).value();

    ASSERT_TRUE(enc.submitAudio(tone(1600, 0.5f), Duration::zero()).isOk());
    ASSERT_TRUE(enc.submitAudio(tone(1600, 0.5f), Duration::fromMilliseconds(33)).isOk());

    EXPECT_EQ(enc.submittedAudioBlockCount(), 2u);
    EXPECT_EQ(enc.submittedAudioFrameCount(), 3200u);
    EXPECT_EQ(enc.lastAudioPresentationTime(), Duration::fromMilliseconds(33));

    ASSERT_EQ(state.audio.size(), 2u);
    EXPECT_EQ(state.audio[0].frames, 1600u);
    EXPECT_EQ(state.audio[0].sampleRate, 48'000);
    EXPECT_EQ(state.audio[0].channels, 2);
    EXPECT_FALSE(state.audio[0].silent);
    EXPECT_LE(state.audio[1].peak, 1.0f);
}

TEST(MediaEncoderAudio, SubmitAudioIsRefusedWithoutAnAudioStream) {
    RecordingState state;
    auto encoder = MediaEncoder::create(videoSpec(), recordingFactory(&state));
    ASSERT_TRUE(encoder.isOk());
    MediaEncoder enc = std::move(encoder).value();

    auto submitted = enc.submitAudio(silence(480), Duration::zero());
    ASSERT_TRUE(submitted.isError());
    EXPECT_EQ(submitted.error().code(), ErrorCode::FailedPrecondition);
    // Refused, not silently dropped: nothing reached the backend and no state moved.
    EXPECT_TRUE(state.audio.empty());
    EXPECT_EQ(enc.submittedAudioBlockCount(), 0u);
}

TEST(MediaEncoderAudio, ABackendWithoutAudioSupportReportsUnsupported) {
    // The inherited default IEncodeBackend::encodeAudio: an encoder configured for
    // audio over a video-only backend fails loudly rather than dropping audio.
    auto encoder = MediaEncoder::create(audioSpec(), videoOnlyFactory());
    ASSERT_TRUE(encoder.isOk());
    MediaEncoder enc = std::move(encoder).value();

    auto submitted = enc.submitAudio(silence(480), Duration::zero());
    ASSERT_TRUE(submitted.isError());
    EXPECT_EQ(submitted.error().code(), ErrorCode::Unsupported);
    EXPECT_EQ(enc.submittedAudioBlockCount(), 0u);
}

TEST(MediaEncoderAudio, AFormatMismatchIsRejectedWithoutTouchingTheStream) {
    RecordingState state;
    auto encoder = MediaEncoder::create(audioSpec(), recordingFactory(&state));
    ASSERT_TRUE(encoder.isOk());
    MediaEncoder enc = std::move(encoder).value();

    auto wrongRate = enc.submitAudio(silence(480, 44'100, 2), Duration::zero());
    ASSERT_TRUE(wrongRate.isError());
    EXPECT_EQ(wrongRate.error().code(), ErrorCode::InvalidArgument);

    auto wrongChannels = enc.submitAudio(silence(480, 48'000, 1), Duration::zero());
    ASSERT_TRUE(wrongChannels.isError());
    EXPECT_EQ(wrongChannels.error().code(), ErrorCode::InvalidArgument);

    EXPECT_TRUE(state.audio.empty());
    EXPECT_EQ(enc.submittedAudioBlockCount(), 0u);
}

TEST(MediaEncoderAudio, ARegressingPresentationTimeIsRejected) {
    RecordingState state;
    auto encoder = MediaEncoder::create(audioSpec(), recordingFactory(&state));
    ASSERT_TRUE(encoder.isOk());
    MediaEncoder enc = std::move(encoder).value();

    ASSERT_TRUE(enc.submitAudio(silence(480), Duration::fromMilliseconds(100)).isOk());
    auto regressed = enc.submitAudio(silence(480), Duration::fromMilliseconds(50));
    ASSERT_TRUE(regressed.isError());
    EXPECT_EQ(regressed.error().code(), ErrorCode::InvalidArgument);

    // The rejected block advanced nothing.
    EXPECT_EQ(enc.submittedAudioBlockCount(), 1u);
    EXPECT_EQ(enc.lastAudioPresentationTime(), Duration::fromMilliseconds(100));
    // An equal timestamp is NOT a regression and is accepted.
    EXPECT_TRUE(enc.submitAudio(silence(480), Duration::fromMilliseconds(100)).isOk());
}

TEST(MediaEncoderAudio, AnEmptyBlockIsAccepted) {
    // A frame interval that mixes to zero audio frames is normal at high frame
    // rates, not an error.
    RecordingState state;
    auto encoder = MediaEncoder::create(audioSpec(), recordingFactory(&state));
    ASSERT_TRUE(encoder.isOk());
    MediaEncoder enc = std::move(encoder).value();

    ASSERT_TRUE(enc.submitAudio(silence(0), Duration::zero()).isOk());
    EXPECT_EQ(enc.submittedAudioBlockCount(), 1u);
    EXPECT_EQ(enc.submittedAudioFrameCount(), 0u);
}

TEST(MediaEncoderAudio, FinishFlushesBothStreamsExactlyOnceAndThenRefusesAudio) {
    RecordingState state;
    auto encoder = MediaEncoder::create(audioSpec(), recordingFactory(&state));
    ASSERT_TRUE(encoder.isOk());
    MediaEncoder enc = std::move(encoder).value();

    ASSERT_TRUE(enc.submitAudio(silence(1600), Duration::zero()).isOk());
    ASSERT_TRUE(enc.finish().isOk());

    // finish() ran after everything that was submitted — the flush covers both
    // streams, which is what the trailer depends on.
    EXPECT_EQ(state.finishCalls, 1);
    EXPECT_EQ(state.audioBlocksAtFinish, 1u);

    auto afterFinish = enc.submitAudio(silence(480), Duration::fromMilliseconds(100));
    ASSERT_TRUE(afterFinish.isError());
    EXPECT_EQ(afterFinish.error().code(), ErrorCode::FailedPrecondition);
    // A second finish is refused, as for the video-only encoder.
    EXPECT_TRUE(enc.finish().isError());
}

TEST(MediaEncoderAudio, AMalformedAudioSpecIsRejectedBeforeTheBackendIsBuilt) {
    struct Case {
        const char* what;
        int         sampleRate;
        int         channels;
        std::int64_t bitrate;
        std::string codecName;
    };
    const Case cases[] = {
        {"zero sample rate", 0, 2, 192'000, "aac"},
        {"negative sample rate", -48'000, 2, 192'000, "aac"},
        {"zero channels", 48'000, 0, 192'000, "aac"},
        {"negative bit rate", 48'000, 2, -1, "aac"},
        {"empty codec name", 48'000, 2, 192'000, ""},
    };

    for (const Case& c : cases) {
        RecordingState state;
        EncodeSpec spec = audioSpec();
        spec.audio->sampleRate = c.sampleRate;
        spec.audio->channels = c.channels;
        spec.audio->bitrateBitsPerSecond = c.bitrate;
        spec.audio->codecName = c.codecName;

        auto encoder = MediaEncoder::create(spec, recordingFactory(&state));
        ASSERT_TRUE(encoder.isError()) << c.what;
        EXPECT_EQ(encoder.error().code(), ErrorCode::InvalidArgument) << c.what;
        // Rejected before the backend existed: nothing was opened.
        EXPECT_EQ(state.finishCalls, 0) << c.what;
    }
}

// ===========================================================================
// ExportEngine — the audio interleave (Requirements 6.5, 6.11)
// ===========================================================================

TEST(ExportEngineAudio, VideoOnlyByDefaultSoNoAudioIsSubmitted) {
    auto ctx = gpu::GpuContext::softwareFallback();
    auto compositor = makeCompositor(ctx);
    ExportEngine engine(*compositor);

    RecordingState state;
    ExportRequest request = audioExportRequest();
    request.includeAudio = false;

    Project project = videoProject(4);
    auto result = engine.run(project, request, recordingFactory(&state));
    ASSERT_TRUE(result.isOk());
    EXPECT_FALSE(result.value().containsAudio);
    EXPECT_EQ(result.value().audioBlocks, 0u);
    EXPECT_EQ(result.value().audioFrames, 0u);
    EXPECT_TRUE(state.audio.empty());
}

TEST(ExportEngineAudio, SubmitsOneAudioBlockPerVideoFrameInterval) {
    auto ctx = gpu::GpuContext::softwareFallback();
    auto compositor = makeCompositor(ctx);
    ExportEngine engine(*compositor);

    RecordingState state;
    Project project = videoProject(5);
    auto result = engine.run(project, audioExportRequest(), recordingFactory(&state));
    ASSERT_TRUE(result.isOk());

    EXPECT_TRUE(result.value().containsAudio);
    EXPECT_EQ(result.value().framesRendered, 5u);
    EXPECT_EQ(result.value().audioBlocks, 5u);
    ASSERT_EQ(state.audio.size(), 5u);
    ASSERT_EQ(state.videoPresentations.size(), 5u);

    // Each audio block carries the presentation time of the frame it belongs to,
    // so the two streams march together through the muxer.
    for (std::size_t i = 0; i < state.audio.size(); ++i) {
        EXPECT_EQ(state.audio[i].presentation, state.videoPresentations[i]) << "block " << i;
    }
    // Non-decreasing, like the video stream.
    for (std::size_t i = 1; i < state.audio.size(); ++i) {
        EXPECT_LT(state.audio[i - 1].presentation, state.audio[i].presentation);
    }
}

TEST(ExportEngineAudio, TheAudioStreamSpansTheWholeTimelineWithinOneFrameInterval) {
    auto ctx = gpu::GpuContext::softwareFallback();
    auto compositor = makeCompositor(ctx);
    ExportEngine engine(*compositor);

    RecordingState state;
    Project project = videoProject(10);
    auto result = engine.run(project, audioExportRequest(), recordingFactory(&state));
    ASSERT_TRUE(result.isOk());

    const std::size_t planned = ExportEngine::plannedFrameCount(project, FrameRate::fps30());
    ASSERT_EQ(planned, 10u);

    // The per-frame intervals tile [0, planned * frameStep) with no gap and no
    // overlap, so the total audio frame count is the whole span at 48 kHz.
    const Duration span = FrameRate::fps30().durationForFrames(static_cast<std::int64_t>(planned));
    const std::uint64_t expectedFrames = durationToFrames(span, AudioEngine::kOutputSampleRate);
    EXPECT_EQ(result.value().audioFrames, expectedFrames);

    // And that span covers the timeline duration, exceeding it by less than one
    // video frame interval (Requirements 6.8, 7.4).
    const Duration audioDuration =
        framesToDuration(result.value().audioFrames, AudioEngine::kOutputSampleRate);
    const Duration timeline = FrameRate::fps30().durationForFrames(10);
    EXPECT_GE(audioDuration.nanoseconds(), timeline.nanoseconds() -
                                               FrameRate::fps30().frameDuration().nanoseconds());
    EXPECT_LE(audioDuration.nanoseconds(),
              timeline.nanoseconds() + FrameRate::fps30().frameDuration().nanoseconds());
}

TEST(ExportEngineAudio, WithNoAudioSourceTheStreamIsFullLengthSilence) {
    // Requirement 6.11 through the fallback renderer: no audio source bound at
    // all, yet the export still carries one full-length silent stream.
    auto ctx = gpu::GpuContext::softwareFallback();
    auto compositor = makeCompositor(ctx);
    ExportEngine engine(*compositor);
    ASSERT_FALSE(engine.hasAudioRangeRenderer());

    RecordingState state;
    Project project = videoProject(6);
    auto result = engine.run(project, audioExportRequest(), recordingFactory(&state));
    ASSERT_TRUE(result.isOk());

    EXPECT_TRUE(result.value().containsAudio);
    ASSERT_EQ(state.audio.size(), 6u);
    for (const AudioSubmission& block : state.audio) {
        EXPECT_TRUE(block.silent);
        EXPECT_GT(block.frames, 0u);
        EXPECT_EQ(block.sampleRate, AudioEngine::kOutputSampleRate);
        EXPECT_EQ(block.channels, AudioEngine::kOutputChannels);
    }
}

TEST(ExportEngineAudio, AnInjectedRendererIsUsedForEveryInterval) {
    auto ctx = gpu::GpuContext::softwareFallback();
    auto compositor = makeCompositor(ctx);

    std::vector<std::pair<Duration, Duration>> ranges;
    ExportEngine engine(*compositor,
                        [&ranges](const Project&, Duration from, Duration to)
                            -> Result<AudioBuffer> {
                            ranges.emplace_back(from, to);
                            const std::size_t frames = static_cast<std::size_t>(
                                durationToFrames(to - from, AudioEngine::kOutputSampleRate));
                            return tone(frames, 0.25f);
                        });
    ASSERT_TRUE(engine.hasAudioRangeRenderer());

    RecordingState state;
    Project project = videoProject(4);
    auto result = engine.run(project, audioExportRequest(), recordingFactory(&state));
    ASSERT_TRUE(result.isOk());

    // Exactly one range per frame, tiling the timeline: each range starts where
    // the previous one ended.
    ASSERT_EQ(ranges.size(), 4u);
    EXPECT_EQ(ranges.front().first, Duration::zero());
    for (std::size_t i = 1; i < ranges.size(); ++i) {
        EXPECT_EQ(ranges[i].first, ranges[i - 1].second) << "gap or overlap before range " << i;
    }
    EXPECT_EQ(ranges.back().second, FrameRate::fps30().durationForFrames(4));

    // The renderer's samples really reached the encoder.
    ASSERT_EQ(state.audio.size(), 4u);
    for (const AudioSubmission& block : state.audio) {
        EXPECT_FALSE(block.silent);
        EXPECT_LE(block.peak, 1.0f);
    }
}

TEST(ExportEngineAudio, AFailingAudioMixStopsTheExportAndRemovesNothingItDidNotCreate) {
    auto ctx = gpu::GpuContext::softwareFallback();
    auto compositor = makeCompositor(ctx);
    ExportEngine engine(*compositor,
                        [](const Project&, Duration, Duration) -> Result<AudioBuffer> {
                            return err<AudioBuffer>(makeError(ErrorCode::Io, "mix failure"));
                        });

    RecordingState state;
    Project project = videoProject(3);
    auto result = engine.run(project, audioExportRequest(), recordingFactory(&state));
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::Io);
    // It stopped at the first frame's audio, so only that frame's video was
    // submitted (Requirement 6.10 stops the export rather than continuing silent).
    EXPECT_EQ(state.videoPresentations.size(), 1u);
    EXPECT_TRUE(state.audio.empty());
}

TEST(ExportEngineAudio, AFailingAudioEncodeStopsTheExport) {
    auto ctx = gpu::GpuContext::softwareFallback();
    auto compositor = makeCompositor(ctx);
    ExportEngine engine(*compositor);

    RecordingState state;
    state.failAudioOnBlock = 2; // the third interval's audio fails.
    Project project = videoProject(5);
    auto result = engine.run(project, audioExportRequest(), recordingFactory(&state));
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::Io);
    EXPECT_EQ(state.audio.size(), 2u);
    EXPECT_EQ(state.videoPresentations.size(), 3u);
}

// ===========================================================================
// Requirement 6.11 through the PRODUCTION path: AudioEngine::renderRange
// ===========================================================================

/// The export mix bound to media::AudioEngine::renderRange — the same call
/// playback mixes through, and the only mixer in the pipeline.
class ExportAudioEngineFixture : public ::testing::Test {
protected:
    void SetUp() override {
        engine_ = std::make_unique<AudioEngine>(
            []() -> const Project* { return nullptr; }, std::make_unique<NullAudioSink>(),
            teardown_, DecodeBackendFactory{});
    }

    AudioRangeRenderer renderer() {
        return [this](const Project& project, Duration from, Duration to) {
            return engine_->renderRange(project, from, to);
        };
    }

    DecoderTeardownQueue         teardown_{};
    std::unique_ptr<AudioEngine> engine_{};
};

TEST_F(ExportAudioEngineFixture, AMutedAudioTrackStillYieldsAFullLengthSilentStream) {
    auto ctx = gpu::GpuContext::softwareFallback();
    auto compositor = makeCompositor(ctx);
    ExportEngine engine(*compositor, renderer());

    RecordingState state;
    Project project = videoProjectWithMutedAudio(8);
    auto result = engine.run(project, audioExportRequest(), recordingFactory(&state));
    ASSERT_TRUE(result.isOk());

    EXPECT_TRUE(result.value().containsAudio);
    ASSERT_EQ(state.audio.size(), 8u);

    // Silence for every interval, and the whole timeline covered: the muted track
    // contributed nothing, and no error was raised for it (Requirements 6.2, 6.11).
    std::uint64_t total = 0;
    for (const AudioSubmission& block : state.audio) {
        EXPECT_TRUE(block.silent);
        EXPECT_EQ(block.sampleRate, AudioEngine::kOutputSampleRate);
        EXPECT_EQ(block.channels, AudioEngine::kOutputChannels);
        total += block.frames;
    }
    const Duration span = FrameRate::fps30().durationForFrames(8);
    EXPECT_EQ(total, durationToFrames(span, AudioEngine::kOutputSampleRate));
    EXPECT_EQ(result.value().audioFrames, total);
    EXPECT_TRUE(engine_->errors().empty());
}

TEST_F(ExportAudioEngineFixture, ATimelineWithNoAudioTrackAtAllStillYieldsSilence) {
    auto ctx = gpu::GpuContext::softwareFallback();
    auto compositor = makeCompositor(ctx);
    ExportEngine engine(*compositor, renderer());

    RecordingState state;
    Project project = videoProject(4); // video only: no audio-bearing track exists.
    auto result = engine.run(project, audioExportRequest(), recordingFactory(&state));
    ASSERT_TRUE(result.isOk());

    ASSERT_EQ(state.audio.size(), 4u);
    for (const AudioSubmission& block : state.audio) {
        EXPECT_TRUE(block.silent);
        EXPECT_GT(block.frames, 0u);
    }
}

TEST(ExportEngineAudioFallback, TheSilentRendererProducesExactlyTheRequestedLength) {
    // The fallback renderer used when nothing is injected: an export-local
    // AudioGraph mixing zero sources, which is what makes Requirement 6.11 hold
    // without an AudioEngine at all.
    AudioRangeRenderer silent = silentAudioRangeRenderer(AudioEngine::outputFormat());
    Project project = videoProject(2);

    const Duration from = Duration::fromMilliseconds(100);
    const Duration to = Duration::fromMilliseconds(600);
    auto buffer = silent(project, from, to);
    ASSERT_TRUE(buffer.isOk());
    EXPECT_EQ(buffer.value().sampleRate(), AudioEngine::kOutputSampleRate);
    EXPECT_EQ(buffer.value().channels(), AudioEngine::kOutputChannels);
    EXPECT_EQ(buffer.value().frameCount(),
              static_cast<std::size_t>(durationToFrames(to - from,
                                                        AudioEngine::kOutputSampleRate)));
    for (float sample : buffer.value().samples()) {
        EXPECT_EQ(sample, 0.0f);
    }

    // A zero-length range is legal and yields nothing; an inverted one is refused.
    auto empty = silent(project, from, from);
    ASSERT_TRUE(empty.isOk());
    EXPECT_EQ(empty.value().frameCount(), 0u);

    auto inverted = silent(project, to, from);
    ASSERT_TRUE(inverted.isError());
    EXPECT_EQ(inverted.error().code(), ErrorCode::InvalidArgument);
}

} // namespace
} // namespace palmier::media
