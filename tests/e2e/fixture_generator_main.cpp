// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/e2e/fixture_generator_main.cpp — the build-time fixture generator for the
// end-to-end test (task 12.10 of the end-to-end-editor-integration spec;
// Requirements 3.6, 15.1, 15.9).
//
// Task 12.10: "Generate the 2-second synthetic A/V source and the reference
// `.palmier` document into `tests/fixtures/` at build time rather than checking in
// binaries".
//
// So this program is run ONCE per build, by the `palmier_e2e_fixtures` custom
// command, and writes exactly two files into the directory named on its command
// line:
//
//   * `e2e_source_2s.mov` — a real container carrying ONE video stream and ONE
//     audio stream of exactly 2 seconds at 30 frames per second (60 frames), the
//     fixture Requirement 15.1 asks the end-to-end test to import. Both codecs are
//     ones `media::isImportSupported()` accepts, so the product imports it; the
//     concrete codec is whichever of the candidate list this host's libavcodec
//     carries (see tests/support/SyntheticMedia.hpp).
//   * `e2e_reference.palmier` — a reference project document written through the
//     PRODUCT serializer (`services::saveProjectToFile`), describing a
//     1920x1080/30 fps project with one video track and one audio track, each
//     holding one clip that spans the generated source. It is a fixture in its own
//     right: the end-to-end test opens it through `project.open`, which is how
//     "the generated document is a genuine `.palmier` the product can load" gets
//     checked rather than assumed.
//
// Determinism. Every pixel and every audio sample is a function of the frame index
// alone, and the project's identifiers are FIXED UUID literals rather than
// generated ones, so two runs of this generator produce identical files. That is
// what makes the fixture reproducible instead of merely regenerated, and it is why
// the document can be compared field by field if a later task wants to.
//
// Failure. Any failure is reported on stderr, naming the file that could not be
// written and why, and exits non-zero — which fails the BUILD. A test consuming a
// fixture must never be the first thing to discover that the fixture is missing
// (Requirement 15.9 makes that a test failure, not a skip; failing the build is
// strictly earlier and strictly louder).

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <system_error>

#include "core/Clip.hpp"
#include "core/ColorSpace.hpp"
#include "core/Duration.hpp"
#include "core/FrameRate.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/Project.hpp"
#include "core/Resolution.hpp"
#include "core/Result.hpp"
#include "core/Track.hpp"
#include "core/Uuid.hpp"
#include "services/ProjectStore.hpp"
#include "support/SyntheticMedia.hpp"

