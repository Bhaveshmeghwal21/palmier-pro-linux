// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/media_peak_envelope_extractor_test.cpp — tests for decoder-driven
// envelope extraction (monitoring-and-grading Requirement 2.1, 2.6, 2.7, 2.8).
//
// The distinction under test is the one the signature exists to enforce: THREE
// outcomes, of which only one is an error.
//
//   * an envelope, for an asset that carries audio;
//   * an EMPTY envelope reported as SUCCESS, for an asset with no audio stream or
//     a stream that yields nothing (Requirement 2.6 — draw nothing, report
//     nothing);
//   * an error, only for a file that would not open, a stream that was refused, or
//     a decode that failed (Requirement 2.7 — reported once).
//
// Reporting a silent asset as a failure would put an error in front of the user
// for a video with no soundtrack, so "silent is not broken" is asserted directly
// and repeatedly rather than left to the implementation's shape.
//
// Everything runs through MediaDecoder's DecodeBackendFactory seam with a
// synthetic backend: no FFmpeg, no media file, no device.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/Duration.hpp"
#include "core/Error.hpp"
#include "core/Result.hpp"
#include "media/MediaDecoder.hpp"
#include "media/MediaInfo.hpp"
#include "media/PeakEnvelope.hpp"
#include "media/PeakEnvelopeExtractor.hpp"

namespace palmier::media {
namespace {

constexpr int         kRate = 48'000;
const Duration        kBucket = Duration::fromMilliseconds(1);
constexpr std::size_t kFramesPerBucket = 48;

/// What the synthetic source claims and produces.
struct SourceSpec {
    bool                     hasAudioStream = true;
    bool                     declaresParameters = true;
    int                      sampleRate = kRate;
    int                      channels = 1;
    std::vector<std::size_t> blockFrames{};
    /// Constant amplitude each block emits, so expected buckets are exact.
    std::vector<float>       blockValues{};
    /// Fail decodeAudio() on this 0-based block (-1: never).
    int                      failOnBlock = -1;
    /// Refuse openAudioStream() by declaring an out-of-range sample rate.
    bool                     declareUnsupportedRate = false;
    /// Never report end of stream — used to prove the read is bounded.
    bool                     endless = false;
};

class SyntheticBackend final : public IDecodeBackend {
public:
    explicit SyntheticBackend(SourceSpec spec) : spec_(std::move(spec)) {
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
            if (spec_.declareUnsupportedRate) {
                audio.sampleRate = 3;  // outside the accepted 8000-192000 Hz range
                audio.channels = spec_.channels;
            } else if (spec_.declaresParameters) {
                audio.sampleRate = spec_.sampleRate;
                audio.channels = spec_.channels;
            }
            info_.streams.push_back(audio);
        }
    }

    [[nodiscard]] const MediaInfo& info() const override { return info_; }

    [[nodiscard]] Result<BackendFrame> decode(bool /*useHardware*/) override {
        return BackendFrame::eos();
    }

    [[nodiscard]] Result<void> seek(Duration /*ts*/) override { return ok(); }

    [[nodiscard]] Result<BackendAudioFrame> decodeAudio(int /*streamIndex*/) override {
        const auto index = static_cast<int>(block_);
        if (spec_.failOnBlock == index) {
            return err<BackendAudioFrame>(
                makeError(ErrorCode::Io, "synthetic audio decode failure"));
        }
        if (block_ >= spec_.blockFrames.size()) {
            if (spec_.endless) {
                // Never EOS: keeps emitting real blocks forever. Exactly the shape
                // that would spin a worker thread indefinitely without a bounded
                // read, which is why the limits exist.
                const auto channels = static_cast<std::size_t>(spec_.channels);
                BackendAudioFrame out;
                out.buffer = AudioBuffer::interleaved(
                    spec_.sampleRate, spec_.channels,
                    std::vector<float>(kFramesPerBucket * channels, 0.5f));
                out.timestamp = emitted_;
                emitted_ += frameToSourceTime(static_cast<std::int64_t>(kFramesPerBucket),
                                              spec_.sampleRate);
                ++block_;
                return out;
            }
            return BackendAudioFrame::eos();
        }

        const std::size_t frames = spec_.blockFrames[block_];
        const float       value =
            block_ < spec_.blockValues.size() ? spec_.blockValues[block_] : 0.0f;
        const auto channels = static_cast<std::size_t>(spec_.channels);

        BackendAudioFrame out;
        out.buffer = AudioBuffer::interleaved(spec_.sampleRate, spec_.channels,
                                              std::vector<float>(frames * channels, value));
        out.timestamp = emitted_;
        emitted_ += frameToSourceTime(static_cast<std::int64_t>(frames), spec_.sampleRate);
        ++block_;
        return out;
    }

