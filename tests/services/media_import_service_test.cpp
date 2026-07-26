// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/services/media_import_service_test.cpp — example-based unit tests for
// services::MediaImportService (task 4.1).
//
// Coverage, by the behaviour each group pins down:
//
//   Accept path .......... probe -> validate -> register puts EXACTLY one asset in
//                          the current project's media library and reports the
//                          asset id, the resolved absolute path, the container
//                          format and the duration in milliseconds (2.1, 2.2).
//   Optional fields ...... resolution and frame rate are reported only for an
//                          asset carrying a DECODABLE video stream, and absent for
//                          an audio-only asset or one whose video stream uses a
//                          codec the engine cannot decode (2.2).
//   Format rejection ..... an unrecognised container, and a container in which
//                          every stream is undecodable, are rejected with the
//                          container format and the rejected codecs named, leaving
//                          the library's entry count and contents unchanged (2.3).
//   Path rejection ....... empty, missing, unopenable and undecodable targets are
//                          each rejected with the path AND the condition that
//                          occurred named, leaving the library unchanged (2.4).
//   Duplicates ........... a second import of the same filesystem location — by a
//                          different spelling, and through the second caller —
//                          returns the existing asset id, adds no entry and keeps
//                          exactly one entry for that location (2.5).
//   No project ........... with no project open nothing is registered and the error
//                          states that no project is open (2.7).
//   Time limit ........... a probe that outruns the limit is abandoned, the library
//                          is unchanged, and the error names the path and the limit
//                          (2.8).
//   Pending .............. while probe/validate/register are running the path is
//                          reported pending and NO library entry exists for it
//                          (2.9).
//
// Every case is driven through the media::MediaProbeBackend seam, so no real media
// file, FFmpeg build or codec is needed; the small filesystem fixtures exist only
// so the missing / unopenable classifications exercise the real access check.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include <unistd.h> // getpid, for a per-process fixture directory name

#include "core/Duration.hpp"
#include "core/Error.hpp"
#include "core/FrameRate.hpp"
#include "core/Resolution.hpp"
#include "media/MediaInfo.hpp"
#include "media/MediaProbe.hpp"
#include "services/MediaImportService.hpp"
#include "services/ProjectSession.hpp"

