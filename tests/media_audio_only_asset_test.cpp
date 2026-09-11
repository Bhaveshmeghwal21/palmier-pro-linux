// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/media_audio_only_asset_test.cpp — an asset that carries audio and no video.
//
// Requirement 2.2 requires media.import to register an asset with no decodable video stream,
// "reporting resolution and frame rate as absent". Import did that, and the asset then sat on
// an audio track contributing silence, because the decode backend factory demanded a video
// stream (av_find_best_stream for AVMEDIA_TYPE_VIDEO) and refused the file before the entirely
// separate audio path could be reached.
//
// Measured before the fix: a bed at -28.2 dB exported at -91.0 dB (digital silence), and at
// -32.4 dB once the identical audio was muxed onto a file that also carried a video stream.
//
// The fixture is a real RIFF/PCM WAV written byte by byte, because the suite has no real-media
// fixture infrastructure and a mocked backend cannot exercise the factory that held the bug.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/Duration.hpp"
#include "core/Error.hpp"
#include "core/Result.hpp"
#include "media/MediaDecoder.hpp"

namespace palmier::media {
namespace {

void appendLe32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
}

void appendLe16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
}

void appendTag(std::vector<std::uint8_t>& out, const char (&tag)[5]) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(tag[i]));
}

/// A mono 16-bit PCM WAV of `sampleCount` samples at 48 kHz, carrying a low-amplitude ramp so
/// the samples are not all zero.
[[nodiscard]] std::vector<std::uint8_t> monoPcmWav(std::uint32_t sampleCount) {
    constexpr std::uint32_t kRate = 48000;
    constexpr std::uint16_t kChannels = 1;
    constexpr std::uint16_t kBits = 16;
    const std::uint32_t dataBytes = sampleCount * kChannels * (kBits / 8u);

    std::vector<std::uint8_t> w;
    appendTag(w, "RIFF");
    appendLe32(w, 36u + dataBytes);
    appendTag(w, "WAVE");
    appendTag(w, "fmt ");
    appendLe32(w, 16u);                                        // PCM fmt chunk size
    appendLe16(w, 1u);                                         // PCM
    appendLe16(w, kChannels);
    appendLe32(w, kRate);
    appendLe32(w, kRate * kChannels * (kBits / 8u));           // byte rate
    appendLe16(w, static_cast<std::uint16_t>(kChannels * (kBits / 8u)));  // block align
    appendLe16(w, kBits);
    appendTag(w, "data");
    appendLe32(w, dataBytes);
    for (std::uint32_t i = 0; i < sampleCount; ++i) {
        const auto s = static_cast<std::int16_t>((i % 2000u) * 8u);
        appendLe16(w, static_cast<std::uint16_t>(s));
    }
    return w;
}

class AudioOnlyAssetTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("palmier_audio_only_" + std::to_string(::getpid()) + "_" +
                std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
        std::filesystem::create_directories(dir_);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    [[nodiscard]] std::filesystem::path writeWav(std::uint32_t samples) {
        const std::filesystem::path p = dir_ / "bed.wav";
        const std::vector<std::uint8_t> bytes = monoPcmWav(samples);
        std::ofstream f(p, std::ios::binary);
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
        f.close();
        return p;
    }

    std::filesystem::path dir_;
};

TEST_F(AudioOnlyAssetTest, TheRealFactoryOpensAFileThatCarriesAudioAndNoVideo) {
    if (!isFfmpegDecodeAvailable()) GTEST_SKIP() << "this build did not link FFmpeg";

    const std::filesystem::path wav = writeWav(48000);  // one second
    auto opened = ffmpegDecodeBackendFactory()(wav, DecodePrefs{});

    // Before the fix this was Unsupported: "no decodable video stream found in: ...".
    ASSERT_TRUE(opened.isOk()) << opened.error().message();
    ASSERT_NE(opened.value(), nullptr);
}

TEST_F(AudioOnlyAssetTest, AnAudioOnlyBackendDecodesItsAudio) {
    if (!isFfmpegDecodeAvailable()) GTEST_SKIP() << "this build did not link FFmpeg";

    const std::filesystem::path wav = writeWav(48000);
    auto opened = ffmpegDecodeBackendFactory()(wav, DecodePrefs{});
    ASSERT_TRUE(opened.isOk()) << opened.error().message();
    const std::unique_ptr<IDecodeBackend>& backend = opened.value();

    // Stream -1 asks the backend to pick the best audio stream itself, which is what the mix
    // does for an asset whose audio stream index it has not resolved.
    auto block = backend->decodeAudio(-1);
    ASSERT_TRUE(block.isOk()) << block.error().message();
    EXPECT_FALSE(block.value().endOfStream);
    EXPECT_GT(block.value().buffer.frameCount(), 0u);
}

TEST_F(AudioOnlyAssetTest, AskingAnAudioOnlyAssetForAVideoFrameSaysWhy) {
    if (!isFfmpegDecodeAvailable()) GTEST_SKIP() << "this build did not link FFmpeg";

    const std::filesystem::path wav = writeWav(4800);
    auto opened = ffmpegDecodeBackendFactory()(wav, DecodePrefs{});
    ASSERT_TRUE(opened.isOk()) << opened.error().message();

    auto frame = opened.value()->decode(/*useHardware=*/false);
    ASSERT_TRUE(frame.isError());
    EXPECT_EQ(frame.error().code(), ErrorCode::Unsupported);
    // The message must name the real cause rather than "decoder was not initialized", which
    // reads like an internal fault in what is a perfectly ordinary asset.
    EXPECT_NE(frame.error().message().find("audio only"), std::string::npos)
        << frame.error().message();
}

TEST_F(AudioOnlyAssetTest, AFileWithNeitherAudioNorVideoIsStillRefused) {
    if (!isFfmpegDecodeAvailable()) GTEST_SKIP() << "this build did not link FFmpeg";

    const std::filesystem::path junk = dir_ / "notmedia.bin";
    std::ofstream f(junk, std::ios::binary);
    const std::array<char, 64> zeros{};
    f.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
    f.close();

    EXPECT_TRUE(ffmpegDecodeBackendFactory()(junk, DecodePrefs{}).isError());
}

} // namespace
} // namespace palmier::media