    [[nodiscard]] Result<void> seekAudio(Duration /*ts*/, int /*streamIndex*/) override {
        return ok();
    }

private:
    SourceSpec  spec_;
    MediaInfo   info_{};
    std::size_t block_ = 0;
    Duration    emitted_{};
};

[[nodiscard]] DecodeBackendFactory factoryFor(SourceSpec spec) {
    return [spec](const std::filesystem::path&,
                  const DecodePrefs&) -> Result<std::unique_ptr<IDecodeBackend>> {
        return std::unique_ptr<IDecodeBackend>(new SyntheticBackend(spec));
    };
}

[[nodiscard]] Result<PeakEnvelope> extract(SourceSpec spec,
                                           EnvelopeExtractionLimits limits = {}) {
    return extractPeakEnvelope("synthetic.mov", kBucket, DecodePrefs{}, factoryFor(std::move(spec)),
                               limits);
}

// ---------------------------------------------------------------------------
// The success path
// ---------------------------------------------------------------------------

TEST(ExtractPeakEnvelope, ReadsTheWholeStreamAndBucketsItByItsSourceTime) {
    SourceSpec spec;
    spec.blockFrames = {kFramesPerBucket, kFramesPerBucket, kFramesPerBucket};
    spec.blockValues = {1.0f, -0.5f, 0.25f};

    auto result = extract(spec);
    ASSERT_TRUE(result.isOk()) << "a decodable audio asset must yield an envelope";
    const PeakEnvelope envelope = std::move(result).value();

    ASSERT_EQ(envelope.buckets.size(), 3u);
    EXPECT_EQ(envelope.sampleRate, kRate);
    EXPECT_EQ(envelope.bucketDuration, kBucket);
    EXPECT_FLOAT_EQ(envelope.buckets[0].max, 1.0f);
    EXPECT_FLOAT_EQ(envelope.buckets[1].min, -0.5f);
    EXPECT_FLOAT_EQ(envelope.buckets[2].max, 0.25f);
    EXPECT_EQ(envelope.duration(), Duration::fromMilliseconds(3));
}

TEST(ExtractPeakEnvelope, RaggedDecoderBlocksDoNotMoveBucketBoundaries) {
    // Requirement 2.8's practical consequence: the drawn shape is a function of
    // the asset, not of how the container happened to chunk it. Same 96 frames of
    // content, two very different block layouts.
    SourceSpec even;
    even.blockFrames = {kFramesPerBucket, kFramesPerBucket};
    even.blockValues = {1.0f, -1.0f};

    SourceSpec ragged;
    ragged.blockFrames = {10, 20, 18, 7, 41};
    ragged.blockValues = {1.0f, 1.0f, 1.0f, -1.0f, -1.0f};

    auto a = extract(even);
    auto b = extract(ragged);
    ASSERT_TRUE(a.isOk());
    ASSERT_TRUE(b.isOk());
    const PeakEnvelope first = std::move(a).value();
    const PeakEnvelope second = std::move(b).value();

    ASSERT_EQ(first.buckets.size(), 2u);
    ASSERT_EQ(second.buckets.size(), 2u);
    EXPECT_FLOAT_EQ(first.buckets[0].max, 1.0f);
    EXPECT_FLOAT_EQ(second.buckets[0].max, 1.0f);
    EXPECT_FLOAT_EQ(first.buckets[1].min, -1.0f);
    EXPECT_FLOAT_EQ(second.buckets[1].min, -1.0f);
}

TEST(ExtractPeakEnvelope, AdoptsTheSampleRateFromTheFirstBlockWhenTheContainerDeclaresNone) {
    // Raw and streamed inputs declare nothing; the format only appears in the
    // first decoded block. Handled the same way AudioEngine handles it.
    SourceSpec spec;
    spec.declaresParameters = false;
    spec.blockFrames = {kFramesPerBucket * 2};
    spec.blockValues = {0.75f};

    auto result = extract(spec);
    ASSERT_TRUE(result.isOk());
    const PeakEnvelope envelope = std::move(result).value();
    EXPECT_EQ(envelope.sampleRate, kRate);
    ASSERT_EQ(envelope.buckets.size(), 2u);
    EXPECT_FLOAT_EQ(envelope.buckets[0].max, 0.75f);
}

TEST(ExtractPeakEnvelope, HullsChannelsTogetherForAMultiChannelAsset) {
    SourceSpec spec;
    spec.channels = 2;
    spec.blockFrames = {kFramesPerBucket};
    spec.blockValues = {0.6f};  // both channels carry the same constant here

    auto result = extract(spec);
    ASSERT_TRUE(result.isOk());
    const PeakEnvelope envelope = std::move(result).value();
    ASSERT_EQ(envelope.buckets.size(), 1u);
    EXPECT_FLOAT_EQ(envelope.buckets[0].max, 0.6f);
    EXPECT_FLOAT_EQ(envelope.buckets[0].min, 0.6f);
}

// ---------------------------------------------------------------------------
// Silent, not failed (Requirement 2.6)
// ---------------------------------------------------------------------------

TEST(ExtractPeakEnvelope, AnAssetWithNoAudioStreamSucceedsWithAnEmptyEnvelope) {
    SourceSpec spec;
    spec.hasAudioStream = false;

    auto result = extract(spec);
    ASSERT_TRUE(result.isOk()) << "an asset with no soundtrack is silent, not broken";
    EXPECT_TRUE(std::move(result).value().empty());
}

TEST(ExtractPeakEnvelope, AnAudioStreamThatYieldsNothingSucceedsWithAnEmptyEnvelope) {
    // Declared audio, immediately exhausted. The same conclusion AudioEngine
    // reaches for the same situation.
    SourceSpec spec;
    spec.blockFrames = {};

    auto result = extract(spec);
    ASSERT_TRUE(result.isOk());
    EXPECT_TRUE(std::move(result).value().empty());
}

// ---------------------------------------------------------------------------
// Genuine failures (Requirement 2.7)
// ---------------------------------------------------------------------------

TEST(ExtractPeakEnvelope, AFileThatWillNotOpenIsReportedAsAnError) {
    const DecodeBackendFactory refusing =
        [](const std::filesystem::path&,
           const DecodePrefs&) -> Result<std::unique_ptr<IDecodeBackend>> {
        return err<std::unique_ptr<IDecodeBackend>>(
            makeError(ErrorCode::NotFound, "no such file"));
    };

    auto result = extractPeakEnvelope("missing.mov", kBucket, DecodePrefs{}, refusing);
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::NotFound);
}