namespace palmier::services {
namespace {

using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Synthetic containers
// ---------------------------------------------------------------------------

[[nodiscard]] media::MediaStreamInfo videoStream(std::string codecName, Resolution resolution,
                                                 FrameRate fps) {
    media::MediaStreamInfo stream;
    stream.index = 0;
    stream.type = media::MediaStreamType::Video;
    stream.codecName = std::move(codecName);
    stream.resolution = resolution;
    stream.frameRate = fps;
    stream.duration = Duration::fromMilliseconds(2500);
    return stream;
}

[[nodiscard]] media::MediaStreamInfo audioStream(std::string codecName) {
    media::MediaStreamInfo stream;
    stream.index = 1;
    stream.type = media::MediaStreamType::Audio;
    stream.codecName = std::move(codecName);
    stream.sampleRate = 48000;
    stream.channels = 2;
    stream.duration = Duration::fromMilliseconds(2500);
    return stream;
}

/// A decodable H.264 + AAC MP4 of 2.5 seconds at 1920x1080 / 30 fps.
[[nodiscard]] media::MediaInfo mp4WithVideoAndAudio() {
    media::MediaInfo info;
    info.containerFormat = "mov,mp4,m4a,3gp,3g2,mj2";
    info.containerLongName = "QuickTime / MOV";
    info.duration = Duration::fromMilliseconds(2500);
    info.streams.push_back(videoStream("h264", Resolution{1920, 1080}, FrameRate::fps30()));
    info.streams.push_back(audioStream("aac"));
    return info;
}

/// A decodable audio-only container.
[[nodiscard]] media::MediaInfo audioOnly() {
    media::MediaInfo info;
    info.containerFormat = "wav";
    info.containerLongName = "WAV / WAVE";
    info.duration = Duration::fromMilliseconds(1200);
    media::MediaStreamInfo stream = audioStream("pcm_s16le");
    stream.index = 0;
    stream.duration = Duration::fromMilliseconds(1200);
    info.streams.push_back(stream);
    return info;
}

/// A container whose only video stream uses an undecodable codec, alongside a
/// decodable audio stream — accepted, but with no video parameters to report.
[[nodiscard]] media::MediaInfo undecodableVideoWithAudio() {
    media::MediaInfo info;
    info.containerFormat = "matroska,webm";
    info.containerLongName = "Matroska / WebM";
    info.duration = Duration::fromMilliseconds(3000);
    info.streams.push_back(videoStream("theora", Resolution{640, 360}, FrameRate::fps25()));
    info.streams.push_back(audioStream("opus"));
    return info;
}

/// A container in which every stream is undecodable.
[[nodiscard]] media::MediaInfo whollyUndecodable() {
    media::MediaInfo info;
    info.containerFormat = "matroska,webm";
    info.containerLongName = "Matroska / WebM";
    info.duration = Duration::fromMilliseconds(4000);
    info.streams.push_back(videoStream("theora", Resolution{640, 360}, FrameRate::fps25()));
    info.streams.push_back(audioStream("vorbis"));
    return info;
}

[[nodiscard]] media::MediaProbeBackend backendReturning(media::MediaInfo info) {
    return [info](const std::filesystem::path&) { return Result<media::MediaInfo>(info); };
}

[[nodiscard]] media::MediaProbeBackend backendFailing(ErrorCode code, std::string message) {
    return [code, message](const std::filesystem::path&) {
        return err<media::MediaInfo>(makeError(code, message));
    };
}

[[nodiscard]] bool contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class MediaImportServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        // The process id is part of the name because gtest_discover_tests runs the
        // binary once per test case and ctest runs those processes in parallel:
        // the gtest seed and the per-process counter are identical across them, so
        // without the pid two live cases would share — and, on TearDown, delete —
        // one fixture directory.
        dir_ = std::filesystem::temp_directory_path() /
               ("palmier_media_import_" + std::to_string(::getpid()) + "_" +
                std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                "_" + std::to_string(counter_++));
        std::filesystem::create_directories(dir_);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    /// Create a small non-empty file inside the fixture directory.
    [[nodiscard]] std::filesystem::path makeFile(const std::string& name) const {
        const std::filesystem::path path = dir_ / name;
        std::ofstream out(path, std::ios::binary);
        out << "not really media, the probe backend is injected";
        out.close();
        return path;
    }

