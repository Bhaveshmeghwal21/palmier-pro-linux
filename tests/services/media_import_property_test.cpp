// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/services/media_import_property_test.cpp — the universally quantified
// media-import properties for services::MediaImportService (task 4.2).
//
// Five properties from design.md "Correctness Properties", one RC_GTEST_PROP
// each:
//
//   Property 4  — an accepted import's result is complete, and resolution and
//                 frame rate are present EXACTLY when the probed description
//                 holds a decodable video stream (Requirement 2.2);
//   Property 5  — an unrecognised container, or one whose every stream uses an
//                 undecodable codec, is rejected with the container format and
//                 each rejected codec named, and the library is byte-identical
//                 to before the call (Requirement 2.3);
//   Property 78 — each of the four rejection classes (empty path, nonexistent
//                 file, unopenable/unreadable file, decodable stream that fails
//                 to decode) is rejected with the path and the condition named,
//                 and the library is unchanged (Requirement 2.4);
//   Property 6  — importing any set of equivalent spellings of one location, in
//                 any order and alternating between the media-browser action and
//                 the `media.import` tool, always returns the same asset id and
//                 leaves exactly one entry (Requirement 2.5);
//   Property 7  — for all request sequences the entry count equals the number of
//                 distinct locations imported successfully, and every rejected
//                 and every duplicate request leaves it unchanged
//                 (Requirement 2.6).
//
// These are the universal counterparts to the single examples in
// media_import_service_test.cpp: there each behaviour is pinned with one
// container, one codec pair and one path spelling; here the container format, the
// stream mix, the codec sets, the durations, the resolutions, the frame rates, the
// path spellings, the rejection classes, the pre-existing library and the request
// sequence are all generated.
//
// Everything is driven through the seams MediaImportService already exposes — the
// media::MediaProbeBackend constructor argument, Options::accessCheck and
// Options::decodeCheck — so no real media file, FFmpeg build or codec is needed.
// Small real files back the accept paths so the DEFAULT filesystem access check
// runs for them; only the "cannot be opened or read" class is injected.
//
// _Requirements: 2.2, 2.3, 2.4, 2.5, 2.6_

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <unistd.h> // getpid, for a per-process scratch directory name

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include "core/Duration.hpp"
#include "core/Error.hpp"
#include "core/FrameRate.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/MediaManager.hpp"
#include "core/Resolution.hpp"
#include "core/Result.hpp"
#include "core/Uuid.hpp"
#include "media/MediaInfo.hpp"
#include "media/MediaProbe.hpp"
#include "services/MediaImportService.hpp"
#include "services/ProjectSession.hpp"

namespace palmier::services {
namespace {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Scratch filesystem: one unique directory per generated case, removed however
// the case ends (RapidCheck reports a failure by throwing out of the property
// body, so cleanup must be RAII rather than trailing statements).
// ---------------------------------------------------------------------------

class ScratchDir {
public:
    ScratchDir() {
        static std::atomic<std::uint64_t> counter{0};
        // The process id is part of the name because the test binary is run once
        // per test case (and those runs happen in parallel under ctest -j): a
        // counter alone would have two live cases sharing — and removing — one
        // directory.
        path_ = fs::temp_directory_path() /
                ("palmier_media_import_prop_" + std::to_string(::getpid()) + "_" +
                 std::to_string(counter.fetch_add(1, std::memory_order_relaxed)));
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_, ec);
    }
    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;

    [[nodiscard]] const fs::path& path() const noexcept { return path_; }

    /// A path inside the scratch directory. Nothing is created.
    [[nodiscard]] fs::path child(std::string_view name) const { return path_ / name; }

