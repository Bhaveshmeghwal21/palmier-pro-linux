// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/media_audio_graph_test.cpp — unit tests for the audio graph (task 8.4;
// Requirement 2.8).
//
// These exercise the FFmpeg-free numeric core of the audio graph: buffer/rate
// math, channel up/down-mix, the linear resampler, multi-source mixing
// (sum + clamp with per-source gain), PCM pack/unpack, and the AudioGraph
// orchestration. They run without FFmpeg, a GPU, or any vendor SDK — the
// libswresample backend, when compiled in, satisfies the same IResampler seam
// and is transparently substitutable.

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "core/Error.hpp"
#include "media/AudioGraph.hpp"

namespace palmier::media {
namespace {

// --- AudioFormat / sample math ---------------------------------------------

TEST(AudioFormat, ValidityAndFrameSize) {
    AudioFormat f{48000, 2, SampleFormat::S16};
    EXPECT_TRUE(f.isValid());
    EXPECT_EQ(f.bytesPerFrame(), 4); // 2 channels * 2 bytes

    EXPECT_FALSE((AudioFormat{0, 2, SampleFormat::F32}).isValid());
    EXPECT_FALSE((AudioFormat{48000, 0, SampleFormat::F32}).isValid());
    EXPECT_EQ((AudioFormat{44100, 6, SampleFormat::F32}).bytesPerFrame(), 24);
}

TEST(ResampledFrameCount, ScalesByRateRatioRoundingToNearest) {
    EXPECT_EQ(resampledFrameCount(100, 48000, 48000), 100u); // identity
    EXPECT_EQ(resampledFrameCount(100, 48000, 24000), 50u);  // halve
    EXPECT_EQ(resampledFrameCount(100, 24000, 48000), 200u); // double
    // 441 @ 44.1k -> 480 @ 48k (exactly 10ms both ways).
    EXPECT_EQ(resampledFrameCount(441, 44100, 48000), 480u);
    // Empty / invalid inputs yield zero.
    EXPECT_EQ(resampledFrameCount(0, 48000, 48000), 0u);
    EXPECT_EQ(resampledFrameCount(100, 0, 48000), 0u);
    EXPECT_EQ(resampledFrameCount(100, 48000, -1), 0u);
}

// --- AudioBuffer ------------------------------------------------------------

TEST(AudioBuffer, FrameCountAndBoundsSafeAccess) {
    AudioBuffer b = AudioBuffer::interleaved(48000, 2, {0.1f, 0.2f, 0.3f, 0.4f});
    EXPECT_EQ(b.frameCount(), 2u);
    EXPECT_FLOAT_EQ(b.at(0, 0), 0.1f);
    EXPECT_FLOAT_EQ(b.at(1, 1), 0.4f);
    // Out-of-range reads yield silence.
    EXPECT_FLOAT_EQ(b.at(5, 0), 0.0f);
    EXPECT_FLOAT_EQ(b.at(0, 3), 0.0f);

    AudioBuffer zeros(48000, 2, 4);
    EXPECT_EQ(zeros.frameCount(), 4u);
    EXPECT_FLOAT_EQ(zeros.at(3, 1), 0.0f);
}

// --- Mixing -----------------------------------------------------------------

TEST(Mix, SumsSourcesWithGain) {
    AudioBuffer a = AudioBuffer::interleaved(48000, 1, {0.2f, 0.2f, 0.2f});
    AudioBuffer b = AudioBuffer::interleaved(48000, 1, {0.1f, 0.1f, 0.1f});

    auto mixed = mix(48000, 1, 3, {{&a, 1.0}, {&b, 2.0}});
    ASSERT_TRUE(mixed.isOk());
    const AudioBuffer& out = mixed.value();
    ASSERT_EQ(out.frameCount(), 3u);
    // 0.2*1.0 + 0.1*2.0 = 0.4
    for (std::size_t i = 0; i < 3; ++i) EXPECT_FLOAT_EQ(out.at(i, 0), 0.4f);
}

TEST(Mix, ClampsToUnitRange) {
    AudioBuffer a = AudioBuffer::interleaved(48000, 1, {0.8f, -0.8f});
    AudioBuffer b = AudioBuffer::interleaved(48000, 1, {0.8f, -0.8f});

    auto mixed = mix(48000, 1, 2, {{&a, 1.0}, {&b, 1.0}});
    ASSERT_TRUE(mixed.isOk());
    // 0.8 + 0.8 = 1.6 -> clamped to 1.0; -1.6 -> -1.0.
    EXPECT_FLOAT_EQ(mixed.value().at(0, 0), 1.0f);
    EXPECT_FLOAT_EQ(mixed.value().at(1, 0), -1.0f);
}

TEST(Mix, ShorterSourceContributesSilencePastItsEnd) {
    AudioBuffer full = AudioBuffer::interleaved(48000, 1, {0.5f, 0.5f, 0.5f, 0.5f});
    AudioBuffer shortB = AudioBuffer::interleaved(48000, 1, {0.5f, 0.5f});

    auto mixed = mix(48000, 1, 4, {{&full, 1.0}, {&shortB, 1.0}});
    ASSERT_TRUE(mixed.isOk());
    EXPECT_FLOAT_EQ(mixed.value().at(0, 0), 1.0f); // 0.5 + 0.5
    EXPECT_FLOAT_EQ(mixed.value().at(2, 0), 0.5f); // 0.5 + silence
}

TEST(Mix, RejectsInvalidArguments) {
    AudioBuffer stereo = AudioBuffer::interleaved(48000, 2, {0.0f, 0.0f});
    // channel mismatch
    EXPECT_TRUE(mix(48000, 1, 1, {{&stereo, 1.0}}).isError());
    // null buffer
    EXPECT_TRUE(mix(48000, 1, 1, {{nullptr, 1.0}}).isError());
    // negative gain
    AudioBuffer mono = AudioBuffer::interleaved(48000, 1, {0.0f});
    EXPECT_EQ(mix(48000, 1, 1, {{&mono, -1.0}}).error().code(), ErrorCode::InvalidArgument);
    // invalid output params
    EXPECT_TRUE(mix(0, 1, 1, {}).isError());
}

TEST(Mix, EmptySourceListYieldsSilence) {
    auto mixed = mix(48000, 2, 4, {});
    ASSERT_TRUE(mixed.isOk());
    EXPECT_EQ(mixed.value().frameCount(), 4u);
    for (std::size_t f = 0; f < 4; ++f) {
        EXPECT_FLOAT_EQ(mixed.value().at(f, 0), 0.0f);
        EXPECT_FLOAT_EQ(mixed.value().at(f, 1), 0.0f);
    }
}

// --- Linear resampler: channel conversion -----------------------------------

TEST(LinearResampler, MonoToStereoReplicatesChannel) {
    auto r = makeLinearResampler(48000, 1, 48000, 2);
    ASSERT_TRUE(r.isOk());
    AudioBuffer in = AudioBuffer::interleaved(48000, 1, {0.3f, -0.4f});
    auto out = r.value()->resample(in);
    ASSERT_TRUE(out.isOk());
    ASSERT_EQ(out.value().frameCount(), 2u);
    EXPECT_EQ(out.value().channels(), 2);
    EXPECT_FLOAT_EQ(out.value().at(0, 0), 0.3f);
    EXPECT_FLOAT_EQ(out.value().at(0, 1), 0.3f);
    EXPECT_FLOAT_EQ(out.value().at(1, 0), -0.4f);
    EXPECT_FLOAT_EQ(out.value().at(1, 1), -0.4f);
}

TEST(LinearResampler, StereoToMonoAveragesChannels) {
    auto r = makeLinearResampler(48000, 2, 48000, 1);
    ASSERT_TRUE(r.isOk());
    AudioBuffer in = AudioBuffer::interleaved(48000, 2, {0.4f, 0.2f, 1.0f, -1.0f});
    auto out = r.value()->resample(in);
    ASSERT_TRUE(out.isOk());
    ASSERT_EQ(out.value().frameCount(), 2u);
    EXPECT_FLOAT_EQ(out.value().at(0, 0), 0.3f); // (0.4 + 0.2)/2
    EXPECT_FLOAT_EQ(out.value().at(1, 0), 0.0f); // (1.0 - 1.0)/2
}

TEST(LinearResampler, SameFormatIsIdentity) {
    auto r = makeLinearResampler(48000, 2, 48000, 2);
    ASSERT_TRUE(r.isOk());
    AudioBuffer in = AudioBuffer::interleaved(48000, 2, {0.1f, 0.2f, 0.3f, 0.4f});
    auto out = r.value()->resample(in);
    ASSERT_TRUE(out.isOk());
    EXPECT_EQ(out.value().samples(), in.samples());
}

// --- Linear resampler: rate conversion --------------------------------------

TEST(LinearResampler, UpsampleDoublesFrameCountAndInterpolates) {
    auto r = makeLinearResampler(24000, 1, 48000, 1);
    ASSERT_TRUE(r.isOk());
    // Two input frames -> four output frames; interpolated midpoint appears.
    AudioBuffer in = AudioBuffer::interleaved(24000, 1, {0.0f, 1.0f});
    auto out = r.value()->resample(in);
    ASSERT_TRUE(out.isOk());
    ASSERT_EQ(out.value().frameCount(), 4u);
    EXPECT_FLOAT_EQ(out.value().at(0, 0), 0.0f);  // srcPos 0.0
    EXPECT_FLOAT_EQ(out.value().at(1, 0), 0.5f);  // srcPos 0.5 -> interp
}

TEST(LinearResampler, DownsampleHalvesFrameCount) {
    auto r = makeLinearResampler(48000, 1, 24000, 1);
    ASSERT_TRUE(r.isOk());
    AudioBuffer in = AudioBuffer::interleaved(48000, 1, {0.0f, 0.25f, 0.5f, 0.75f});
    auto out = r.value()->resample(in);
    ASSERT_TRUE(out.isOk());
    EXPECT_EQ(out.value().frameCount(), 2u);
    EXPECT_FLOAT_EQ(out.value().at(0, 0), 0.0f);
    EXPECT_FLOAT_EQ(out.value().at(1, 0), 0.5f);
}

TEST(LinearResampler, EmptyInputYieldsEmptyOutput) {
    auto r = makeLinearResampler(44100, 2, 48000, 2);
    ASSERT_TRUE(r.isOk());
    AudioBuffer in(44100, 2, 0);
    auto out = r.value()->resample(in);
    ASSERT_TRUE(out.isOk());
    EXPECT_TRUE(out.value().empty());
    EXPECT_EQ(out.value().sampleRate(), 48000);
    EXPECT_EQ(out.value().channels(), 2);
}

TEST(LinearResampler, RejectsMismatchedInputAndBadConfig) {
    auto r = makeLinearResampler(48000, 2, 48000, 2);
    ASSERT_TRUE(r.isOk());
    AudioBuffer wrongRate = AudioBuffer::interleaved(44100, 2, {0.0f, 0.0f});
    EXPECT_TRUE(r.value()->resample(wrongRate).isError());

    EXPECT_EQ(makeLinearResampler(0, 2, 48000, 2).error().code(), ErrorCode::InvalidArgument);
    EXPECT_TRUE(makeLinearResampler(48000, 0, 48000, 2).isError());
}

// --- PCM pack / unpack ------------------------------------------------------

TEST(Pack, S16RoundTripThroughUnpack) {
    AudioBuffer in = AudioBuffer::interleaved(48000, 2, {0.0f, 0.5f, -0.5f, 1.0f});
    auto packed = pack(in, SampleFormat::S16);
    ASSERT_TRUE(packed.isOk());
    EXPECT_EQ(packed.value().size(), 4u * 2u); // 4 samples * 2 bytes

    auto restored = unpack(packed.value(), AudioFormat{48000, 2, SampleFormat::S16});
    ASSERT_TRUE(restored.isOk());
    ASSERT_EQ(restored.value().frameCount(), 2u);
    // Within one 16-bit quantization step of the originals.
    EXPECT_NEAR(restored.value().at(0, 1), 0.5f, 1.0f / 32767.0f);
    EXPECT_NEAR(restored.value().at(1, 0), -0.5f, 1.0f / 32767.0f);
    EXPECT_NEAR(restored.value().at(1, 1), 1.0f, 1.0f / 32767.0f);
}

TEST(Pack, F32IsExact) {
    AudioBuffer in = AudioBuffer::interleaved(48000, 1, {0.123f, -0.456f});
    auto packed = pack(in, SampleFormat::F32);
    ASSERT_TRUE(packed.isOk());
    ASSERT_EQ(packed.value().size(), 8u);
    float v0 = 0.0f;
    std::memcpy(&v0, packed.value().data(), 4);
    EXPECT_FLOAT_EQ(v0, 0.123f);
}

TEST(Pack, ClampsOutOfRangeSamples) {
    AudioBuffer in = AudioBuffer::interleaved(48000, 1, {2.0f, -2.0f});
    auto packed = pack(in, SampleFormat::S16);
    ASSERT_TRUE(packed.isOk());
    std::int16_t hi = 0;
    std::int16_t lo = 0;
    std::memcpy(&hi, packed.value().data(), 2);
    std::memcpy(&lo, packed.value().data() + 2, 2);
    EXPECT_EQ(hi, 32767);
    EXPECT_EQ(lo, -32767);
}

TEST(Unpack, RejectsPartialFrames) {
    std::vector<std::byte> threeBytes(3, std::byte{0});
    EXPECT_EQ(unpack(threeBytes, AudioFormat{48000, 1, SampleFormat::S16}).error().code(),
              ErrorCode::InvalidArgument);
}

// --- AudioGraph -------------------------------------------------------------

TEST(AudioGraph, MixesTwoSourcesIntoCommonOutputFormat) {
    AudioGraph graph{AudioFormat{48000, 2, SampleFormat::F32}};
    auto a = graph.addSource(48000, 2, 1.0); // already at output format
    auto b = graph.addSource(24000, 1, 1.0); // needs rate + channel conversion
    ASSERT_TRUE(a.isOk());
    ASSERT_TRUE(b.isOk());
    EXPECT_EQ(graph.sourceCount(), 2u);

    // Long, constant-valued buffers so a deep interior frame is unambiguously
    // at the input value on BOTH backends (well past any resampler filter delay).
    AudioBuffer inA =
        AudioBuffer::interleaved(48000, 2, std::vector<float>(240, 0.2f)); // 120 frames
    AudioBuffer inB =
        AudioBuffer::interleaved(24000, 1, std::vector<float>(100, 0.1f)); // 100 -> 200 frames

    auto rendered = graph.render({inA, inB});
    ASSERT_TRUE(rendered.isOk());
    const AudioBuffer& out = rendered.value();
    EXPECT_EQ(out.channels(), 2);
    EXPECT_EQ(out.sampleRate(), 48000);
    // Longest resampled source is B: resampledFrameCount(100, 24000, 48000) = 200
    // frames at 48k, versus A's 120 identity frames.
    EXPECT_EQ(out.frameCount(), 200u);
    // Source A is 48k stereo -> 48k stereo = identity fast-path: EXACTLY 0.2 for its
    // 120 frames on every backend. Source B (constant 0.1 mono) resamples to a
    // steady-state constant `c` per channel that DIFFERS by backend (libswresample
    // applies a ~1/sqrt(2) mono->stereo center-mix attenuation; the linear fallback
    // replicates at unity gain), but `c` is the SAME at any two steady-state frames.
    // Frame 80 is in the A+B overlap (80 < 120); frame 160 is B-only (>= 120). Their
    // difference cancels B's unknown-but-constant contribution and isolates A's exact
    // identity contribution (0.2) — identical on both backends. Both frames are deep
    // past any resampler filter warm-up.
    EXPECT_NEAR(out.at(80, 0) - out.at(160, 0), 0.2f, 0.02f);
    EXPECT_NEAR(out.at(80, 1) - out.at(160, 1), 0.2f, 0.02f);
    // Source B was actually resampled and mixed across the extended length (present
    // in the B-only region), regardless of the backend's channel-mix gain.
    EXPECT_GT(out.at(160, 0), 0.02f);
    EXPECT_GT(out.at(160, 1), 0.02f);
    // Sanity: no clipping/overflow in the overlap region.
    EXPECT_LE(out.at(80, 0), 0.35f);
}

TEST(AudioGraph, GainAffectsContribution) {
    AudioGraph graph{AudioFormat{48000, 1, SampleFormat::F32}};
    auto id = graph.addSource(48000, 1, 0.5);
    ASSERT_TRUE(id.isOk());
    AudioBuffer in = AudioBuffer::interleaved(48000, 1, {0.8f, 0.8f});

    auto rendered = graph.render({in});
    ASSERT_TRUE(rendered.isOk());
    EXPECT_FLOAT_EQ(rendered.value().at(0, 0), 0.4f); // 0.8 * 0.5

    ASSERT_TRUE(graph.setGain(id.value(), 1.0).isOk());
    auto again = graph.render({in});
    ASSERT_TRUE(again.isOk());
    EXPECT_FLOAT_EQ(again.value().at(0, 0), 0.8f);
}

TEST(AudioGraph, RejectsInputCountAndFormatMismatch) {
    AudioGraph graph{AudioFormat{48000, 2, SampleFormat::F32}};
    ASSERT_TRUE(graph.addSource(48000, 2, 1.0).isOk());

    // Wrong number of inputs.
    EXPECT_EQ(graph.render({}).error().code(), ErrorCode::InvalidArgument);
    // Wrong per-source format.
    AudioBuffer wrong = AudioBuffer::interleaved(44100, 2, {0.0f, 0.0f});
    EXPECT_EQ(graph.render({wrong}).error().code(), ErrorCode::InvalidArgument);
}

TEST(AudioGraph, RejectsBadSourceParams) {
    AudioGraph graph{AudioFormat{48000, 2, SampleFormat::F32}};
    EXPECT_EQ(graph.addSource(0, 2, 1.0).error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(graph.addSource(48000, 2, -0.5).error().code(), ErrorCode::InvalidArgument);

    AudioGraph invalidOut{AudioFormat{0, 0, SampleFormat::F32}};
    EXPECT_EQ(invalidOut.addSource(48000, 2, 1.0).error().code(), ErrorCode::FailedPrecondition);
}

TEST(AudioGraph, RenderPackedProducesOutputBytes) {
    AudioGraph graph{AudioFormat{48000, 1, SampleFormat::S16}};
    ASSERT_TRUE(graph.addSource(48000, 1, 1.0).isOk());
    AudioBuffer in = AudioBuffer::interleaved(48000, 1, {0.5f, -0.5f});

    auto packed = graph.renderPacked({in});
    ASSERT_TRUE(packed.isOk());
    EXPECT_EQ(packed.value().size(), 2u * 2u); // 2 frames * 2 bytes (mono S16)
}

} // namespace
} // namespace palmier::media
