// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/media/audio_decode_property_test.cpp — Property 26 for the MediaDecoder
// audio surface (task 8.2 of the end-to-end-editor-integration spec;
// Requirement 6.1).
//
// Exactly one property lives here:
//
//   * Property 26 — decoded audio buffers conform to the declared ranges: every
//     buffer nextAudioFrame() yields declares a sample rate in 8 000-192 000 Hz,
//     a channel count in 1-8, and a presentation timestamp non-decreasing across
//     consecutive buffers of that stream; and hasAudio() agrees with the source's
//     stream layout.
//
// How the property is driven. MediaDecoder already owns an injectable decode
// seam — DecodeBackendFactory / IDecodeBackend — so no FFmpeg, no GPU and no
// media file on disk is needed. A synthetic backend overrides the (default,
// "no audio") decodeAudio()/seekAudio() and emits scripted blocks of interleaved
// float samples. That lets the generator explore the whole space the requirement
// talks about, including the cases a real fixture could not easily produce:
//
//   * sample rates spanning the declared range (8 000 and 192 000 included) AND
//     rates outside it (0, 4 000, 7 999, 192 001, 384 000), because "every buffer
//     conforms" is only meaningful if a non-conforming source is refused rather
//     than passed through;
//   * channel counts 1-8 as well as 0, 9 and 12;
//   * block sizes from a single sample frame to a maximal 4 096-frame block, and
//     block counts from zero (immediate end of stream) upward;
//   * stream layouts: audio at index 0, audio at a NON-zero index behind a video
//     stream, and an asset with no audio stream at all;
//   * a backend that deliberately regresses its presentation timestamps, which is
//     what proves the non-decreasing guarantee is enforced by the decoder rather
//     than merely inherited from a well-behaved backend.
//
// The assertions are made against the buffers the decoder actually EMITS. A
// generated source outside the declared ranges must therefore be refused (at
// openAudioStream when the stream declares its parameters, or at nextAudioFrame
// when only the produced buffer reveals them) — a refusal keeps the property
// true, silently emitting an out-of-range buffer breaks it.
//
// The unit tests below the property cover the example-based edges of the same
// task 8.1 surface: the default "no audio" backend implementation, index
// validation, hasAudio() on an audio-less source, and seekAudio() starting a new
// monotonic run.
//
// _Requirements: 6.1_

#include "media/MediaDecoder.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
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
#include "core/Result.hpp"
#include "media/AudioGraph.hpp"
#include "media/MediaInfo.hpp"

namespace palmier::media {
namespace {

// ---------------------------------------------------------------------------
// The generated source description
// ---------------------------------------------------------------------------

/// What the synthetic asset looks like and what its audio backend will emit.
struct AudioSourceSpec {
    // --- stream layout ---
    bool hasVideoStream = true;
    bool hasAudioStream = true;
    int  audioStreamIndex = 0;   ///< The index the audio stream carries.

    /// Whether MediaStreamInfo declares the audio parameters up front (a real
    /// container usually does) or leaves them unknown as 0 (raw/streamed inputs),
    /// in which case only the produced buffer reveals them.
    bool declaresParameters = true;

    // --- what the backend produces ---
    int                      sampleRate = 48'000;
    int                      channels = 2;
    std::vector<std::size_t> blockFrames{};  ///< One entry per emitted block.
    bool                     regressTimestamps = false;
};

// ---------------------------------------------------------------------------
// Synthetic decode backend
// ---------------------------------------------------------------------------

/// A decode backend with no video content and scripted audio. It overrides the
/// default "no audio" decodeAudio()/seekAudio(), which is also the seam this file
/// uses to prove those defaults exist (see the unit tests: a backend that does
/// NOT override them presents as audio-less and still compiles).
class SyntheticAudioBackend final : public IDecodeBackend {
public:
    explicit SyntheticAudioBackend(AudioSourceSpec spec) : spec_(std::move(spec)) {
        int nextIndex = 0;
        if (spec_.hasVideoStream) {
            MediaStreamInfo video;
            video.index = nextIndex++;
            video.type = MediaStreamType::Video;
            video.codec = MediaCodecId::H264;
            video.codecName = "h264";
            video.resolution = Resolution{64, 64};
            info_.streams.push_back(video);
        }
        if (spec_.hasAudioStream) {
            MediaStreamInfo audio;
            audio.index = spec_.audioStreamIndex;
            audio.type = MediaStreamType::Audio;
            audio.codec = MediaCodecId::AAC;
            audio.codecName = "aac";
            audio.sampleRate = spec_.declaresParameters ? spec_.sampleRate : 0;
            audio.channels = spec_.declaresParameters ? spec_.channels : 0;
            info_.streams.push_back(audio);
        }
        (void)nextIndex;
    }