    /// A small non-empty real file, so the DEFAULT filesystem access check
    /// (exists + openable + readable) is satisfied for it. The bytes are
    /// irrelevant: the probe backend is injected.
    [[nodiscard]] fs::path file(std::string_view name) const {
        const fs::path target = path_ / name;
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        out << "not really media; the probe backend is injected";
        return target;
    }

private:
    fs::path path_;
};

// ---------------------------------------------------------------------------
// Library snapshots. Requirements 2.3, 2.4 and 2.6 speak of the entry COUNT and
// the CONTENTS being unchanged, so a rejection is checked against a total
// rendering of the library (identity + source path of every entry, in order)
// rather than a hand-picked field or the count alone.
// ---------------------------------------------------------------------------

[[nodiscard]] std::string librarySnapshot(const MediaManager& library) {
    std::string rendered;
    for (const MediaAssetRef& entry : library.library()) {
        rendered += entry.assetId.toString();
        rendered += '|';
        rendered += entry.sourcePath;
        rendered += '\n';
    }
    return rendered;
}

[[nodiscard]] bool mentions(const std::string& haystack, std::string_view needle) {
    return !needle.empty() && haystack.find(needle) != std::string::npos;
}

// ---------------------------------------------------------------------------
// Generator vocabulary.
//
// The codec pools are split by the engine's decode set
// (media::isImportSupported): the "decodable" names are the ones validation
// accepts, the "undecodable" names the ones it rejects. Subtitle codec names map
// to MediaCodecId::Unknown, so a subtitle stream is never decodable and a
// subtitle-only container is never accepted.
// ---------------------------------------------------------------------------

const std::vector<std::string> kDecodableVideoCodecs{"h264", "hevc", "av1", "vp9", "prores"};
const std::vector<std::string> kUndecodableVideoCodecs{"theora",     "vp8",   "mpeg4",
                                                      "mpeg2video", "mjpeg", "dnxhd"};
const std::vector<std::string> kDecodableAudioCodecs{"aac", "opus", "pcm_s16le", "pcm_s24le",
                                                     "pcm_f32le"};
const std::vector<std::string> kUndecodableAudioCodecs{"mp3", "vorbis", "flac", "ac3", "eac3",
                                                       "alac"};
const std::vector<std::string> kSubtitleCodecs{"subrip", "ass", "mov_text"};

struct ContainerNames {
    std::string shortName;
    std::string longName;
};

const std::vector<ContainerNames> kContainers{
    {"mov,mp4,m4a,3gp,3g2,mj2", "QuickTime / MOV"},
    {"matroska,webm", "Matroska / WebM"},
    {"wav", "WAV / WAVE"},
    {"mxf", "MXF (Material eXchange Format)"},
    {"avi", "AVI (Audio Video Interleaved)"},
    {"mpegts", "MPEG-TS (MPEG-2 Transport Stream)"},
    {"", "Ogg"},   // the short name is unknown; only the long name is reported
    {"", ""},      // the container names nothing at all
};

/// Draw one element of `pool` (never empty).
template <typename T>
[[nodiscard]] const T& drawFrom(const std::vector<T>& pool) {
    const std::size_t index =
        static_cast<std::size_t>(*rc::gen::inRange<int>(0, static_cast<int>(pool.size())));
    return pool[index];
}

/// The label the service names a rejected codec by: the stable media-level name
/// when the raw decoder name is recognised, otherwise the raw name itself.
[[nodiscard]] std::string codecLabelFor(const std::string& rawName) {
    const media::MediaCodecId codec = media::mediaCodecFromName(rawName);
    if (codec != media::MediaCodecId::Unknown) return std::string(media::codecName(codec));
    return rawName.empty() ? std::string("unknown") : rawName;
}

/// The container label the service names in a result and in a rejection.
[[nodiscard]] std::string containerLabelFor(const ContainerNames& names) {
    if (!names.shortName.empty()) return names.shortName;
    if (!names.longName.empty()) return names.longName;
    return "unknown";
}

// ---------------------------------------------------------------------------
// Stream generators
// ---------------------------------------------------------------------------

[[nodiscard]] Resolution drawResolution() {
    // Valid pixel dimensions within the canvas bounds the domain accepts.
    return Resolution{static_cast<std::uint32_t>(*rc::gen::inRange<int>(16, 7'681)),
                      static_cast<std::uint32_t>(*rc::gen::inRange<int>(16, 4'321))};
}

[[nodiscard]] FrameRate drawFrameRate() {
    // Integer rates (25, 30, 60) and the broadcast rationals (24000/1001, ...).
    if (*rc::gen::arbitrary<bool>()) {
        return FrameRate{*rc::gen::inRange<std::int64_t>(1, 241), 1};
    }
    return FrameRate{*rc::gen::inRange<std::int64_t>(1'000, 240'001),
                     *rc::gen::element<std::int64_t>(1'001, 1'000, 2, 3)};
}

[[nodiscard]] std::int64_t drawDurationMs() {
    return *rc::gen::inRange<std::int64_t>(1, 7'200'001);
}

[[nodiscard]] media::MediaStreamInfo videoStream(int index, std::string codecName,
                                                 Resolution resolution, FrameRate frameRate,
                                                 std::int64_t durationMs) {
    media::MediaStreamInfo stream;
    stream.index = index;
    stream.type = media::MediaStreamType::Video;
    stream.codecName = std::move(codecName);
    stream.resolution = resolution;
    stream.frameRate = frameRate;
    stream.duration = Duration::fromMilliseconds(durationMs);
    return stream;
}

[[nodiscard]] media::MediaStreamInfo audioStream(int index, std::string codecName,
                                                 std::int64_t durationMs) {
    media::MediaStreamInfo stream;
    stream.index = index;
    stream.type = media::MediaStreamType::Audio;
    stream.codecName = std::move(codecName);
    stream.sampleRate = *rc::gen::element<int>(48'000, 44'100, 96'000);
    stream.channels = *rc::gen::element<int>(1, 2, 6);
    stream.duration = Duration::fromMilliseconds(durationMs);
    return stream;
}

[[nodiscard]] media::MediaStreamInfo subtitleStream(int index, std::string codecName,
                                                    std::int64_t durationMs) {
    media::MediaStreamInfo stream;
    stream.index = index;
    stream.type = media::MediaStreamType::Subtitle;
    stream.codecName = std::move(codecName);
    stream.duration = Duration::fromMilliseconds(durationMs);
    return stream;
}

// ---------------------------------------------------------------------------
// Property 4's generated case: a probed description validation ACCEPTS (at least
// one decodable stream), paired with what Requirement 2.2 says the result must
// carry. The expectations are computed from the DRAWN values, not by re-running
// the service's own derivation over the MediaInfo.
// ---------------------------------------------------------------------------

struct AcceptedCase {
    media::MediaInfo          info;
    std::string               expectedContainer;
    std::int64_t              expectedDurationMs = 0;
    bool                      expectVideo = false; ///< A decodable video stream is present.
    bool                      expectAudio = false;
    std::optional<Resolution> expectedResolution;
    std::optional<FrameRate>  expectedFrameRate;
};

/// Stream mixes: video-only, audio-only, both, a decodable half paired with an
/// undecodable one, and mixes carrying a (never decodable) subtitle stream.
enum class StreamMix {
    DecodableVideoOnly,
    DecodableAudioOnly,
    DecodableVideoAndAudio,
    DecodableVideoUndecodableAudio,
    UndecodableVideoDecodableAudio,
    DecodableVideoAndSubtitle,
    DecodableAudioAndSubtitle,
    UndecodableVideoDecodableAudioAndSubtitle,
};

[[nodiscard]] AcceptedCase drawAcceptedCase() {
    const StreamMix mix = *rc::gen::element<StreamMix>(
        StreamMix::DecodableVideoOnly, StreamMix::DecodableAudioOnly,
        StreamMix::DecodableVideoAndAudio, StreamMix::DecodableVideoUndecodableAudio,
        StreamMix::UndecodableVideoDecodableAudio, StreamMix::DecodableVideoAndSubtitle,
        StreamMix::DecodableAudioAndSubtitle,
        StreamMix::UndecodableVideoDecodableAudioAndSubtitle);

    const bool decodableVideo = mix == StreamMix::DecodableVideoOnly ||
                                mix == StreamMix::DecodableVideoAndAudio ||
                                mix == StreamMix::DecodableVideoUndecodableAudio ||
                                mix == StreamMix::DecodableVideoAndSubtitle;
    const bool undecodableVideo = mix == StreamMix::UndecodableVideoDecodableAudio ||
                                  mix == StreamMix::UndecodableVideoDecodableAudioAndSubtitle;
    const bool decodableAudio = mix == StreamMix::DecodableAudioOnly ||
                                mix == StreamMix::DecodableVideoAndAudio ||
                                mix == StreamMix::UndecodableVideoDecodableAudio ||
                                mix == StreamMix::DecodableAudioAndSubtitle ||
                                mix == StreamMix::UndecodableVideoDecodableAudioAndSubtitle;
    const bool undecodableAudio = mix == StreamMix::DecodableVideoUndecodableAudio;
    const bool subtitle = mix == StreamMix::DecodableVideoAndSubtitle ||
                          mix == StreamMix::DecodableAudioAndSubtitle ||
                          mix == StreamMix::UndecodableVideoDecodableAudioAndSubtitle;

    AcceptedCase generated;
    const ContainerNames container = drawFrom(kContainers);
    generated.info.containerFormat = container.shortName;
    generated.info.containerLongName = container.longName;
    generated.expectedContainer = containerLabelFor(container);

    int          index = 0;
    std::int64_t longestStreamMs = 0;

    if (undecodableVideo) {
        const std::int64_t streamMs = drawDurationMs();
        longestStreamMs = std::max(longestStreamMs, streamMs);
        generated.info.streams.push_back(videoStream(index++, drawFrom(kUndecodableVideoCodecs),
                                                     drawResolution(), drawFrameRate(), streamMs));
    }
    if (decodableVideo) {
        const std::int64_t streamMs = drawDurationMs();
        const Resolution   resolution = drawResolution();
        const FrameRate    frameRate = drawFrameRate();
        longestStreamMs = std::max(longestStreamMs, streamMs);
        generated.info.streams.push_back(
            videoStream(index++, drawFrom(kDecodableVideoCodecs), resolution, frameRate, streamMs));
        generated.expectVideo = true;
        generated.expectedResolution = resolution;
        generated.expectedFrameRate = frameRate;
    }
    if (decodableAudio) {
        const std::int64_t streamMs = drawDurationMs();
        longestStreamMs = std::max(longestStreamMs, streamMs);
        generated.info.streams.push_back(
            audioStream(index++, drawFrom(kDecodableAudioCodecs), streamMs));
        generated.expectAudio = true;
    }
    if (undecodableAudio) {
        const std::int64_t streamMs = drawDurationMs();
        longestStreamMs = std::max(longestStreamMs, streamMs);
        generated.info.streams.push_back(
            audioStream(index++, drawFrom(kUndecodableAudioCodecs), streamMs));
    }
    if (subtitle) {
        const std::int64_t streamMs = drawDurationMs();
        longestStreamMs = std::max(longestStreamMs, streamMs);
        generated.info.streams.push_back(
            subtitleStream(index++, drawFrom(kSubtitleCodecs), streamMs));
    }

    // A container either states its own duration or leaves it to be derived from
    // its longest stream (the raw/streamed-input case).
    if (*rc::gen::arbitrary<bool>()) {
        const std::int64_t containerMs = drawDurationMs();
        generated.info.duration = Duration::fromMilliseconds(containerMs);
        generated.expectedDurationMs = containerMs;
    } else {
        generated.info.duration = Duration::zero();
        generated.expectedDurationMs = longestStreamMs;
    }

    return generated;
}

// ---------------------------------------------------------------------------
// Backends
// ---------------------------------------------------------------------------

[[nodiscard]] media::MediaProbeBackend backendReturning(media::MediaInfo info) {
    return [info](const fs::path&) { return Result<media::MediaInfo>(info); };
}

[[nodiscard]] media::MediaProbeBackend backendFailing(ErrorCode code, std::string message) {
    return [code, message](const fs::path&) {
        return err<media::MediaInfo>(makeError(code, message));
    };
}

/// Pre-populate a project's media library with 0-20 assets registered by
/// something other than this service (an opened document, a generation), so
/// "the library is unchanged" is checked against a non-empty library too.
void prePopulateLibrary(ProjectSession& session) {
    const int existing = *rc::gen::inRange<int>(0, 21);
    for (int i = 0; i < existing; ++i) {
        // Identities are freshly generated rather than drawn byte-wise: a
        // byte-wise draw shrinks straight to the nil UUID and to duplicates,
        // both of which MediaManager::importAsset rejects. These properties
        // quantify over import REQUESTS, not over identity collisions.
        const std::string path = "/library/preexisting_" + std::to_string(i) + ".mp4";
        RC_ASSERT(session.mediaLibrary().importAsset(MediaAssetRef(Uuid::generateV4(), path))
                      .isOk());
    }
}

// ---------------------------------------------------------------------------
// Property 4
// ---------------------------------------------------------------------------

// Feature: end-to-end-editor-integration, Property 4: Import result completeness
// and optional-field rule — for any probed media description that validation
// accepts, the `media.import` result carries an asset identifier, the resolved
// absolute source path, the container format name and the duration in ms, and
// carries a pixel resolution and a frame rate exactly when the description holds
// at least one decodable video stream.
// Validates: Requirements 2.2
RC_GTEST_PROP(MediaImportProperties, AnAcceptedImportReportsEveryRequiredFieldAndOnlyThen, ()) {
    const AcceptedCase generated = drawAcceptedCase();

    const ScratchDir scratch;
    const fs::path   file = scratch.file("clip.media");

    ProjectSession     session;
    MediaImportService service(session, backendReturning(generated.info));

    const Result<ImportedAsset> imported = service.import(file);
    RC_ASSERT(imported.isOk());
    const ImportedAsset& asset = imported.value();

    // The result carries an asset identifier that the library resolves.
    RC_ASSERT(!asset.assetId.isNil());
    RC_ASSERT(session.mediaLibrary().hasAsset(asset.assetId));
    RC_ASSERT(!asset.duplicate);

    // ... the resolved ABSOLUTE source path ...
    RC_ASSERT(asset.sourcePath.is_absolute());
    RC_ASSERT(asset.sourcePath == fs::weakly_canonical(file));

    // ... the container format name ...
    RC_ASSERT(asset.containerFormat == generated.expectedContainer);
    RC_ASSERT(!asset.containerFormat.empty());

    // ... and the duration in milliseconds.
    RC_ASSERT(asset.durationMs == generated.expectedDurationMs);

    // Resolution and frame rate are present EXACTLY when a decodable video
    // stream is, and carry that stream's declared values.
    RC_ASSERT(asset.hasVideo == generated.expectVideo);
    RC_ASSERT(asset.hasAudio == generated.expectAudio);
    RC_ASSERT(asset.resolution.has_value() == generated.expectVideo);
    RC_ASSERT(asset.frameRate.has_value() == generated.expectVideo);
    if (generated.expectVideo) {
        RC_ASSERT(*asset.resolution == *generated.expectedResolution);
        RC_ASSERT(*asset.frameRate == *generated.expectedFrameRate);
    }

    // Exactly one entry was registered for the location.
    RC_ASSERT(session.mediaLibrary().assetCount() == 1u);
    RC_ASSERT(session.mediaLibrary().library().front().sourcePath == asset.sourcePath.string());
    RC_ASSERT(service.lastFailure() == ImportFailure::None);
}

// ---------------------------------------------------------------------------
// Property 5
// ---------------------------------------------------------------------------

// Feature: end-to-end-editor-integration, Property 5: Rejected imports name the
// format and leave the library unchanged — for any probed media description whose
// container is unrecognised or whose every stream uses an undecodable codec, the
// import fails with an error naming the container format and each rejected codec,
// and the media library's entry count and contents are byte-identical to before
// the call.
// Validates: Requirements 2.3
RC_GTEST_PROP(MediaImportProperties, AFormatRejectionNamesTheFormatAndChangesNothing, ()) {
    ProjectSession session;
    prePopulateLibrary(session);

    const ScratchDir scratch;
    const fs::path   file = scratch.file("rejected.media");

    // Either the container itself is unrecognised, or it was read but every
    // stream in it uses a codec the engine cannot decode.
    const bool unrecognisedContainer = *rc::gen::arbitrary<bool>();

    std::vector<std::string> expectedMentions;
    media::MediaProbeBackend backend;

    if (unrecognisedContainer) {
        // The probe could not recognise the container. Its message carries
        // whatever is known about the format — sometimes nothing at all.
        const std::string named = *rc::gen::element<std::string>(
            "unrecognized container format", "unrecognized container format: bin",
            "unrecognized container format: application/octet-stream", "");
        backend = backendFailing(ErrorCode::Unsupported, named);
        expectedMentions.push_back(named.empty() ? "unknown" : named);
        // No codec was recognised, so the rejected-codec list says so.
        expectedMentions.push_back("rejected codecs: unknown");
    } else {
        const ContainerNames container = drawFrom(kContainers);
        media::MediaInfo     info;
        info.containerFormat = container.shortName;
        info.containerLongName = container.longName;
        info.duration = Duration::fromMilliseconds(drawDurationMs());

        // 1-4 streams, every one of them undecodable.
        const int streamCount = *rc::gen::inRange<int>(1, 5);
        for (int i = 0; i < streamCount; ++i) {
            const std::int64_t streamMs = drawDurationMs();
            switch (*rc::gen::inRange<int>(0, 3)) {
                case 0:
                    info.streams.push_back(videoStream(i, drawFrom(kUndecodableVideoCodecs),
                                                       drawResolution(), drawFrameRate(),
                                                       streamMs));
                    break;
                case 1:
                    info.streams.push_back(
                        audioStream(i, drawFrom(kUndecodableAudioCodecs), streamMs));
                    break;
                default:
                    info.streams.push_back(
                        subtitleStream(i, drawFrom(kSubtitleCodecs), streamMs));
                    break;
            }
        }

        // The container format, and every distinct rejected codec, must be named.
        expectedMentions.push_back(containerLabelFor(container));
        for (const media::MediaStreamInfo& stream : info.streams) {
            expectedMentions.push_back(codecLabelFor(stream.codecName));
        }
        backend = backendReturning(info);
    }

    MediaImportService service(session, backend);

    const std::string  snapshotBefore = librarySnapshot(session.mediaLibrary());
    const std::size_t  countBefore = session.mediaLibrary().assetCount();
    const bool         modifiedBefore = session.modified();

    const Result<ImportedAsset> imported = service.import(file);

    // The import failed as an unsupported format.
    RC_ASSERT(imported.isError());
    RC_ASSERT(imported.error().code() == ErrorCode::Unsupported);
    RC_ASSERT(service.lastFailure() == ImportFailure::UnsupportedFormat);

    // The error names the path, the container format and each rejected codec.
    const std::string message = imported.error().message();
    RC_ASSERT(mentions(message, MediaImportService::resolvePath(file).string()));
    for (const std::string& expected : expectedMentions) {
        RC_ASSERT(mentions(message, expected));
    }

    // The library's entry count and contents are byte-identical.
    RC_ASSERT(session.mediaLibrary().assetCount() == countBefore);
    RC_ASSERT(librarySnapshot(session.mediaLibrary()) == snapshotBefore);
    RC_ASSERT(session.modified() == modifiedBefore);
}

// ---------------------------------------------------------------------------
// Property 78
// ---------------------------------------------------------------------------

/// The four rejection classes of Requirement 2.4.
enum class RejectionClass {
    EmptyPath,
    Nonexistent,
    Unreadable,
    Undecodable,
};

// Feature: end-to-end-editor-integration, Property 78: A rejected import
// classifies its failure and changes nothing — for any import target drawn from
// the four rejection classes (an empty path, a path naming no existing file, a
// path naming a file that cannot be opened or read, and a path whose decodable
// stream fails to decode), the import fails with an error that names the file
// path and states which of the four conditions occurred, and the media library's
// entry count and contents are unchanged.
// Validates: Requirements 2.4
RC_GTEST_PROP(MediaImportProperties, ARejectedImportClassifiesItsFailureAndChangesNothing, ()) {
    ProjectSession session;
    prePopulateLibrary(session);

    const ScratchDir scratch;
    const RejectionClass rejection = *rc::gen::element<RejectionClass>(
        RejectionClass::EmptyPath, RejectionClass::Nonexistent, RejectionClass::Unreadable,
        RejectionClass::Undecodable);

    // A decodable container, so nothing but the drawn condition can reject the
    // import.
    media::MediaInfo decodable;
    decodable.containerFormat = "mov,mp4,m4a,3gp,3g2,mj2";
    decodable.containerLongName = "QuickTime / MOV";
    decodable.duration = Duration::fromMilliseconds(drawDurationMs());
    decodable.streams.push_back(videoStream(0, drawFrom(kDecodableVideoCodecs), drawResolution(),
                                            drawFrameRate(), drawDurationMs()));
    decodable.streams.push_back(audioStream(1, drawFrom(kDecodableAudioCodecs), drawDurationMs()));

    const std::string             name = "target_" + std::to_string(*rc::gen::inRange<int>(0, 1000));
    fs::path                      target;
    ImportFailure                 expected = ImportFailure::None;
    MediaImportService::Options   options;

    switch (rejection) {
        case RejectionClass::EmptyPath:
            target = fs::path{};
            expected = ImportFailure::EmptyPath;
            break;
        case RejectionClass::Nonexistent:
            // Nothing is created at this location.
            target = scratch.child(name + ".mp4");
            expected = ImportFailure::FileNotFound;
            break;
        case RejectionClass::Unreadable:
            expected = ImportFailure::FileUnreadable;
            if (*rc::gen::arbitrary<bool>()) {
                // A real target that exists but cannot be opened as a file.
                target = scratch.child(name + ".dir");
                fs::create_directories(target);
            } else {
                // A file whose open/read fails, injected through the access seam
                // so no privileged or exotic filesystem state is needed.
                target = scratch.file(name + ".mp4");
                options.accessCheck = [](const fs::path& path) {
                    return err<void>(makeError(ErrorCode::Io,
                                              "cannot open for reading: " + path.string()));
                };
            }
            break;
        case RejectionClass::Undecodable:
            // The container reads and validates, but its decodable stream does
            // not actually decode.
            target = scratch.file(name + ".mp4");
            expected = ImportFailure::UndecodableContent;
            options.decodeCheck = [](const fs::path& path, const media::MediaInfo&) {
                return err<void>(
                    makeError(ErrorCode::Io, "decoder failed on " + path.filename().string()));
            };
            break;
    }

    MediaImportService service(session, backendReturning(decodable), std::move(options));

    const std::string snapshotBefore = librarySnapshot(session.mediaLibrary());
    const std::size_t countBefore = session.mediaLibrary().assetCount();
    const bool        modifiedBefore = session.modified();

    const Result<ImportedAsset> imported = service.import(target);

    // The import failed, classified as exactly the condition that occurred.
    RC_ASSERT(imported.isError());
    RC_ASSERT(service.lastFailure() == expected);
    RC_ASSERT(imported.error().code() == importFailureCode(expected));

    // The error states WHICH of the four conditions occurred, and names the file
    // path (an empty path has no spelling to name; it is named as empty).
    const std::string message = imported.error().message();
    RC_ASSERT(mentions(message, describeImportFailure(expected)));
    if (rejection == RejectionClass::EmptyPath) {
        RC_ASSERT(mentions(message, "''"));
    } else {
        RC_ASSERT(mentions(message, MediaImportService::resolvePath(target).string()));
    }

    // The library's entry count and contents are unchanged, and nothing is left
    // pending.
    RC_ASSERT(session.mediaLibrary().assetCount() == countBefore);
    RC_ASSERT(librarySnapshot(session.mediaLibrary()) == snapshotBefore);
    RC_ASSERT(session.modified() == modifiedBefore);
    RC_ASSERT(service.pendingCount() == 0u);
}

// ---------------------------------------------------------------------------
// Property 6
// ---------------------------------------------------------------------------

/// The two entry points Requirement 2.5 names. Both funnel through the ONE
/// MediaImportService — that is what makes "one asset per location" hold no
/// matter who asked — and differ in the spelling they hand it: the media browser
/// passes the path exactly as the user selected it, while the `media.import` tool
/// contracts for an absolute path and therefore resolves first.
enum class EntryPoint { MediaBrowser, ImportTool };

[[nodiscard]] Result<ImportedAsset> importVia(EntryPoint entry, MediaImportService& service,
                                              const fs::path& spelling) {
    if (entry == EntryPoint::ImportTool) {
        return service.import(MediaImportService::resolvePath(spelling));
    }
    return service.import(spelling);
}

/// 1-6 further spellings of `file`, all naming the same filesystem location:
/// a redundant `.`, `..` round trips through existing and non-existing
/// directories, repeated separators, a separator-terminated directory component,
/// and a spelling relative to the current working directory.
[[nodiscard]] std::vector<fs::path> drawEquivalentSpellings(const ScratchDir& scratch,
                                                            const fs::path& file) {
    const std::string dir = scratch.path().string();
    const std::string name = file.filename().string();

    std::vector<fs::path> pool{
        fs::path(dir + "/./" + name),          // a redundant current-directory segment
        fs::path(dir + "/sub/../" + name),     // a round trip through an existing directory
        fs::path(dir + "/nosuch/../" + name),  // ... and through one that does not exist
        fs::path(dir + "//" + name),           // repeated separators
        fs::path(dir + "/sub//../" + name),    // a separator-terminated directory component
        fs::path(dir + "/./sub/.././" + name), // several of the above at once
    };
    fs::create_directories(scratch.path() / "sub");

    // A spelling relative to the current working directory, when one exists and
    // genuinely names the same location (a spelling that does not belong to the
    // equivalence class has no business being quantified over).
    std::error_code ec;
    const fs::path  relative = fs::relative(file, fs::current_path(ec), ec);
    if (!ec && !relative.empty() &&
        MediaImportService::resolvePath(relative) == fs::weakly_canonical(file)) {
        pool.push_back(relative);
    }

    const int count = *rc::gen::inRange<int>(1, static_cast<int>(pool.size()) + 1);
    std::vector<fs::path> chosen;
    for (int i = 0; i < count; ++i) {
        const std::size_t index =
            static_cast<std::size_t>(*rc::gen::inRange<int>(0, static_cast<int>(pool.size())));
        chosen.push_back(pool[index]);
    }
    return chosen;
}

// Feature: end-to-end-editor-integration, Property 6: Import is idempotent over
// path spellings — for any canonical file location and any set of equivalent
// spellings of it (relative, trailing separator, `.`/`..` segments, repeated
// separators), importing those spellings in any order — and alternating between
// the media-browser action and the `media.import` tool — returns the same asset
// identifier every time and leaves exactly one media library entry for that
// location.
// Validates: Requirements 2.5
RC_GTEST_PROP(MediaImportProperties, ImportIsIdempotentOverEquivalentPathSpellings, ()) {
    const AcceptedCase generated = drawAcceptedCase();

    const ScratchDir scratch;
    const fs::path   file = scratch.file("shared_location.media");
    const fs::path   canonical = fs::weakly_canonical(file);

    // The canonical spelling plus 1-6 equivalent ones, requested in an arbitrary
    // order, each through an arbitrary entry point.
    std::vector<fs::path> requests = drawEquivalentSpellings(scratch, file);
    requests.push_back(file);
    std::vector<fs::path> order;
    while (!requests.empty()) {
        const std::size_t index =
            static_cast<std::size_t>(*rc::gen::inRange<int>(0, static_cast<int>(requests.size())));
        order.push_back(requests[index]);
        requests.erase(requests.begin() + static_cast<std::ptrdiff_t>(index));
    }

    ProjectSession     session;
    MediaImportService service(session, backendReturning(generated.info));

    std::optional<Uuid> firstAssetId;
    for (std::size_t i = 0; i < order.size(); ++i) {
        // Alternate between the media-browser action and the `media.import` tool.
        const EntryPoint entry = (i % 2 == 0) ? EntryPoint::MediaBrowser : EntryPoint::ImportTool;
        const Result<ImportedAsset> imported = importVia(entry, service, order[i]);
        RC_ASSERT(imported.isOk());

        // Every spelling resolves to the one location ...
        RC_ASSERT(imported.value().sourcePath == canonical);
        // ... and returns the same asset identifier, the later requests being
        // reported as duplicates of the first.
        if (!firstAssetId.has_value()) {
            firstAssetId = imported.value().assetId;
            RC_ASSERT(!imported.value().duplicate);
        } else {
            RC_ASSERT(imported.value().assetId == *firstAssetId);
            RC_ASSERT(imported.value().duplicate);
        }

        // Exactly one library entry exists for the location, throughout.
        RC_ASSERT(session.mediaLibrary().assetCount() == 1u);
    }

    RC_ASSERT(firstAssetId.has_value());
    RC_ASSERT(session.mediaLibrary().assetCount() == 1u);
    RC_ASSERT(session.mediaLibrary().hasAsset(*firstAssetId));
    RC_ASSERT(session.mediaLibrary().library().front().sourcePath == canonical.string());
}

// ---------------------------------------------------------------------------
// Property 7
// ---------------------------------------------------------------------------

/// One request in a generated sequence.
enum class RequestKind {
    NewLocation,        ///< A fresh decodable file: succeeds.
    DuplicateLocation,  ///< An already imported location, possibly re-spelled.
    UnsupportedFormat,  ///< An unrecognised container: rejected (2.3).
    UnreadableFile,     ///< A file that cannot be opened or read: rejected (2.4).
    EmptyPath,          ///< An empty path: rejected (2.4).
};

// Feature: end-to-end-editor-integration, Property 7: Media library entry count
// invariant — for all sequences of import requests, the media library's entry
// count equals the number of distinct absolute filesystem locations imported
// successfully, and every rejected and every duplicate request leaves that count
// unchanged.
// Validates: Requirements 2.6
RC_GTEST_PROP(MediaImportProperties, LibraryEntryCountEqualsDistinctLocationsImported, ()) {
    const ScratchDir scratch;

    // The behaviour of each generated path, consulted by the injected seams so a
    // single service can serve a whole heterogeneous request sequence.
    auto decodable = std::make_shared<std::map<std::string, media::MediaInfo>>();
    auto unsupported = std::make_shared<std::set<std::string>>();
    auto unreadable = std::make_shared<std::set<std::string>>();

    media::MediaProbeBackend backend = [decodable, unsupported](
                                           const fs::path& path) -> Result<media::MediaInfo> {
        const std::string key = path.generic_string();
        if (unsupported->count(key) != 0) {
            return err<media::MediaInfo>(
                makeError(ErrorCode::Unsupported, "unrecognized container format"));
        }
        if (const auto found = decodable->find(key); found != decodable->end()) {
            return Result<media::MediaInfo>(found->second);
        }
        return err<media::MediaInfo>(makeError(ErrorCode::NotFound, "no such file: " + key));
    };

    MediaImportService::Options options;
    options.accessCheck = [unreadable](const fs::path& path) -> Result<void> {
        if (unreadable->count(path.generic_string()) != 0) {
            return err<void>(makeError(ErrorCode::Io, "cannot read: " + path.string()));
        }
        return MediaImportService::filesystemAccessCheck()(path);
    };

    ProjectSession     session;
    MediaImportService service(session, backend, std::move(options));

    // The distinct absolute locations imported successfully, and the asset each
    // one was registered under.
    std::map<std::string, Uuid> succeeded;

    const int requestCount = *rc::gen::inRange<int>(0, 41);
    for (int i = 0; i < requestCount; ++i) {
        RequestKind kind = *rc::gen::element<RequestKind>(
            RequestKind::NewLocation, RequestKind::NewLocation, RequestKind::DuplicateLocation,
            RequestKind::UnsupportedFormat, RequestKind::UnreadableFile, RequestKind::EmptyPath);
        if (kind == RequestKind::DuplicateLocation && succeeded.empty()) {
            kind = RequestKind::NewLocation;
        }

        fs::path request;
        switch (kind) {
            case RequestKind::NewLocation: {
                request = scratch.file("new_" + std::to_string(i) + ".mp4");
                media::MediaInfo info = drawAcceptedCase().info;
                (*decodable)[MediaImportService::resolvePath(request).generic_string()] =
                    std::move(info);
                break;
            }
            case RequestKind::DuplicateLocation: {
                const std::size_t index = static_cast<std::size_t>(
                    *rc::gen::inRange<int>(0, static_cast<int>(succeeded.size())));
                auto entry = succeeded.begin();
                std::advance(entry, static_cast<std::ptrdiff_t>(index));
                const fs::path registered(entry->first);
                // Either the same spelling, or an equivalent one.
                request = *rc::gen::arbitrary<bool>()
                              ? registered
                              : fs::path(registered.parent_path().string() + "/./" +
                                         registered.filename().string());
                break;
            }
            case RequestKind::UnsupportedFormat: {
                request = scratch.file("unsupported_" + std::to_string(i) + ".bin");
                unsupported->insert(MediaImportService::resolvePath(request).generic_string());
                break;
            }
            case RequestKind::UnreadableFile: {
                request = scratch.file("unreadable_" + std::to_string(i) + ".mp4");
                unreadable->insert(MediaImportService::resolvePath(request).generic_string());
                break;
            }
            case RequestKind::EmptyPath:
                request = fs::path{};
                break;
        }

        const std::size_t countBefore = session.mediaLibrary().assetCount();
        const std::string snapshotBefore = librarySnapshot(session.mediaLibrary());
        const std::string key = MediaImportService::resolvePath(request).generic_string();

        const Result<ImportedAsset> imported = service.import(request);

        switch (kind) {
            case RequestKind::NewLocation:
                // A distinct location imported successfully adds exactly one entry.
                RC_ASSERT(imported.isOk());
                RC_ASSERT(!imported.value().duplicate);
                RC_ASSERT(succeeded.emplace(key, imported.value().assetId).second);
                RC_ASSERT(session.mediaLibrary().assetCount() == countBefore + 1u);
                break;
            case RequestKind::DuplicateLocation:
                // A duplicate returns the existing asset and changes nothing.
                RC_ASSERT(imported.isOk());
                RC_ASSERT(imported.value().duplicate);
                RC_ASSERT(succeeded.count(key) == 1u);
                RC_ASSERT(imported.value().assetId == succeeded.at(key));
                RC_ASSERT(session.mediaLibrary().assetCount() == countBefore);
                RC_ASSERT(librarySnapshot(session.mediaLibrary()) == snapshotBefore);
                break;
            case RequestKind::UnsupportedFormat:
            case RequestKind::UnreadableFile:
            case RequestKind::EmptyPath:
                // Every rejection leaves the count and the contents alone.
                RC_ASSERT(imported.isError());
                RC_ASSERT(session.mediaLibrary().assetCount() == countBefore);
                RC_ASSERT(librarySnapshot(session.mediaLibrary()) == snapshotBefore);
                break;
        }

        // The invariant, after every single request.
        RC_ASSERT(session.mediaLibrary().assetCount() == succeeded.size());
    }

    RC_ASSERT(session.mediaLibrary().assetCount() == succeeded.size());
    for (const auto& [path, assetId] : succeeded) {
        RC_ASSERT(session.mediaLibrary().hasAsset(assetId));
        const std::optional<MediaAssetRef> entry = session.mediaLibrary().asset(assetId);
        RC_ASSERT(entry.has_value());
        RC_ASSERT(MediaImportService::resolvePath(fs::path(entry->sourcePath)).generic_string() ==
                  path);
    }
}

} // namespace
} // namespace palmier::services