    std::filesystem::path       dir_;
    static inline int           counter_ = 0;
};

// ---------------------------------------------------------------------------
// 2.1 / 2.2 — the accept path
// ---------------------------------------------------------------------------

TEST_F(MediaImportServiceTest, ImportRegistersExactlyOneAssetAndReportsItsMetadata) {
    ProjectSession session;
    MediaImportService service(session, backendReturning(mp4WithVideoAndAudio()));
    const std::filesystem::path file = makeFile("clip.mp4");

    ASSERT_EQ(session.mediaLibrary().assetCount(), 0u);
    ASSERT_FALSE(session.modified());

    const Result<ImportedAsset> imported = service.import(file);
    ASSERT_TRUE(imported.isOk()) << imported.error().toString();

    const ImportedAsset& asset = imported.value();
    EXPECT_FALSE(asset.assetId.isNil());
    EXPECT_FALSE(asset.duplicate);
    EXPECT_EQ(asset.sourcePath, std::filesystem::weakly_canonical(file));
    EXPECT_TRUE(asset.sourcePath.is_absolute());
    EXPECT_EQ(asset.containerFormat, "mov,mp4,m4a,3gp,3g2,mj2");
    EXPECT_EQ(asset.durationMs, 2500);
    EXPECT_TRUE(asset.hasVideo);
    EXPECT_TRUE(asset.hasAudio);
    ASSERT_TRUE(asset.resolution.has_value());
    EXPECT_EQ(*asset.resolution, Resolution(1920, 1080));
    ASSERT_TRUE(asset.frameRate.has_value());
    EXPECT_EQ(*asset.frameRate, FrameRate::fps30());

    // Exactly one library entry, naming the resolved location.
    ASSERT_EQ(session.mediaLibrary().assetCount(), 1u);
    EXPECT_TRUE(session.mediaLibrary().hasAsset(asset.assetId));
    EXPECT_EQ(session.mediaLibrary().library().front().sourcePath, asset.sourcePath.string());

    // The library changed outside the engine, so the session is modified.
    EXPECT_TRUE(session.modified());
    EXPECT_EQ(service.lastFailure(), ImportFailure::None);
    EXPECT_FALSE(service.isPending(file));
}

TEST_F(MediaImportServiceTest, AudioOnlyAssetReportsNoResolutionOrFrameRate) {
    ProjectSession session;
    MediaImportService service(session, backendReturning(audioOnly()));

    const Result<ImportedAsset> imported = service.import(makeFile("voice.wav"));
    ASSERT_TRUE(imported.isOk()) << imported.error().toString();

    EXPECT_FALSE(imported.value().hasVideo);
    EXPECT_TRUE(imported.value().hasAudio);
    EXPECT_FALSE(imported.value().resolution.has_value());
    EXPECT_FALSE(imported.value().frameRate.has_value());
    EXPECT_EQ(imported.value().durationMs, 1200);
    EXPECT_EQ(session.mediaLibrary().assetCount(), 1u);
}

TEST_F(MediaImportServiceTest, UndecodableVideoStreamReportsNoVideoParameters) {
    ProjectSession session;
    MediaImportService service(session, backendReturning(undecodableVideoWithAudio()));

    const Result<ImportedAsset> imported = service.import(makeFile("theora.mkv"));
    ASSERT_TRUE(imported.isOk()) << imported.error().toString();

    // The container carries a video stream, but not a DECODABLE one.
    EXPECT_FALSE(imported.value().hasVideo);
    EXPECT_TRUE(imported.value().hasAudio);
    EXPECT_FALSE(imported.value().resolution.has_value());
    EXPECT_FALSE(imported.value().frameRate.has_value());
    EXPECT_EQ(session.mediaLibrary().assetCount(), 1u);
}

TEST_F(MediaImportServiceTest, VideoWithoutDeclaredParametersReportsThemAbsent) {
    media::MediaInfo info = mp4WithVideoAndAudio();
    info.streams[0].resolution = Resolution{};   // the probe reported nothing usable
    info.streams[0].frameRate = FrameRate{};

    ProjectSession session;
    MediaImportService service(session, backendReturning(info));

    const Result<ImportedAsset> imported = service.import(makeFile("noparams.mp4"));
    ASSERT_TRUE(imported.isOk()) << imported.error().toString();
    EXPECT_TRUE(imported.value().hasVideo);
    EXPECT_FALSE(imported.value().resolution.has_value());
    EXPECT_FALSE(imported.value().frameRate.has_value());
}

// ---------------------------------------------------------------------------
// 2.3 — format rejections
// ---------------------------------------------------------------------------

TEST_F(MediaImportServiceTest, WhollyUndecodableContainerIsRejectedNamingFormatAndCodecs) {
    ProjectSession session;
    MediaImportService service(session, backendReturning(whollyUndecodable()));
    const std::filesystem::path file = makeFile("everything_rejected.mkv");

    const Result<ImportedAsset> imported = service.import(file);
    ASSERT_TRUE(imported.isError());
    EXPECT_EQ(imported.error().code(), ErrorCode::Unsupported);
    EXPECT_EQ(service.lastFailure(), ImportFailure::UnsupportedFormat);

    const std::string message = imported.error().message();
    EXPECT_TRUE(contains(message, file.filename().string())) << message;
    EXPECT_TRUE(contains(message, "matroska,webm")) << message;   // container named
    EXPECT_TRUE(contains(message, "Theora")) << message;          // rejected codecs named
    EXPECT_TRUE(contains(message, "Vorbis")) << message;

    EXPECT_EQ(session.mediaLibrary().assetCount(), 0u);
    EXPECT_TRUE(session.mediaLibrary().library().empty());
    EXPECT_FALSE(session.modified());
}

TEST_F(MediaImportServiceTest, UnrecognisedContainerIsRejectedAndChangesNothing) {
    ProjectSession session;
    MediaImportService service(
        session, backendFailing(ErrorCode::Unsupported, "unrecognized container format"));
    const std::filesystem::path file = makeFile("mystery.bin");

    const Result<ImportedAsset> imported = service.import(file);
    ASSERT_TRUE(imported.isError());
    EXPECT_EQ(imported.error().code(), ErrorCode::Unsupported);
    EXPECT_EQ(service.lastFailure(), ImportFailure::UnsupportedFormat);

    const std::string message = imported.error().message();
    EXPECT_TRUE(contains(message, file.filename().string())) << message;
    EXPECT_TRUE(contains(message, "container")) << message;
    EXPECT_TRUE(contains(message, "unrecognized container format")) << message;

    EXPECT_EQ(session.mediaLibrary().assetCount(), 0u);
    EXPECT_FALSE(session.modified());
}

// ---------------------------------------------------------------------------
// 2.4 — the four path conditions, each named
// ---------------------------------------------------------------------------

TEST_F(MediaImportServiceTest, EmptyPathIsRejectedAsEmpty) {
    ProjectSession session;
    MediaImportService service(session, backendReturning(mp4WithVideoAndAudio()));

    const Result<ImportedAsset> imported = service.import(std::filesystem::path{});
    ASSERT_TRUE(imported.isError());
    EXPECT_EQ(imported.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(service.lastFailure(), ImportFailure::EmptyPath);
    EXPECT_TRUE(contains(imported.error().message(), "empty")) << imported.error().message();
    EXPECT_EQ(session.mediaLibrary().assetCount(), 0u);
}

TEST_F(MediaImportServiceTest, MissingFileIsRejectedAsNonexistent) {
    ProjectSession session;
    MediaImportService service(session, backendReturning(mp4WithVideoAndAudio()));
    const std::filesystem::path missing = dir_ / "absent.mp4";

    const Result<ImportedAsset> imported = service.import(missing);
    ASSERT_TRUE(imported.isError());
    EXPECT_EQ(imported.error().code(), ErrorCode::NotFound);
    EXPECT_EQ(service.lastFailure(), ImportFailure::FileNotFound);
    EXPECT_TRUE(contains(imported.error().message(), "absent.mp4"))
        << imported.error().message();
    EXPECT_TRUE(contains(imported.error().message(), "does not exist"))
        << imported.error().message();
    EXPECT_EQ(session.mediaLibrary().assetCount(), 0u);
}

TEST_F(MediaImportServiceTest, UnopenableTargetIsRejectedAsUnreadable) {
    ProjectSession session;
    MediaImportService service(session, backendReturning(mp4WithVideoAndAudio()));
    const std::filesystem::path directory = dir_ / "not_a_file";
    std::filesystem::create_directories(directory);

    const Result<ImportedAsset> imported = service.import(directory);
    ASSERT_TRUE(imported.isError());
    EXPECT_EQ(imported.error().code(), ErrorCode::Io);
    EXPECT_EQ(service.lastFailure(), ImportFailure::FileUnreadable);
    EXPECT_TRUE(contains(imported.error().message(), "not_a_file"))
        << imported.error().message();
    EXPECT_TRUE(contains(imported.error().message(), "could not be opened or read"))
        << imported.error().message();
    EXPECT_EQ(session.mediaLibrary().assetCount(), 0u);
}

TEST_F(MediaImportServiceTest, DecodeFailureIsRejectedAsUndecodableAndChangesNothing) {
    ProjectSession session;
    MediaImportService::Options options;
    options.decodeCheck = [](const std::filesystem::path& path, const media::MediaInfo&) {
        return err<void>(makeError(ErrorCode::Io, "decoder failed on " + path.filename().string()));
    };
    MediaImportService service(session, backendReturning(mp4WithVideoAndAudio()),
                               std::move(options));
    const std::filesystem::path file = makeFile("broken.mp4");

    const Result<ImportedAsset> imported = service.import(file);
    ASSERT_TRUE(imported.isError());
    EXPECT_EQ(imported.error().code(), ErrorCode::Io);
    EXPECT_EQ(service.lastFailure(), ImportFailure::UndecodableContent);
    EXPECT_TRUE(contains(imported.error().message(), "broken.mp4")) << imported.error().message();
    EXPECT_TRUE(contains(imported.error().message(), "failed to decode"))
        << imported.error().message();
    EXPECT_EQ(session.mediaLibrary().assetCount(), 0u);
    EXPECT_FALSE(session.modified());
}

// ---------------------------------------------------------------------------
// 2.5 — duplicates converge on one entry
// ---------------------------------------------------------------------------

TEST_F(MediaImportServiceTest, SecondImportOfSameLocationReturnsExistingAssetAndAddsNoEntry) {
    ProjectSession session;
    auto probeCount = std::make_shared<std::atomic<int>>(0);
    const media::MediaInfo info = mp4WithVideoAndAudio();
    MediaImportService service(session, [probeCount, info](const std::filesystem::path&) {
        probeCount->fetch_add(1);
        return Result<media::MediaInfo>(info);
    });
    const std::filesystem::path file = makeFile("once.mp4");

    const Result<ImportedAsset> first = service.import(file);
    ASSERT_TRUE(first.isOk()) << first.error().toString();
    EXPECT_FALSE(first.value().duplicate);

    const Result<ImportedAsset> second = service.import(file);
    ASSERT_TRUE(second.isOk()) << second.error().toString();
    EXPECT_TRUE(second.value().duplicate);
    EXPECT_EQ(second.value().assetId, first.value().assetId);
    EXPECT_EQ(second.value().sourcePath, first.value().sourcePath);
    EXPECT_EQ(second.value().durationMs, first.value().durationMs);

    EXPECT_EQ(session.mediaLibrary().assetCount(), 1u);
    EXPECT_EQ(probeCount->load(), 1);
}

TEST_F(MediaImportServiceTest, DifferentSpellingsOfOneLocationConvergeOnOneEntry) {
    ProjectSession session;
    MediaImportService service(session, backendReturning(mp4WithVideoAndAudio()));
    const std::filesystem::path file = makeFile("spelled.mp4");

    // The media-browser action's spelling.
    const Result<ImportedAsset> viaBrowser = service.import(file);
    ASSERT_TRUE(viaBrowser.isOk()) << viaBrowser.error().toString();

    // The `media.import` tool's spelling of the same location: a redundant "." and
    // a parent-directory round trip.
    const std::filesystem::path indirect =
        dir_ / "." / "nested" / ".." / file.filename();
    std::filesystem::create_directories(dir_ / "nested");
    const Result<ImportedAsset> viaTool = service.import(indirect);
    ASSERT_TRUE(viaTool.isOk()) << viaTool.error().toString();

    EXPECT_TRUE(viaTool.value().duplicate);
    EXPECT_EQ(viaTool.value().assetId, viaBrowser.value().assetId);
    EXPECT_EQ(session.mediaLibrary().assetCount(), 1u);
}

TEST_F(MediaImportServiceTest, SymlinkToRegisteredLocationIsADuplicate) {
    ProjectSession session;
    MediaImportService service(session, backendReturning(mp4WithVideoAndAudio()));
    const std::filesystem::path file = makeFile("target.mp4");
    const std::filesystem::path link = dir_ / "link.mp4";

    std::error_code ec;
    std::filesystem::create_symlink(file, link, ec);
    if (ec) GTEST_SKIP() << "symlinks unavailable on this filesystem: " << ec.message();

    const Result<ImportedAsset> direct = service.import(file);
    ASSERT_TRUE(direct.isOk()) << direct.error().toString();

    const Result<ImportedAsset> viaLink = service.import(link);
    ASSERT_TRUE(viaLink.isOk()) << viaLink.error().toString();
    EXPECT_TRUE(viaLink.value().duplicate);
    EXPECT_EQ(viaLink.value().assetId, direct.value().assetId);
    EXPECT_EQ(session.mediaLibrary().assetCount(), 1u);
}

TEST_F(MediaImportServiceTest, DistinctLocationsEachGetTheirOwnEntry) {
    ProjectSession session;
    MediaImportService service(session, backendReturning(mp4WithVideoAndAudio()));

    const Result<ImportedAsset> a = service.import(makeFile("a.mp4"));
    const Result<ImportedAsset> b = service.import(makeFile("b.mp4"));
    ASSERT_TRUE(a.isOk());
    ASSERT_TRUE(b.isOk());
    EXPECT_NE(a.value().assetId, b.value().assetId);
    EXPECT_EQ(session.mediaLibrary().assetCount(), 2u);
}

// ---------------------------------------------------------------------------
// 2.7 — no project open
// ---------------------------------------------------------------------------

TEST_F(MediaImportServiceTest, NoProjectOpenRegistersNothingAndSaysSo) {
    MediaImportService service(static_cast<ProjectSession*>(nullptr),
                               backendReturning(mp4WithVideoAndAudio()));
    const std::filesystem::path file = makeFile("orphan.mp4");

    const Result<ImportedAsset> imported = service.import(file);
    ASSERT_TRUE(imported.isError());
    EXPECT_EQ(imported.error().code(), ErrorCode::FailedPrecondition);
    EXPECT_EQ(service.lastFailure(), ImportFailure::NoProjectOpen);
    EXPECT_TRUE(contains(imported.error().message(), "no project is open"))
        << imported.error().message();
    EXPECT_FALSE(service.isPending(file));
}

// ---------------------------------------------------------------------------
// 2.8 — the time limit
// ---------------------------------------------------------------------------

TEST_F(MediaImportServiceTest, ProbeExceedingTheTimeLimitIsAbandonedAndChangesNothing) {
    /// A gate the probe blocks on until the test releases it. Shared by
    /// shared_ptr so the abandoned worker cannot outlive what it touches.
    struct Gate {
        std::mutex              mutex;
        std::condition_variable cv;
        bool                    open = false;

        void wait() {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait(lock, [this] { return open; });
        }
        void release() {
            {
                const std::lock_guard<std::mutex> lock(mutex);
                open = true;
            }
            cv.notify_all();
        }
    };
    auto gate = std::make_shared<Gate>();

    ProjectSession session;
    MediaImportService::Options options;
    options.timeout = 50ms;
    const media::MediaInfo info = mp4WithVideoAndAudio();
    MediaImportService service(
        session,
        [gate, info](const std::filesystem::path&) {
            gate->wait();
            return Result<media::MediaInfo>(info);
        },
        std::move(options));
    const std::filesystem::path file = makeFile("slow.mp4");

    EXPECT_EQ(service.timeout(), 50ms);
    const Result<ImportedAsset> imported = service.import(file);
    gate->release();

    ASSERT_TRUE(imported.isError());
    EXPECT_EQ(imported.error().code(), ErrorCode::Timeout);
    EXPECT_EQ(service.lastFailure(), ImportFailure::TimedOut);
    EXPECT_TRUE(contains(imported.error().message(), "slow.mp4")) << imported.error().message();
    EXPECT_TRUE(contains(imported.error().message(), "time limit"))
        << imported.error().message();

    EXPECT_EQ(session.mediaLibrary().assetCount(), 0u);
    EXPECT_TRUE(session.mediaLibrary().library().empty());
    EXPECT_FALSE(session.modified());
}

TEST_F(MediaImportServiceTest, DefaultTimeLimitIsThirtySeconds) {
    ProjectSession session;
    MediaImportService service(session, backendReturning(mp4WithVideoAndAudio()));
    EXPECT_EQ(service.timeout(), 30s);
}

// ---------------------------------------------------------------------------
// 2.9 — pending while in progress
// ---------------------------------------------------------------------------

TEST_F(MediaImportServiceTest, ImportInProgressIsPendingAndExposesNoLibraryEntry) {
    struct Observed {
        std::atomic<bool>        pending{false};
        std::atomic<std::size_t> entries{999};
        std::atomic<std::size_t> pendingCount{999};
    };
    auto observed = std::make_shared<Observed>();

    ProjectSession session;
    // The service and the probe are mutually referential, so the pointer is
    // published into the backend after construction.
    auto slot = std::make_shared<std::atomic<MediaImportService*>>(nullptr);
    const std::filesystem::path file = makeFile("inflight.mp4");

    MediaImportService service(
        session, [slot, observed, &session](const std::filesystem::path& path) {
            MediaImportService* self = slot->load();
            if (self != nullptr) {
                observed->pending.store(self->isPending(path));
                observed->pendingCount.store(self->pendingCount());
            }
            observed->entries.store(session.mediaLibrary().assetCount());
            return Result<media::MediaInfo>(mp4WithVideoAndAudio());
        });
    slot->store(&service);

    EXPECT_FALSE(service.isPending(file));
    EXPECT_EQ(service.pendingCount(), 0u);

    const Result<ImportedAsset> imported = service.import(file);
    ASSERT_TRUE(imported.isOk()) << imported.error().toString();

    // Observed from inside the probe: the import was pending, and no library
    // entry existed for it yet.
    EXPECT_TRUE(observed->pending.load());
    EXPECT_EQ(observed->pendingCount.load(), 1u);
    EXPECT_EQ(observed->entries.load(), 0u);

    // Registration has completed, so it is no longer pending and the entry is up.
    EXPECT_FALSE(service.isPending(file));
    EXPECT_EQ(service.pendingCount(), 0u);
    EXPECT_EQ(session.mediaLibrary().assetCount(), 1u);
    EXPECT_TRUE(session.mediaLibrary().hasAsset(imported.value().assetId));
}

TEST_F(MediaImportServiceTest, RejectedImportLeavesNothingPending) {
    ProjectSession session;
    MediaImportService service(session, backendReturning(whollyUndecodable()));
    const std::filesystem::path file = makeFile("rejected.mkv");

    const Result<ImportedAsset> imported = service.import(file);
    ASSERT_TRUE(imported.isError());
    EXPECT_FALSE(service.isPending(file));
    EXPECT_EQ(service.pendingCount(), 0u);
}

// ---------------------------------------------------------------------------
// Path resolution helper
// ---------------------------------------------------------------------------

TEST_F(MediaImportServiceTest, ResolvePathIsAbsoluteAndCanonical) {
    const std::filesystem::path file = makeFile("resolve.mp4");
    const std::filesystem::path resolved =
        MediaImportService::resolvePath(dir_ / "." / file.filename());
    EXPECT_TRUE(resolved.is_absolute());
    EXPECT_EQ(resolved, std::filesystem::weakly_canonical(file));

    // A path that does not exist still resolves (weakly), and an empty path stays
    // empty so it can be reported as such.
    EXPECT_TRUE(MediaImportService::resolvePath(dir_ / "ghost.mp4").is_absolute());
    EXPECT_TRUE(MediaImportService::resolvePath(std::filesystem::path{}).empty());
}

} // namespace
} // namespace palmier::services