    [[nodiscard]] const MediaInfo& info() const override { return info_; }

    // Video is irrelevant to Property 26; the source behaves as immediately
    // exhausted so the pure-virtual contract stays satisfied.
    [[nodiscard]] Result<BackendFrame> decode(bool /*useHardware*/) override {
        return BackendFrame::eos();
    }

    [[nodiscard]] Result<void> seek(Duration /*ts*/) override { return ok(); }

    [[nodiscard]] Result<BackendAudioFrame> decodeAudio(int streamIndex) override {
        requestedStreamIndices.push_back(streamIndex);

        if (nextBlock_ >= spec_.blockFrames.size()) {
            return BackendAudioFrame::eos();
        }
        const std::size_t frames = spec_.blockFrames[nextBlock_];
        const std::size_t channelCount =
            spec_.channels > 0 ? static_cast<std::size_t>(spec_.channels) : 0;

        // A deterministic, bounded waveform: the numeric content is irrelevant to
        // Property 26 (which is about the declared format), so a cheap ramp keeps
        // the generated cases fast.
        std::vector<float> samples(frames * channelCount, 0.0f);
        for (std::size_t i = 0; i < samples.size(); ++i) {
            samples[i] = static_cast<float>((i % 200) - 100) / 100.0f;
        }

        BackendAudioFrame out;
        out.buffer =
            AudioBuffer::interleaved(spec_.sampleRate, spec_.channels, std::move(samples));

        // Presentation time of the block's first sample, advancing by each block's
        // duration. When the spec asks for a regression, every other block is
        // pushed backwards behind its predecessor, so the decoder — not the
        // backend — has to make the emitted sequence non-decreasing.
        Duration pts = emitted_;
        if (spec_.regressTimestamps && (nextBlock_ % 2 == 1)) {
            pts = pts - Duration::fromMilliseconds(500);
        }
        out.timestamp = pts;

        if (spec_.sampleRate > 0) {
            emitted_ = emitted_ + Duration::fromSeconds(static_cast<double>(frames) /
                                                        static_cast<double>(spec_.sampleRate));
        }
        ++nextBlock_;
        return out;
    }

    [[nodiscard]] Result<void> seekAudio(Duration ts, int streamIndex) override {
        ++seekAudioCalls;
        lastSeekAudio = ts;
        lastSeekAudioStreamIndex = streamIndex;
        // Restart the script at the requested position so a post-seek run is a
        // fresh monotonic run, as a real demuxer would produce.
        nextBlock_ = 0;
        emitted_ = ts;
        return ok();
    }

    std::vector<int> requestedStreamIndices{};
    int              seekAudioCalls{0};
    Duration         lastSeekAudio{};
    int              lastSeekAudioStreamIndex{-1};

private:
    AudioSourceSpec spec_;
    MediaInfo       info_{};
    std::size_t     nextBlock_{0};
    Duration        emitted_{Duration::zero()};
};

/// A backend that does NOT override the audio entry points at all — the shape
/// every backend and test double in the tree had before task 8.1. It compiles
/// unchanged precisely because IDecodeBackend's audio methods are default
/// implemented, and it must present as decoding no audio.
class VideoOnlyBackend final : public IDecodeBackend {
public:
    VideoOnlyBackend() {
        MediaStreamInfo video;
        video.index = 0;
        video.type = MediaStreamType::Video;
        video.codec = MediaCodecId::H264;
        video.codecName = "h264";
        video.resolution = Resolution{32, 32};
        info_.streams.push_back(video);

        MediaStreamInfo audio;  // the container HAS audio; the backend cannot decode it
        audio.index = 1;
        audio.type = MediaStreamType::Audio;
        audio.codec = MediaCodecId::AAC;
        audio.codecName = "aac";
        audio.sampleRate = 44'100;
        audio.channels = 2;
        info_.streams.push_back(audio);
    }

