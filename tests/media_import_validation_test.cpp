// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/media_import_validation_test.cpp — unit tests for import validation
// (task 8.5; Requirements 3.2, 3.3).
//
// These drive validateMediaImport through MediaProbe's injectable backend seam,
// so no real FFmpeg or media file is needed. They cover the accept path and the
// two distinct rejection paths (unsupported format vs. unreadable), and assert
// the "library unchanged on rejection" contract by only mutating a stand-in
// library when validation returns Ok.

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "core/Error.hpp"
#include "core/Result.hpp"
#include "media/ImportValidation.hpp"
#include "media/MediaInfo.hpp"
#include "media/MediaProbe.hpp"

namespace palmier::media {
namespace {

// --- Synthetic MediaInfo builders ------------------------------------------

MediaStreamInfo videoStream(MediaCodecId codec, std::string rawName) {
    MediaStreamInfo s;
    s.index = 0;
    s.type = MediaStreamType::Video;
    s.codec = codec;
    s.codecName = std::move(rawName);
    s.resolution = Resolution{1920, 1080};
    return s;
}

MediaStreamInfo audioStream(MediaCodecId codec, std::string rawName) {
    MediaStreamInfo s;
    s.index = 1;
    s.type = MediaStreamType::Audio;
    s.codec = codec;
    s.codecName = std::move(rawName);
    s.sampleRate = 48000;
    s.channels = 2;
    return s;
}

// A probe backend that always returns the given MediaInfo (accept/normalize
// paths). The public probeMediaFile normalizes whatever we return.
MediaProbeBackend backendReturning(MediaInfo info) {
    return [info = std::move(info)](const std::filesystem::path&) -> Result<MediaInfo> {
        return info;
    };
}

// A probe backend that always fails with the given error (unreadable / unknown
// container paths).
MediaProbeBackend backendFailing(Error error) {
    return [error = std::move(error)](const std::filesystem::path&) -> Result<MediaInfo> {
        return err<MediaInfo>(error);
    };
}

// --- Accept path -----------------------------------------------------------

TEST(ImportValidation, AcceptsFileWithDecodableStream) {
    MediaInfo info;
    info.containerFormat = "mov,mp4,m4a,3gp,3g2,mj2";
    info.containerLongName = "QuickTime / MOV";
    info.streams.push_back(videoStream(MediaCodecId::H264, "h264"));
    info.streams.push_back(audioStream(MediaCodecId::AAC, "aac"));

    Result<MediaInfo> result =
        validateMediaImport("/media/clip.mp4", backendReturning(info));

    ASSERT_TRUE(result.isOk()) << result.error().toString();
    EXPECT_TRUE(result.value().hasSupportedStream());
    EXPECT_TRUE(result.value().hasVideo());
}

TEST(ImportValidation, AcceptsWhenOnlyOneOfSeveralStreamsIsSupported) {
    // Unknown video codec but a supported PCM audio track -> still importable.
    MediaInfo info;
    info.containerFormat = "matroska,webm";
    info.streams.push_back(videoStream(MediaCodecId::Unknown, "some_exotic_video"));
    info.streams.push_back(audioStream(MediaCodecId::Pcm, "pcm_s16le"));

    Result<MediaInfo> result =
        validateMediaImport("/media/mixed.mkv", backendReturning(info));

    ASSERT_TRUE(result.isOk()) << result.error().toString();
    EXPECT_TRUE(result.value().hasSupportedStream());
}


// --- Unsupported-format rejection (Requirement 3.2) ------------------------

TEST(ImportValidation, RejectsUnsupportedCodecsAndNamesTheFormat) {
    // Container reads fine, but every stream uses a codec we cannot decode.
    MediaInfo info;
    info.containerFormat = "ogg";
    info.containerLongName = "Ogg";
    info.streams.push_back(videoStream(MediaCodecId::Theora, "theora"));
    info.streams.push_back(audioStream(MediaCodecId::Vorbis, "vorbis"));

    Result<MediaInfo> result =
        validateMediaImport("/media/legacy.ogv", backendReturning(info));

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::Unsupported);