namespace {

using palmier::Clip;
using palmier::Duration;
using palmier::FrameRate;
using palmier::MediaAssetRef;
using palmier::Project;
using palmier::Resolution;
using palmier::Result;
using palmier::Track;
using palmier::TrackKind;
using palmier::Uuid;
using palmier::test_support::SyntheticAvSource;
using palmier::test_support::SyntheticAvSpec;

// ---------------------------------------------------------------------------
// What the fixture is
// ---------------------------------------------------------------------------

/// The fixture file names. The end-to-end test knows these same two names through
/// `PALMIER_E2E_FIXTURE_DIR`, and CMake knows them as the custom command's OUTPUT,
/// so all three agree by construction.
constexpr const char* kSourceName = "e2e_source_2s.mov";
constexpr const char* kDocumentName = "e2e_reference.palmier";

/// "at least 2 seconds duration" (Requirement 15.1) at 30 fps — exactly 2 s, so
/// the test has a single expected duration rather than an inequality.
constexpr int kSourceFrames = 60;

/// The source is deliberately SMALL. Requirement 15.1 constrains the PROJECT to
/// 1920x1080, not the imported file, and a small source keeps the build-time
/// generation cheap while still exercising decode, scaling and mixing for real.
constexpr std::uint32_t kSourceWidth = 320;
constexpr std::uint32_t kSourceHeight = 180;

/// The reference document's project settings — the ones Requirement 15.1 names.
constexpr const char* kProjectName = "palmier-e2e-reference";

/// Fixed identifiers, so the generated document is byte-identical across runs.
constexpr const char* kProjectUuid = "11111111-2222-4333-8444-555555555551";
constexpr const char* kAssetUuid = "11111111-2222-4333-8444-555555555552";
constexpr const char* kVideoTrackUuid = "11111111-2222-4333-8444-555555555553";
constexpr const char* kAudioTrackUuid = "11111111-2222-4333-8444-555555555554";
constexpr const char* kVideoClipUuid = "11111111-2222-4333-8444-555555555555";
constexpr const char* kAudioClipUuid = "11111111-2222-4333-8444-555555555556";

[[nodiscard]] std::optional<Uuid> fixedUuid(const char* text) { return Uuid::parse(text); }

[[nodiscard]] Clip makeClip(const Uuid& id, const MediaAssetRef& asset, Duration span) {
    Clip clip;
    clip.id = id;
    clip.assetRef = asset;
    clip.timelineStart = Duration::zero();
    clip.sourceIn = Duration::zero();
    clip.sourceOut = span;
    clip.opacity = 1.0;
    clip.gain = 1.0;
    return clip;
}

/// The reference project: 1920x1080 at 30 fps, one video track and one audio
/// track, each carrying one clip that spans the whole generated source, and the
/// source registered in `assets` so a load rebuilds the media library from it.
[[nodiscard]] std::optional<Project> makeReferenceProject(
    const std::filesystem::path& sourcePath) {
    const std::optional<Uuid> projectId = fixedUuid(kProjectUuid);
    const std::optional<Uuid> assetId = fixedUuid(kAssetUuid);
    const std::optional<Uuid> videoTrackId = fixedUuid(kVideoTrackUuid);
    const std::optional<Uuid> audioTrackId = fixedUuid(kAudioTrackUuid);
    const std::optional<Uuid> videoClipId = fixedUuid(kVideoClipUuid);
    const std::optional<Uuid> audioClipId = fixedUuid(kAudioClipUuid);
    if (!projectId || !assetId || !videoTrackId || !audioTrackId || !videoClipId ||
        !audioClipId) {
        return std::nullopt;
    }

    const FrameRate fps = FrameRate::fps30();
    const Duration span = fps.durationForFrames(kSourceFrames);

    Project project;
    project.id = *projectId;
    project.name = kProjectName;
    project.timelineFps = fps;
    project.canvas = Resolution::hd1080();

    const MediaAssetRef asset(*assetId, sourcePath.string());
    project.assets.push_back(asset);

    Track video;
    video.id = *videoTrackId;
    video.kind = TrackKind::Video;
    video.name = "Video 1";
    video.clips.push_back(makeClip(*videoClipId, asset, span));
    project.tracks.push_back(video);

    Track audio;
    audio.id = *audioTrackId;
    audio.kind = TrackKind::Audio;
    audio.name = "Audio 1";
    audio.clips.push_back(makeClip(*audioClipId, asset, span));
    project.tracks.push_back(audio);

    return project;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: palmier_e2e_fixture_generator <output-directory>\n";
        return EXIT_FAILURE;
    }

    const std::filesystem::path outDir(argv[1]);
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    if (ec) {
        std::cerr << "fixture generator: could not create " << outDir << ": " << ec.message()
                  << '\n';
        return EXIT_FAILURE;
    }

    // --- The synthetic A/V source -------------------------------------------
    const std::filesystem::path sourcePath = outDir / kSourceName;
    SyntheticAvSpec spec;
    spec.width = kSourceWidth;
    spec.height = kSourceHeight;
    spec.frameRate = FrameRate::fps30();
    spec.container = "mov";

    const Result<SyntheticAvSource> source =
        palmier::test_support::writeSyntheticAvSource(sourcePath, spec, kSourceFrames);
    if (source.isError()) {
        std::cerr << "fixture generator: could not write the media fixture " << sourcePath
                  << ": " << source.error().toString() << '\n';
        std::filesystem::remove(sourcePath, ec);
        return EXIT_FAILURE;
    }

    std::cout << "fixture generator: wrote " << sourcePath << " (" << kSourceFrames
              << " frames, " << source.value().duration.seconds() << " s, video \""
              << source.value().choice.videoEncoder << "\", audio \""
              << source.value().choice.audioEncoder << "\", container \""
              << source.value().choice.container << "\")\n";

    // --- The reference `.palmier` document ----------------------------------
    const std::filesystem::path documentPath = outDir / kDocumentName;
    const std::optional<Project> reference = makeReferenceProject(sourcePath);
    if (!reference.has_value()) {
        std::cerr << "fixture generator: the fixed fixture identifiers are not valid UUIDs\n";
        return EXIT_FAILURE;
    }

    const Result<void> written =
        palmier::services::saveProjectToFile(*reference, documentPath);
    if (written.isError()) {
        std::cerr << "fixture generator: could not write the project fixture " << documentPath
                  << ": " << written.error().toString() << '\n';
        std::filesystem::remove(documentPath, ec);
        return EXIT_FAILURE;
    }

    std::cout << "fixture generator: wrote " << documentPath << '\n';
    return EXIT_SUCCESS;
}