    [[nodiscard]] const MediaInfo& info() const override { return info_; }
    [[nodiscard]] Result<BackendFrame> decode(bool /*useHardware*/) override {
        return BackendFrame::eos();
    }
    [[nodiscard]] Result<void> seek(Duration /*ts*/) override { return ok(); }

private:
    MediaInfo info_{};
};

// ---------------------------------------------------------------------------
// Factories
// ---------------------------------------------------------------------------

DecodeBackendFactory syntheticFactory(AudioSourceSpec spec, SyntheticAudioBackend** out) {
    return [spec, out](const std::filesystem::path&, const DecodePrefs&)
               -> Result<std::unique_ptr<IDecodeBackend>> {
        auto backend = std::make_unique<SyntheticAudioBackend>(spec);
        if (out != nullptr) *out = backend.get();
        return std::unique_ptr<IDecodeBackend>(std::move(backend));
    };
}

DecodeBackendFactory videoOnlyFactory() {
    return [](const std::filesystem::path&, const DecodePrefs&)
               -> Result<std::unique_ptr<IDecodeBackend>> {
        return std::unique_ptr<IDecodeBackend>(std::make_unique<VideoOnlyBackend>());
    };
}

MediaDecoder openWith(const DecodeBackendFactory& factory) {
    DecodePrefs prefs;
    prefs.preferHardware = false;
    auto opened = MediaDecoder::open("synthetic-audio.mp4", prefs, factory);
    RC_ASSERT(opened.isOk());
    return std::move(opened).value();
}

// ---------------------------------------------------------------------------
// Generators — constrained to the space Requirement 6.1 talks about, plus the
// out-of-range neighbourhood that gives "conforms" its meaning.
// ---------------------------------------------------------------------------

/// Rates inside the declared range, including both endpoints.
rc::Gen<int> genConformingRate() {
    return rc::gen::element(kMinAudioSampleRate, 11'025, 16'000, 22'050, 32'000, 44'100, 48'000,
                            88'200, 96'000, kMaxAudioSampleRate);
}

/// Rates outside the declared range, straddling both endpoints.
rc::Gen<int> genNonConformingRate() {
    return rc::gen::element(0, 1, 4'000, kMinAudioSampleRate - 1, kMaxAudioSampleRate + 1,
                            384'000);
}

rc::Gen<int> genConformingChannels() {
    return rc::gen::inRange<int>(kMinAudioChannels, kMaxAudioChannels + 1);
}

rc::Gen<int> genNonConformingChannels() {
    return rc::gen::element(0, kMaxAudioChannels + 1, 12, 16);
}

AudioSourceSpec genSpec() {
    AudioSourceSpec spec;

    spec.hasAudioStream = *rc::gen::weightedElement<bool>({{4, true}, {1, false}});
    spec.hasVideoStream = *rc::gen::element(true, false);
    // A source with neither stream kind is not an asset the importer would accept;
    // keep at least one elementary stream present.
    if (!spec.hasAudioStream) spec.hasVideoStream = true;

    // The audio stream index is NOT assumed to be 0: it sits after the video
    // stream when one is present, and the generator also explores indices well
    // past the stream count's natural ordering.
    spec.audioStreamIndex =
        *rc::gen::element(0, 1, 2, 3, 7);
    if (spec.hasVideoStream && spec.audioStreamIndex == 0) spec.audioStreamIndex = 1;

    spec.declaresParameters = *rc::gen::weightedElement<bool>({{3, true}, {1, false}});

    // Mostly conforming formats — with a meaningful minority outside the declared
    // ranges, which the decoder must refuse rather than emit.
    spec.sampleRate = *rc::gen::weightedOneOf<int>(
        {{4, genConformingRate()}, {1, genNonConformingRate()}});
    spec.channels = *rc::gen::weightedOneOf<int>(
        {{4, genConformingChannels()}, {1, genNonConformingChannels()}});

    // Block sizes from a single sample frame to a maximal block, and block counts
    // including zero (a stream that is immediately exhausted).
    const std::size_t blockCount = *rc::gen::inRange<std::size_t>(0, 9);
    spec.blockFrames.reserve(blockCount);
    for (std::size_t i = 0; i < blockCount; ++i) {
        spec.blockFrames.push_back(*rc::gen::element<std::size_t>(
            std::size_t{1}, std::size_t{2}, std::size_t{64}, std::size_t{512}, std::size_t{1'024},
            std::size_t{4'096}, *rc::gen::inRange<std::size_t>(1, 4'097)));
    }

    spec.regressTimestamps = *rc::gen::element(true, false);
    return spec;
}

// ---------------------------------------------------------------------------
// Property 26
// ---------------------------------------------------------------------------

// Feature: end-to-end-editor-integration, Property 26: Decoded audio buffers
// conform to the declared ranges — for any audio stream the decoder opens, every
// buffer it yields declares a sample rate in 8 000-192 000 samples per second, a
// channel count in 1-8, and a presentation timestamp non-decreasing across
// consecutive buffers of that stream.
//
// **Validates: Requirements 6.1**
RC_GTEST_PROP(AudioDecodeProperties, DecodedBuffersConformToDeclaredRanges, ()) {
    const AudioSourceSpec spec = genSpec();

    SyntheticAudioBackend* backend = nullptr;
    MediaDecoder decoder = openWith(syntheticFactory(spec, &backend));
    RC_ASSERT(backend != nullptr);

    // (a) hasAudio() agrees with the stream layout, for every layout — including
    //     an asset with no audio stream at all.
    RC_ASSERT(decoder.hasAudio() == spec.hasAudioStream);

    if (!spec.hasAudioStream) {
        // Nothing to open, so nothing can be emitted out of range.
        RC_ASSERT(decoder.openAudioStream().isError());
        RC_ASSERT(decoder.openAudioStream(spec.audioStreamIndex).isError());
        RC_ASSERT(decoder.nextAudioFrame().isError());
        RC_ASSERT(decoder.audioStreamIndex() < 0);
        return;
    }

    // (b) Both selection spellings resolve to the same stream: -1 for "the
    //     primary audio stream", and the explicit index — which is NOT assumed
    //     to be 0.
    const Result<void> openedPrimary = decoder.openAudioStream();
    const Result<void> openedExplicit = decoder.openAudioStream(spec.audioStreamIndex);
    RC_ASSERT(openedPrimary.isOk() == openedExplicit.isOk());

    if (openedPrimary.isOk()) {
        RC_ASSERT(decoder.audioStreamIndex() == spec.audioStreamIndex);
    } else {
        // The only reason a present audio stream is refused is a declared format
        // outside the ranges Requirement 6.1 fixes.
        RC_ASSERT(spec.declaresParameters);
        RC_ASSERT(!isDeclaredAudioSampleRate(spec.sampleRate) ||
                  !isDeclaredAudioChannelCount(spec.channels));
        RC_ASSERT(decoder.audioStreamIndex() < 0);
        RC_ASSERT(decoder.nextAudioFrame().isError());
        return;
    }

    // A non-existent index is still rejected, whatever the layout.
    RC_ASSERT(decoder.openAudioStream(9'999).isError());
    RC_ASSERT(decoder.openAudioStream(spec.audioStreamIndex).isOk());

    // (c) Drain the stream and check every EMITTED buffer.
    std::optional<Duration> previous;
    std::size_t             emitted = 0;
    bool                    sawEndOfStream = false;
    bool                    refused = false;

    for (std::size_t step = 0; step <= spec.blockFrames.size() + 1; ++step) {
        Result<AudioFrame> next = decoder.nextAudioFrame();
        if (next.isError()) {
            // A refusal is a valid outcome: it is how a source whose produced
            // buffers fall outside the declared ranges is prevented from ever
            // yielding one.
            refused = true;
            break;
        }
        const AudioFrame frame = std::move(next).value();
        if (frame.endOfStream) {
            sawEndOfStream = true;
            break;
        }

        ++emitted;

        // Declared sample rate inside 8 000-192 000 Hz.
        RC_ASSERT(frame.sampleRate() >= kMinAudioSampleRate);
        RC_ASSERT(frame.sampleRate() <= kMaxAudioSampleRate);
        // Declared channel count inside 1-8.
        RC_ASSERT(frame.channels() >= kMinAudioChannels);
        RC_ASSERT(frame.channels() <= kMaxAudioChannels);
        // The buffer's payload is consistent with what it declares: an
        // interleaved float buffer of whole frames.
        RC_ASSERT(frame.buffer.samples().size() ==
                  frame.frameCount() * static_cast<std::size_t>(frame.channels()));
        // Presentation timestamps are non-decreasing across consecutive buffers
        // of this stream, even though the backend may have regressed them.
        if (previous.has_value()) {
            RC_ASSERT(frame.presentation >= *previous);
        }
        previous = frame.presentation;
    }

    // A conforming source is fully drained rather than refused, and yields
    // exactly the blocks its stream carried.
    if (isDeclaredAudioSampleRate(spec.sampleRate) &&
        isDeclaredAudioChannelCount(spec.channels)) {
        RC_ASSERT(!refused);
        RC_ASSERT(sawEndOfStream);
        RC_ASSERT(emitted == spec.blockFrames.size());
    } else {
        // A non-conforming source emitted nothing at all.
        RC_ASSERT(emitted == 0);
        RC_ASSERT(refused || spec.blockFrames.empty());
    }

    // Every decode request named the stream the caller selected.
    for (const int requested : backend->requestedStreamIndices) {
        RC_ASSERT(requested == spec.audioStreamIndex);
    }
}

// ---------------------------------------------------------------------------
// Unit tests — the example-based edges of the same surface (task 8.1)
// ---------------------------------------------------------------------------

AudioSourceSpec conformingSpec() {
    AudioSourceSpec spec;
    spec.hasVideoStream = true;
    spec.hasAudioStream = true;
    spec.audioStreamIndex = 1;
    spec.sampleRate = 44'100;
    spec.channels = 2;
    spec.blockFrames = {1'024, 1'024, 1'024};
    return spec;
}

MediaDecoder openForTest(const DecodeBackendFactory& factory) {
    DecodePrefs prefs;
    prefs.preferHardware = false;
    auto opened = MediaDecoder::open("synthetic-audio.mp4", prefs, factory);
    EXPECT_TRUE(opened.isOk());
    return std::move(opened).value();
}

TEST(MediaDecoderAudio, DefaultBackendImplementationReportsNoAudio) {
    // The container advertises audio, so hasAudio() and openAudioStream() succeed
    // — but the backend never overrode decodeAudio(), so the DEFAULT
    // implementation answers, and it must report that no audio is decodable
    // rather than fail to compile or return a bogus buffer.
    MediaDecoder decoder = openForTest(videoOnlyFactory());
    EXPECT_TRUE(decoder.hasAudio());
    ASSERT_TRUE(decoder.openAudioStream().isOk());

    Result<AudioFrame> frame = decoder.nextAudioFrame();
    ASSERT_TRUE(frame.isError());
    EXPECT_EQ(frame.error().code(), ErrorCode::Unsupported);

    Result<void> sought = decoder.seekAudio(Duration::fromMilliseconds(250));
    ASSERT_TRUE(sought.isError());
    EXPECT_EQ(sought.error().code(), ErrorCode::Unsupported);
}

TEST(MediaDecoderAudio, AudioLessSourceHasNoAudioAndRefusesToOpen) {
    AudioSourceSpec spec = conformingSpec();
    spec.hasAudioStream = false;

    MediaDecoder decoder = openForTest(syntheticFactory(spec, nullptr));
    EXPECT_FALSE(decoder.hasAudio());

    Result<void> opened = decoder.openAudioStream();
    ASSERT_TRUE(opened.isError());
    EXPECT_EQ(opened.error().code(), ErrorCode::FailedPrecondition);
    EXPECT_EQ(decoder.audioStreamIndex(), -1);
}

TEST(MediaDecoderAudio, PrimaryStreamSelectionFindsANonZeroAudioIndex) {
    AudioSourceSpec spec = conformingSpec();
    spec.audioStreamIndex = 3;

    SyntheticAudioBackend* backend = nullptr;
    MediaDecoder decoder = openForTest(syntheticFactory(spec, &backend));
    ASSERT_TRUE(decoder.openAudioStream().isOk());
    EXPECT_EQ(decoder.audioStreamIndex(), 3);

    Result<AudioFrame> frame = decoder.nextAudioFrame();
    ASSERT_TRUE(frame.isOk());
    const AudioFrame decoded = std::move(frame).value();
    EXPECT_EQ(decoded.sampleRate(), 44'100);
    EXPECT_EQ(decoded.channels(), 2);
    EXPECT_EQ(decoded.frameCount(), 1'024u);
    ASSERT_FALSE(backend->requestedStreamIndices.empty());
    EXPECT_EQ(backend->requestedStreamIndices.front(), 3);
}

TEST(MediaDecoderAudio, NonAudioAndUnknownIndicesAreRejectedByName) {
    AudioSourceSpec spec = conformingSpec();
    MediaDecoder decoder = openForTest(syntheticFactory(spec, nullptr));

    Result<void> video = decoder.openAudioStream(0);  // the video stream
    ASSERT_TRUE(video.isError());
    EXPECT_EQ(video.error().code(), ErrorCode::InvalidArgument);
    EXPECT_NE(video.error().message().find('0'), std::string::npos);

    Result<void> missing = decoder.openAudioStream(42);
    ASSERT_TRUE(missing.isError());
    EXPECT_EQ(missing.error().code(), ErrorCode::InvalidArgument);
    EXPECT_NE(missing.error().message().find("42"), std::string::npos);
}

TEST(MediaDecoderAudio, OutOfRangeDeclaredFormatIsRefusedNamingTheValue) {
    AudioSourceSpec tooFast = conformingSpec();
    tooFast.sampleRate = 384'000;
    MediaDecoder fastDecoder = openForTest(syntheticFactory(tooFast, nullptr));
    Result<void> fast = fastDecoder.openAudioStream();
    ASSERT_TRUE(fast.isError());
    EXPECT_EQ(fast.error().code(), ErrorCode::Unsupported);
    EXPECT_NE(fast.error().message().find("384000"), std::string::npos);

    AudioSourceSpec tooManyChannels = conformingSpec();
    tooManyChannels.channels = 12;
    MediaDecoder wideDecoder = openForTest(syntheticFactory(tooManyChannels, nullptr));
    Result<void> wide = wideDecoder.openAudioStream();
    ASSERT_TRUE(wide.isError());
    EXPECT_EQ(wide.error().code(), ErrorCode::Unsupported);
    EXPECT_NE(wide.error().message().find("12"), std::string::npos);
}

TEST(MediaDecoderAudio, UndeclaredButOutOfRangeBufferIsRejectedAtDecodeTime) {
    // A stream that declares nothing (sampleRate/channels 0, as a raw input does)
    // passes openAudioStream; the per-buffer check is then the enforcement point.
    AudioSourceSpec spec = conformingSpec();
    spec.declaresParameters = false;
    spec.sampleRate = 4'000;

    MediaDecoder decoder = openForTest(syntheticFactory(spec, nullptr));
    ASSERT_TRUE(decoder.openAudioStream().isOk());

    Result<AudioFrame> frame = decoder.nextAudioFrame();
    ASSERT_TRUE(frame.isError());
    EXPECT_EQ(frame.error().code(), ErrorCode::Unsupported);
    EXPECT_NE(frame.error().message().find("4000"), std::string::npos);
}

TEST(MediaDecoderAudio, NextAudioFrameRequiresAnOpenStream) {
    AudioSourceSpec spec = conformingSpec();
    MediaDecoder decoder = openForTest(syntheticFactory(spec, nullptr));

    Result<AudioFrame> frame = decoder.nextAudioFrame();
    ASSERT_TRUE(frame.isError());
    EXPECT_EQ(frame.error().code(), ErrorCode::FailedPrecondition);

    Result<void> sought = decoder.seekAudio(Duration::zero());
    ASSERT_TRUE(sought.isError());
    EXPECT_EQ(sought.error().code(), ErrorCode::FailedPrecondition);
}

TEST(MediaDecoderAudio, RegressingBackendTimestampsAreRaisedToNonDecreasing) {
    AudioSourceSpec spec = conformingSpec();
    spec.blockFrames = {512, 512, 512, 512};
    spec.regressTimestamps = true;

    MediaDecoder decoder = openForTest(syntheticFactory(spec, nullptr));
    ASSERT_TRUE(decoder.openAudioStream().isOk());

    std::vector<Duration> presented;
    for (int i = 0; i < 4; ++i) {
        Result<AudioFrame> frame = decoder.nextAudioFrame();
        ASSERT_TRUE(frame.isOk());
        const AudioFrame decoded = std::move(frame).value();
        ASSERT_FALSE(decoded.endOfStream);
        presented.push_back(decoded.presentation);
    }
    for (std::size_t i = 1; i < presented.size(); ++i) {
        EXPECT_GE(presented[i], presented[i - 1]);
    }
}

TEST(MediaDecoderAudio, SeekAudioForwardsToTheBackendAndStartsANewRun) {
    AudioSourceSpec spec = conformingSpec();
    spec.blockFrames = {1'024, 1'024, 1'024};

    SyntheticAudioBackend* backend = nullptr;
    MediaDecoder decoder = openForTest(syntheticFactory(spec, &backend));
    ASSERT_TRUE(decoder.openAudioStream().isOk());

    Result<AudioFrame> first = decoder.nextAudioFrame();
    ASSERT_TRUE(first.isOk());
    const Duration firstAt = std::move(first).value().presentation;

    Result<AudioFrame> second = decoder.nextAudioFrame();
    ASSERT_TRUE(second.isOk());
    const Duration secondAt = std::move(second).value().presentation;
    EXPECT_GE(secondAt, firstAt);

    // Seek backwards: the new run legitimately starts before the previous
    // position, so the monotonic baseline must have been cleared rather than
    // clamping the post-seek frame forward.
    const Duration target = Duration::zero();
    ASSERT_TRUE(decoder.seekAudio(target).isOk());
    EXPECT_EQ(backend->seekAudioCalls, 1);
    EXPECT_EQ(backend->lastSeekAudio, target);
    EXPECT_EQ(backend->lastSeekAudioStreamIndex, spec.audioStreamIndex);

    Result<AudioFrame> afterSeek = decoder.nextAudioFrame();
    ASSERT_TRUE(afterSeek.isOk());
    const AudioFrame resumed = std::move(afterSeek).value();
    EXPECT_EQ(resumed.presentation, target);
    EXPECT_EQ(resumed.sampleRate(), 44'100);
}

TEST(MediaDecoderAudio, StreamIsDrainedToEndOfStream) {
    AudioSourceSpec spec = conformingSpec();
    spec.blockFrames = {1, 4'096};

    MediaDecoder decoder = openForTest(syntheticFactory(spec, nullptr));
    ASSERT_TRUE(decoder.openAudioStream().isOk());

    std::size_t emitted = 0;
    for (int i = 0; i < 8; ++i) {
        Result<AudioFrame> frame = decoder.nextAudioFrame();
        ASSERT_TRUE(frame.isOk());
        const AudioFrame decoded = std::move(frame).value();
        if (decoded.endOfStream) break;
        ++emitted;
        EXPECT_EQ(decoded.channels(), 2);
    }
    EXPECT_EQ(emitted, 2u);
}

} // namespace
} // namespace palmier::media