    // The message must name the format so the UI can tell the user what was
    // rejected: it should mention the container and the offending codecs.
    const std::string msg = result.error().message();
    EXPECT_NE(msg.find("Ogg"), std::string::npos) << msg;
    EXPECT_NE(msg.find("Theora"), std::string::npos) << msg;
    EXPECT_NE(msg.find("Vorbis"), std::string::npos) << msg;
}

TEST(ImportValidation, RejectsUnrecognizedContainerAsUnsupported) {
    // The probe could not recognize the container at all.
    Result<MediaInfo> result = validateMediaImport(
        "/media/mystery.bin",
        backendFailing(makeError(ErrorCode::Unsupported,
                                 "unrecognized container: raw bytes")));

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::Unsupported);
    EXPECT_NE(result.error().message().find("unsupported media format"),
              std::string::npos)
        << result.error().message();
}

TEST(ImportValidation, DescribeMediaFormatFallsBackToUnknown) {
    MediaInfo empty;
    EXPECT_EQ(describeMediaFormat(empty), "unknown format");
}

// --- Unreadable / undecodable rejection (Requirement 3.3) ------------------

TEST(ImportValidation, RejectsUnreadableFileWithReadIndication) {
    // Supported-looking file, but the contents cannot be opened/read.
    Result<MediaInfo> result = validateMediaImport(
        "/media/corrupt.mp4",
        backendFailing(makeError(ErrorCode::Io,
                                 "could not open media file: /media/corrupt.mp4")));

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::Io);
    EXPECT_NE(result.error().message().find("could not be read"), std::string::npos)
        << result.error().message();
}

TEST(ImportValidation, RejectsMissingFileAsUnreadable) {
    Result<MediaInfo> result = validateMediaImport(
        "/media/gone.mp4",
        backendFailing(makeError(ErrorCode::NotFound, "no such file")));

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::Io);
}

// --- Propagated caller/build errors ----------------------------------------

TEST(ImportValidation, PropagatesEmptyPathAsInvalidArgument) {
    // probeMediaFile rejects an empty path before the backend is consulted.
    Result<MediaInfo> result =
        validateMediaImport("", backendReturning(MediaInfo{}));

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(ImportValidation, PropagatesMissingFfmpegPrecondition) {
    Result<MediaInfo> result = validateMediaImport(
        "/media/clip.mp4",
        backendFailing(makeError(ErrorCode::FailedPrecondition,
                                 "media probing requires FFmpeg")));

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::FailedPrecondition);
}

// --- Library-unchanged contract (Requirements 3.2, 3.3) --------------------

TEST(ImportValidation, LibraryUnchangedOnEitherRejection) {
    // A stand-in library. The import policy is to append ONLY when validation
    // returns Ok, so both rejection paths must leave this vector untouched.
    std::vector<MediaInfo> library;
    auto tryImport = [&library](const std::filesystem::path& p,
                                const MediaProbeBackend& backend) {
        Result<MediaInfo> r = validateMediaImport(p, backend);
        if (r.isOk()) library.push_back(r.value());
        return r;
    };

    // Unsupported-format rejection: library stays empty.
    MediaInfo unsupported;
    unsupported.containerFormat = "ogg";
    unsupported.streams.push_back(videoStream(MediaCodecId::Theora, "theora"));
    ASSERT_TRUE(tryImport("/media/a.ogv", backendReturning(unsupported)).isError());
    EXPECT_TRUE(library.empty());

    // Unreadable rejection: library still empty.
    ASSERT_TRUE(tryImport("/media/b.mp4",
                          backendFailing(makeError(ErrorCode::Io, "read fail")))
                    .isError());
    EXPECT_TRUE(library.empty());

    // A valid import DOES land in the library, confirming the harness reacts to Ok.
    MediaInfo ok;
    ok.streams.push_back(videoStream(MediaCodecId::H264, "h264"));
    ASSERT_TRUE(tryImport("/media/c.mp4", backendReturning(ok)).isOk());
    EXPECT_EQ(library.size(), 1u);
}

} // namespace
} // namespace palmier::media