TEST(ExtractPeakEnvelope, ARefusedAudioStreamIsReportedRatherThanDrawnAsSilence) {
    // The asset claims audio this build will not decode. Saying so once is more
    // useful than silently drawing nothing.
    SourceSpec spec;
    spec.declareUnsupportedRate = true;
    spec.blockFrames = {kFramesPerBucket};
    spec.blockValues = {1.0f};

    auto result = extract(spec);
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::Unsupported);
}

TEST(ExtractPeakEnvelope, ADecodeFailureMidStreamIsReportedAndNotPartiallyReturned) {
    SourceSpec spec;
    spec.blockFrames = {kFramesPerBucket, kFramesPerBucket, kFramesPerBucket};
    spec.blockValues = {1.0f, 1.0f, 1.0f};
    spec.failOnBlock = 1;

    auto result = extract(spec);
    ASSERT_TRUE(result.isError()) << "a partial envelope must not masquerade as a whole one";
    EXPECT_EQ(result.error().code(), ErrorCode::Io);
}

TEST(ExtractPeakEnvelope, ANonPositiveBucketDurationIsRejectedBeforeAnythingIsOpened) {
    SourceSpec spec;
    spec.blockFrames = {kFramesPerBucket};
    spec.blockValues = {1.0f};

    auto result = extractPeakEnvelope("synthetic.mov", Duration::zero(), DecodePrefs{},
                                      factoryFor(spec));
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

// ---------------------------------------------------------------------------
// The read is bounded
// ---------------------------------------------------------------------------

TEST(ExtractPeakEnvelope, ABackendThatNeverEndsIsBoundedRatherThanSpinningForever) {
    // A decoder that never reports end-of-stream would hang a worker thread. The
    // limit converts that into an ordinary reported failure. Without the bound
    // this case does not fail — it never returns, which is why it is worth a test.
    SourceSpec spec;
    spec.endless = true;
    spec.blockFrames = {};

    EnvelopeExtractionLimits limits;
    limits.maxBlocks = 64;

    auto result = extract(spec, limits);
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::OutOfRange);
}

TEST(ExtractPeakEnvelope, TheFrameLimitStopsAnUnreasonablyLongRead) {
    SourceSpec spec;
    spec.blockFrames = {kFramesPerBucket, kFramesPerBucket, kFramesPerBucket};
    spec.blockValues = {1.0f, 1.0f, 1.0f};

    EnvelopeExtractionLimits limits;
    limits.maxFrames = kFramesPerBucket;  // one bucket's worth

    auto result = extract(spec, limits);
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::OutOfRange);
}

} // namespace
} // namespace palmier::media
