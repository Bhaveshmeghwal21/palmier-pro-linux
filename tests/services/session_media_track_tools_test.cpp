// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/services/session_media_track_tools_test.cpp — unit tests for the tools
// tasks 4.3, 4.4 and 4.5 add to the shared tool surface:
//
//   * task 4.3 — `project.create`, `project.open`, `project.save`, `project.info`,
//     the session-level tools. They are NOT EditCommands and are NOT undoable
//     (design.md D1): ProjectSession builds a complete Project value locally and
//     commits it with TimelineEngine::reset only on full success. Requirements
//     3.1, 3.2, 3.4, 3.5, 3.8, 3.9.
//   * task 4.4 — `media.import` and `media.list`, wired to MediaImportService
//     through the `importMedia` hook, reporting the fields Requirement 2.2 lists
//     (with the resolution and frame rate absent for an asset carrying no
//     decodable video stream). Requirements 2.2, 3.1.
//   * task 4.5 — `timeline.add_track` and `timeline.remove_track`, backed by the
//     core AddTrackCommand / RemoveTrackCommand, so both are atomic,
//     invariant-checked and undoable through the same path as every clip edit.
//     Requirements 3.1, 3.3, 3.8, 3.10.
//
// Requirement 3.5 gets its own case: with no project current — modelled, as
// McpToolExecutor and MediaImportService already model it, by a null session — every
// tool OTHER than `project.create` and `project.open` refuses with "no project is
// open", and those two are demonstrably exempt.
//
// The media tools are exercised through a REAL MediaImportService over
// MediaProbe's injectable backend seam, so the asset-description fields the tool
// reports are the ones a genuine import produces, not a hand-built stand-in.
//
// _Requirements: 2.2, 3.1, 3.2, 3.3, 3.4, 3.5, 3.8, 3.9, 3.10_

#include "services/ToolRegistry.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h> // getpid, for a per-process fixture directory name

#include "core/Clip.hpp"
#include "core/ColorSpace.hpp"
#include "core/Duration.hpp"
#include "core/EditCommands.hpp"
#include "core/Error.hpp"
#include "core/FrameRate.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/Project.hpp"
#include "core/Resolution.hpp"
#include "core/TimelineEngine.hpp"
#include "core/Track.hpp"
#include "core/Uuid.hpp"
#include "media/MediaInfo.hpp"
#include "media/MediaProbe.hpp"
#include "services/Json.hpp"
#include "services/MediaImportService.hpp"
#include "services/ProjectSession.hpp"
#include "services/ToolSchema.hpp"

namespace palmier::services {
namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

[[nodiscard]] bool contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

[[nodiscard]] Json createArgs(std::string name, double fps, std::int64_t width,
                              std::int64_t height) {
    Json args = Json::object();
    args.set("name", std::move(name));
    args.set("fps", fps);
    args.set("width", width);
    args.set("height", height);
    return args;
}

[[nodiscard]] Json pathArgs(const std::filesystem::path& path) {
    Json args = Json::object();
    args.set("path", path.string());
    return args;
}

[[nodiscard]] Json kindArgs(std::string kind) {
    Json args = Json::object();
    args.set("kind", std::move(kind));
    return args;
}

/// Invoke `tool` and require success, reporting the error otherwise. The result is
/// returned for the cases that assert on it and may be ignored by the cases that
/// only need the effect.
Json invokeOk(const ToolRegistry& registry, std::string_view tool, const Json& args) {
    Result<Json> result = registry.invoke(tool, args);
    EXPECT_TRUE(result.isOk()) << tool << ": " << result.error().toString();
    return result.isOk() ? result.value() : Json::object();
}

/// The clip count across every track of `project`.
[[nodiscard]] std::size_t clipCount(const Project& project) {
    std::size_t count = 0;
    for (const Track& track : project.tracks) count += track.clips.size();
    return count;
}

[[nodiscard]] Clip makeClip(const Uuid& assetId, std::int64_t startNs, std::int64_t lengthNs) {
    Clip clip;
    clip.id = Uuid::generateV4();
    clip.assetRef = MediaAssetRef(assetId, "/media/source.mp4");
    clip.timelineStart = Duration::fromNanoseconds(startNs);
    clip.sourceIn = Duration::zero();
    clip.sourceOut = Duration::fromNanoseconds(lengthNs);
    return clip;
}

// ---------------------------------------------------------------------------
// Synthetic probed containers (the media tools' inputs)
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

[[nodiscard]] media::MediaStreamInfo audioStream(std::string codecName, int index) {
    media::MediaStreamInfo stream;
    stream.index = index;
    stream.type = media::MediaStreamType::Audio;
    stream.codecName = std::move(codecName);
    stream.sampleRate = 48000;
    stream.channels = 2;
    stream.duration = Duration::fromMilliseconds(2500);
    return stream;
}

/// A decodable H.264 + AAC MP4 of 2.5 s at 1920x1080 / 30 fps.
[[nodiscard]] media::MediaInfo mp4WithVideoAndAudio() {
    media::MediaInfo info;
    info.containerFormat = "mov,mp4,m4a,3gp,3g2,mj2";
    info.containerLongName = "QuickTime / MOV";
    info.duration = Duration::fromMilliseconds(2500);
    info.streams.push_back(videoStream("h264", Resolution{1920, 1080}, FrameRate::fps30()));
    info.streams.push_back(audioStream("aac", 1));
    return info;
}

/// A decodable audio-only WAV: Requirement 2.2's "no decodable video stream" case.
[[nodiscard]] media::MediaInfo audioOnlyWav() {
    media::MediaInfo info;
    info.containerFormat = "wav";
    info.containerLongName = "WAV / WAVE";
    info.duration = Duration::fromMilliseconds(1200);
    media::MediaStreamInfo stream = audioStream("pcm_s16le", 0);
    stream.duration = Duration::fromMilliseconds(1200);
    info.streams.push_back(stream);
    return info;
}

/// A container in which every stream uses a codec the engine cannot decode.
[[nodiscard]] media::MediaInfo whollyUndecodable() {
    media::MediaInfo info;
    info.containerFormat = "matroska,webm";
    info.containerLongName = "Matroska / WebM";
    info.duration = Duration::fromMilliseconds(4000);
    info.streams.push_back(videoStream("theora", Resolution{640, 360}, FrameRate::fps25()));
    info.streams.push_back(audioStream("vorbis", 1));
    return info;
}

[[nodiscard]] media::MediaProbeBackend backendReturning(media::MediaInfo info) {
    return [info](const std::filesystem::path&) { return Result<media::MediaInfo>(info); };
}

// ---------------------------------------------------------------------------
// Fixture: a session, a registry over it, and a scratch directory for documents
// and media files.
// ---------------------------------------------------------------------------

class SessionToolsTest : public ::testing::Test {
protected:
    void SetUp() override {
        // The pid and the gtest seed are part of the name because
        // gtest_discover_tests runs the binary once per case and ctest runs those
        // processes in parallel.
        dir_ = std::filesystem::temp_directory_path() /
               ("palmier_session_tools_" + std::to_string(::getpid()) + "_" +
                std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" +
                std::to_string(counter_++));
        std::filesystem::create_directories(dir_);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    /// A real file of `bytes` bytes, so the import access check passes.
    [[nodiscard]] std::filesystem::path writeFile(std::string_view name,
                                                  std::size_t bytes = 64) const {
        const std::filesystem::path path = dir_ / name;
        std::ofstream out(path, std::ios::binary);
        out << std::string(bytes, '\0');
        return path;
    }

    std::filesystem::path dir_;
    static int            counter_;
};

int SessionToolsTest::counter_ = 0;

// ===========================================================================
// Task 4.3 — project.create (Requirements 3.2, 3.8)
// ===========================================================================

TEST_F(SessionToolsTest, ProjectCreateCarriesExactlyTheRequestedSettings) {
    ProjectSession     session;
    const ToolRegistry registry = buildDefaultToolRegistry(session);

    Json args = createArgs("Documentary Cut", 24.0, 1920, 1080);
    args.set("colorSpace", std::string("Rec.2020"));
    const Json result = invokeOk(registry, "project.create", args);

    // The reported settings are the requested ones.
    EXPECT_EQ(result.stringOr("name"), "Documentary Cut");
    ASSERT_NE(result.find("fps"), nullptr);
    EXPECT_EQ(result.find("fps")->intOr("numerator"), 24);
    EXPECT_EQ(result.find("fps")->intOr("denominator"), 1);
    ASSERT_NE(result.find("canvas"), nullptr);
    EXPECT_EQ(result.find("canvas")->intOr("width"), 1920);
    EXPECT_EQ(result.find("canvas")->intOr("height"), 1080);
    EXPECT_EQ(result.stringOr("colorSpace"), "Rec.2020");
    EXPECT_FALSE(result.boolOr("modified", true));
    ASSERT_NE(result.find("documentPath"), nullptr);
    EXPECT_TRUE(result.find("documentPath")->isNull());

    // ... and they are the settings the project actually carries.
    const Project project = session.engine().snapshot();
    EXPECT_EQ(project.name, "Documentary Cut");
    EXPECT_EQ(project.timelineFps, FrameRate::fps24());
    EXPECT_EQ(project.canvas, Resolution(1920, 1080));
    EXPECT_EQ(project.colorSpace, ColorSpace::Rec2020);
    EXPECT_EQ(project.id.toString(), result.stringOr("projectId"));
    EXPECT_FALSE(session.modified());
    EXPECT_FALSE(session.documentPath().has_value());
}

TEST_F(SessionToolsTest, ProjectCreateDefaultsTheColorSpace) {
    ProjectSession     session;
    const ToolRegistry registry = buildDefaultToolRegistry(session);

    const Json result = invokeOk(registry, "project.create", createArgs("Untitled", 30.0, 640, 360));
    EXPECT_EQ(result.stringOr("colorSpace"), "Rec.709");
    EXPECT_EQ(session.engine().snapshot().colorSpace, ColorSpace::Rec709);
}

TEST_F(SessionToolsTest, ProjectCreateTakesFractionalRatesToTheirExactRational) {
    ProjectSession     session;
    const ToolRegistry registry = buildDefaultToolRegistry(session);

    // 29.97 is the NTSC pull-down rate 30000/1001, not 2997/100: a project that
    // recorded the decimal approximation would drift against real media.
    invokeOk(registry, "project.create", createArgs("NTSC", 30000.0 / 1001.0, 1280, 720));
    EXPECT_EQ(session.engine().snapshot().timelineFps, FrameRate::fps29_97());

    invokeOk(registry, "project.create", createArgs("Half", 12.5, 1280, 720));
    EXPECT_EQ(session.engine().snapshot().timelineFps, FrameRate(25, 2));
}

TEST_F(SessionToolsTest, ProjectCreateRejectsOutOfRangeArgumentsAndChangesNothing) {
    ProjectSession     session;
    const ToolRegistry registry = buildDefaultToolRegistry(session);
    const ToolSchema&  schema = registry.find("project.create")->schema;

    const Project before = session.engine().snapshot();

    struct Case {
        const char* what;
        Json        args;
    };
    std::vector<Case> cases;
    cases.push_back({"empty name", createArgs("", 30.0, 1920, 1080)});
    cases.push_back({"over-long name", createArgs(std::string(129, 'x'), 30.0, 1920, 1080)});
    cases.push_back({"frame rate below 1", createArgs("p", 0.5, 1920, 1080)});
    cases.push_back({"frame rate above 240", createArgs("p", 240.5, 1920, 1080)});
    cases.push_back({"canvas too small", createArgs("p", 30.0, 8, 1080)});
    cases.push_back({"canvas too tall", createArgs("p", 30.0, 1920, 4321)});
    Json badColorSpace = createArgs("p", 30.0, 1920, 1080);
    badColorSpace.set("colorSpace", std::string("Rec.42"));
    cases.push_back({"unknown colour space", std::move(badColorSpace)});
    // Far outside, not just outside: a handler reached directly must not convert
    // these to a rational or an unsigned canvas at all.
    cases.push_back({"absurd frame rate", createArgs("p", 1e12, 1920, 1080)});
    cases.push_back({"absurd canvas width",
                     createArgs("p", 30.0, static_cast<std::int64_t>(1) << 34, 1080)});
    cases.push_back({"negative canvas height", createArgs("p", 30.0, 1920, -1080)});

    for (const Case& c : cases) {
        // Requirement 3.8's bounds are declared, so the published schema rejects
        // the argument before any handler runs and names it.
        const Result<void> validated = schema.validate(c.args);
        EXPECT_TRUE(validated.isError()) << c.what;

        // And the handler refuses too, leaving the project byte-identical.
        const Result<Json> invoked = registry.invoke("project.create", c.args);
        ASSERT_TRUE(invoked.isError()) << c.what;
        const Project after = session.engine().snapshot();
        EXPECT_EQ(after.id, before.id) << c.what;
        EXPECT_EQ(after.name, before.name) << c.what;
        EXPECT_EQ(after.timelineFps, before.timelineFps) << c.what;
        EXPECT_EQ(after.canvas, before.canvas) << c.what;
        EXPECT_EQ(session.engine().undoDepth(), 0u) << c.what;
    }
}

TEST_F(SessionToolsTest, ProjectCreateIsNotUndoable) {
    ProjectSession     session;
    const ToolRegistry registry = buildDefaultToolRegistry(session);

    // An edit first, so there IS history to lose.
    invokeOk(registry, "timeline.add_track", kindArgs("video"));
    ASSERT_EQ(session.engine().undoDepth(), 1u);

    invokeOk(registry, "project.create", createArgs("Fresh", 30.0, 1920, 1080));

    // A create records no command of its own and clears the history it replaced.
    EXPECT_EQ(session.engine().undoDepth(), 0u);
    EXPECT_FALSE(session.engine().canUndo());
    EXPECT_TRUE(session.engine().snapshot().tracks.empty());
}

// ===========================================================================
// Task 4.3 — project.save and project.open (Requirements 3.4, 3.9)
// ===========================================================================

TEST_F(SessionToolsTest, ProjectSaveThenOpenReportsTheLoadedProjectAccurately) {
    const std::filesystem::path document = dir_ / "round-trip.palmier";

    Uuid savedId;
    {
        ProjectSession     session;
        const ToolRegistry registry = buildDefaultToolRegistry(session);
        const Json created =
            invokeOk(registry, "project.create", createArgs("Saved Cut", 30.0, 1280, 720));
        savedId = *Uuid::parse(created.stringOr("projectId"));

        const Json track = invokeOk(registry, "timeline.add_track", kindArgs("video"));
        Project    withClip = session.engine().snapshot();
        ASSERT_EQ(withClip.tracks.size(), 1u);
        withClip.assets.push_back(MediaAssetRef(Uuid::generateV4(), "/media/source.mp4"));
        withClip.tracks[0].clips.push_back(makeClip(withClip.assets[0].assetId, 0, 1'000'000'000));
        ASSERT_TRUE(session.engine().reset(withClip).isOk());
        EXPECT_EQ(track.stringOr("kind"), "video");

        const Json saved = invokeOk(registry, "project.save", pathArgs(document));
        EXPECT_EQ(saved.stringOr("documentPath"), document.string());
        EXPECT_GT(saved.intOr("bytesWritten"), 0);
        EXPECT_FALSE(saved.boolOr("modified", true));
        EXPECT_FALSE(session.modified());
        EXPECT_EQ(session.documentPath(), document);
    }

    ASSERT_TRUE(std::filesystem::exists(document));

    ProjectSession     session;
    const ToolRegistry registry = buildDefaultToolRegistry(session);
    const Json opened = invokeOk(registry, "project.open", pathArgs(document));

    EXPECT_EQ(opened.stringOr("projectId"), savedId.toString());
    EXPECT_EQ(opened.stringOr("name"), "Saved Cut");
    EXPECT_EQ(opened.intOr("trackCount"), 1);
    EXPECT_EQ(opened.intOr("clipCount"), 1);
    EXPECT_EQ(opened.stringOr("documentPath"), document.string());
    EXPECT_FALSE(opened.boolOr("modified", true));

    EXPECT_FALSE(session.modified());
    EXPECT_EQ(session.engine().snapshot().tracks.size(), 1u);
    EXPECT_EQ(session.engine().undoDepth(), 0u);
}

TEST_F(SessionToolsTest, ProjectSaveDefaultsToTheRecordedDocumentPath) {
    const std::filesystem::path document = dir_ / "resave.palmier";

    ProjectSession     session;
    const ToolRegistry registry = buildDefaultToolRegistry(session);
    invokeOk(registry, "project.create", createArgs("Resaved", 30.0, 640, 480));
    invokeOk(registry, "project.save", pathArgs(document));

    invokeOk(registry, "timeline.add_track", kindArgs("audio"));
    ASSERT_TRUE(session.modified());

    // No `path`: the recorded document path is the destination.
    const Json resaved = invokeOk(registry, "project.save", Json::object());
    EXPECT_EQ(resaved.stringOr("documentPath"), document.string());
    EXPECT_FALSE(resaved.boolOr("modified", true));
    EXPECT_FALSE(session.modified());
}

TEST_F(SessionToolsTest, ProjectSaveWithoutAnyDestinationIsRefused) {
    ProjectSession     session;
    const ToolRegistry registry = buildDefaultToolRegistry(session);

    const Result<Json> refused = registry.invoke("project.save", Json::object());
    ASSERT_TRUE(refused.isError());
    EXPECT_EQ(refused.error().code(), ErrorCode::FailedPrecondition);
    EXPECT_TRUE(contains(refused.error().message(), "document path"));
    EXPECT_FALSE(session.documentPath().has_value());
}

TEST_F(SessionToolsTest, FailedProjectOpenPreservesTheCurrentProjectExactly) {
    ProjectSession     session;
    const ToolRegistry registry = buildDefaultToolRegistry(session);
    invokeOk(registry, "project.create", createArgs("Keep Me", 25.0, 1024, 576));
    invokeOk(registry, "timeline.add_track", kindArgs("video"));

    const Project      before = session.engine().snapshot();
    const std::size_t  historyBefore = session.engine().undoDepth();
    const bool         modifiedBefore = session.modified();

    // Requirement 3.9's four failure classes: absent, unreadable (a directory
    // stands in for "cannot be opened"), and not a valid .palmier document.
    const std::filesystem::path absent = dir_ / "nope.palmier";
    const std::filesystem::path garbage = dir_ / "garbage.palmier";
    { std::ofstream out(garbage); out << "this is not a project document"; }

    for (const std::filesystem::path& target : {absent, dir_, garbage}) {
        const Result<Json> failed = registry.invoke("project.open", pathArgs(target));
        ASSERT_TRUE(failed.isError()) << target;
        EXPECT_TRUE(contains(failed.error().message(), target.string())) << target;

        const Project after = session.engine().snapshot();
        EXPECT_EQ(after.id, before.id) << target;
        EXPECT_EQ(after.name, before.name) << target;
        EXPECT_EQ(after.tracks.size(), before.tracks.size()) << target;
        EXPECT_EQ(session.engine().undoDepth(), historyBefore) << target;
        EXPECT_EQ(session.modified(), modifiedBefore) << target;
        EXPECT_FALSE(session.documentPath().has_value()) << target;
    }
}

// ===========================================================================
// Task 4.3 — project.info (Requirement 3.7's counts, reported per call)
// ===========================================================================

TEST_F(SessionToolsTest, ProjectInfoReportsSettingsCountsHistoryAndModifiedState) {
    ProjectSession     session;
    const ToolRegistry registry = buildDefaultToolRegistry(session);
    invokeOk(registry, "project.create", createArgs("Info", 50.0, 3840, 2160));

    const Json fresh = invokeOk(registry, "project.info", Json::object());
    EXPECT_EQ(fresh.stringOr("name"), "Info");
    ASSERT_NE(fresh.find("fps"), nullptr);
    EXPECT_EQ(fresh.find("fps")->intOr("numerator"), 50);
    EXPECT_EQ(fresh.find("canvas")->intOr("width"), 3840);
    EXPECT_EQ(fresh.stringOr("colorSpace"), "Rec.709");
    EXPECT_EQ(fresh.intOr("trackCount"), 0);
    EXPECT_EQ(fresh.intOr("clipCount"), 0);
    EXPECT_EQ(fresh.intOr("assetCount"), 0);
    EXPECT_EQ(fresh.intOr("durationNs"), 0);
    EXPECT_EQ(fresh.intOr("undoDepth"), 0);
    EXPECT_FALSE(fresh.boolOr("modified", true));
    EXPECT_TRUE(fresh.find("documentPath")->isNull());

    // Register an asset for the clip to reference (a reset is not an edit, so it
    // leaves the history empty), then apply exactly two edits through the tools.
    const Uuid assetId = Uuid::generateV4();
    Project    withAsset = session.engine().snapshot();
    withAsset.assets.push_back(MediaAssetRef(assetId, "/media/a.mp4"));
    ASSERT_TRUE(session.engine().reset(withAsset).isOk());
    ASSERT_EQ(session.engine().undoDepth(), 0u);

    const Json track = invokeOk(registry, "timeline.add_track", kindArgs("video"));
    Json clipArgs = Json::object();
    clipArgs.set("trackId", track.stringOr("trackId"));
    clipArgs.set("assetId", assetId.toString());
    clipArgs.set("sourceOutNs", static_cast<std::int64_t>(2'000'000'000));
    invokeOk(registry, "timeline.add_clip", clipArgs);

    const Json busy = invokeOk(registry, "project.info", Json::object());
    EXPECT_EQ(busy.intOr("trackCount"), 1);
    EXPECT_EQ(busy.intOr("clipCount"), 1);
    EXPECT_EQ(busy.intOr("durationNs"), 2'000'000'000);
    EXPECT_EQ(busy.intOr("undoDepth"), 2);
    EXPECT_TRUE(busy.boolOr("modified"));
}

// ===========================================================================
// Task 4.3 — Requirement 3.5: no project open blocks every other tool
// ===========================================================================

TEST_F(SessionToolsTest, NoProjectOpenBlocksEveryToolExceptCreateAndOpen) {
    // A registry over no session at all is the "no project is current" state.
    const ToolRegistry registry = buildDefaultToolRegistry(nullptr);
    ASSERT_GT(registry.size(), 0u);

    std::size_t blocked = 0;
    for (const Tool& tool : registry.tools()) {
        const Result<Json> invoked = registry.invoke(tool.name, Json::object());
        ASSERT_TRUE(invoked.isError()) << tool.name;

        if (tool.name == "project.create" || tool.name == "project.open") {
            // Exempt: these two exist to MAKE a project current, so they never
            // report "no project is open".
            EXPECT_FALSE(contains(invoked.error().message(), "no project is open")) << tool.name;
            EXPECT_EQ(invoked.error().code(), ErrorCode::Unsupported) << tool.name;
            continue;
        }

        ++blocked;
        EXPECT_EQ(invoked.error().code(), ErrorCode::FailedPrecondition) << tool.name;
        EXPECT_TRUE(contains(invoked.error().message(), "no project is open")) << tool.name;
        EXPECT_TRUE(contains(invoked.error().message(), tool.name)) << tool.name;
    }
    EXPECT_EQ(blocked, registry.size() - 2);
}

TEST_F(SessionToolsTest, NoProjectOpenRefusalPrecedesArgumentParsingAndTheHooks) {
    // A hook that must never be reached while no project is current: the refusal
    // happens before any argument is parsed and before the hook can run, so no
    // state anywhere can have changed (Requirement 3.5).
    bool             hookRan = false;
    ToolRegistryHooks hooks;
    hooks.exportTimeline = [&hookRan](const Json&) -> Result<Json> {
        hookRan = true;
        return Json::object();
    };
    hooks.importMedia = [&hookRan](const std::filesystem::path&) -> Result<ImportedAsset> {
        hookRan = true;
        return ImportedAsset{};
    };
    const ToolRegistry registry = buildDefaultToolRegistry(nullptr, std::move(hooks));

    // Well-formed arguments, so nothing else could be refusing them.
    Json exportArgs = Json::object();
    exportArgs.set("outputPath", std::string("/tmp/out.mp4"));
    exportArgs.set("format", std::string("mp4"));
    EXPECT_TRUE(registry.invoke("timeline.export", exportArgs).isError());
    EXPECT_TRUE(registry.invoke("media.import", pathArgs("/media/a.mp4")).isError());
    EXPECT_FALSE(hookRan);
}

TEST_F(SessionToolsTest, CreateAndOpenRunThroughTheirHooksWithoutASession) {
    // The exemption is usable, not just differently worded: a surface that owns
    // session creation supplies the two hooks and they run with no session bound.
    int               created = 0;
    ToolRegistryHooks hooks;
    hooks.createProject = [&created](const Json& in) -> Result<Json> {
        ++created;
        Json out = Json::object();
        out.set("projectId", in.stringOr("name"));
        return out;
    };
    const ToolRegistry registry = buildDefaultToolRegistry(nullptr, std::move(hooks));

    const Json result =
        invokeOk(registry, "project.create", createArgs("hooked", 30.0, 1920, 1080));
    EXPECT_EQ(result.stringOr("projectId"), "hooked");
    EXPECT_EQ(created, 1);
}

// ===========================================================================
// Task 4.4 — media.import and media.list (Requirement 2.2)
// ===========================================================================

/// A registry whose `media.import` runs a REAL MediaImportService over a probe
/// backend returning `info`.
class MediaToolsTest : public SessionToolsTest {
protected:
    void wire(media::MediaInfo info) {
        service_ = std::make_unique<MediaImportService>(session_, backendReturning(std::move(info)));
        ToolRegistryHooks hooks;
        MediaImportService* service = service_.get();
        hooks.importMedia = [service](const std::filesystem::path& path) {
            return service->import(path);
        };
        registry_ = std::make_unique<ToolRegistry>(
            buildDefaultToolRegistry(session_, std::move(hooks)));
    }

    [[nodiscard]] const ToolRegistry& registry() const { return *registry_; }

    ProjectSession                      session_;
    std::unique_ptr<MediaImportService>  service_;
    std::unique_ptr<ToolRegistry>        registry_;
};

TEST_F(MediaToolsTest, ImportReportsEveryFieldOfADecodableAudioVideoAsset) {
    wire(mp4WithVideoAndAudio());
    const std::filesystem::path file = writeFile("clip.mp4");

    const Json result = invokeOk(registry(), "media.import", pathArgs(file));

    EXPECT_TRUE(Uuid::parse(result.stringOr("assetId")).has_value());
    EXPECT_EQ(result.stringOr("sourcePath"),
              MediaImportService::resolvePath(file).string());
    EXPECT_EQ(result.stringOr("containerFormat"), "mov,mp4,m4a,3gp,3g2,mj2");
    EXPECT_EQ(result.intOr("durationMs"), 2500);
    EXPECT_EQ(result.intOr("width"), 1920);
    EXPECT_EQ(result.intOr("height"), 1080);
    EXPECT_DOUBLE_EQ(result.doubleOr("fps"), 30.0);
    EXPECT_TRUE(result.boolOr("hasVideo"));
    EXPECT_TRUE(result.boolOr("hasAudio"));
    EXPECT_FALSE(result.boolOr("duplicate", true));

    // Exactly one asset is registered, and it is the one reported.
    ASSERT_EQ(session_.mediaLibrary().assetCount(), 1u);
    EXPECT_EQ(session_.mediaLibrary().library()[0].assetId.toString(),
              result.stringOr("assetId"));
}

TEST_F(MediaToolsTest, AudioOnlyImportOmitsResolutionAndFrameRate) {
    wire(audioOnlyWav());
    const Json result = invokeOk(registry(), "media.import", pathArgs(writeFile("voice.wav")));

    // Requirement 2.2: absent, not null, so "audio only" is distinguishable.
    EXPECT_EQ(result.find("width"), nullptr);
    EXPECT_EQ(result.find("height"), nullptr);
    EXPECT_EQ(result.find("fps"), nullptr);
    EXPECT_FALSE(result.boolOr("hasVideo", true));
    EXPECT_TRUE(result.boolOr("hasAudio"));
    EXPECT_EQ(result.intOr("durationMs"), 1200);
}

TEST_F(MediaToolsTest, ReimportingTheSameLocationReportsTheDuplicateAndAddsNoEntry) {
    wire(mp4WithVideoAndAudio());
    const std::filesystem::path file = writeFile("once.mp4");

    const Json first = invokeOk(registry(), "media.import", pathArgs(file));
    // A different spelling of the same location.
    const Json second =
        invokeOk(registry(), "media.import", pathArgs(dir_ / "." / "once.mp4"));

    EXPECT_FALSE(first.boolOr("duplicate", true));
    EXPECT_TRUE(second.boolOr("duplicate"));
    EXPECT_EQ(second.stringOr("assetId"), first.stringOr("assetId"));
    EXPECT_EQ(session_.mediaLibrary().assetCount(), 1u);
}

TEST_F(MediaToolsTest, RejectedImportLeavesTheLibraryUnchanged) {
    wire(whollyUndecodable());
    const std::filesystem::path file = writeFile("undecodable.mkv");

    const Result<Json> rejected = registry().invoke("media.import", pathArgs(file));
    ASSERT_TRUE(rejected.isError());
    EXPECT_TRUE(contains(rejected.error().message(), file.string()));
    EXPECT_EQ(session_.mediaLibrary().assetCount(), 0u);

    // A path that does not exist is likewise rejected, naming the path.
    const Result<Json> missing =
        registry().invoke("media.import", pathArgs(dir_ / "absent.mp4"));
    ASSERT_TRUE(missing.isError());
    EXPECT_EQ(session_.mediaLibrary().assetCount(), 0u);
}

TEST_F(SessionToolsTest, MediaImportIsAdvertisedButUnsupportedWithoutTheHook) {
    ProjectSession     session;
    const ToolRegistry registry = buildDefaultToolRegistry(session);

    ASSERT_TRUE(registry.has("media.import"));
    const Result<Json> unavailable =
        registry.invoke("media.import", pathArgs("/media/a.mp4"));
    ASSERT_TRUE(unavailable.isError());
    EXPECT_EQ(unavailable.error().code(), ErrorCode::Unsupported);
    EXPECT_TRUE(contains(unavailable.error().message(), "media import service"));
}

TEST_F(MediaToolsTest, MediaListReportsEveryRegisteredAssetWithItsDisplayName) {
    wire(mp4WithVideoAndAudio());

    const Json empty = invokeOk(registry(), "media.list", Json::object());
    EXPECT_EQ(empty.intOr("count"), 0);
    ASSERT_NE(empty.find("assets"), nullptr);
    EXPECT_TRUE(empty.find("assets")->asArray().empty());

    const Json a = invokeOk(registry(), "media.import", pathArgs(writeFile("first.mp4")));
    const Json b = invokeOk(registry(), "media.import", pathArgs(writeFile("second.mp4")));

    const Json listed = invokeOk(registry(), "media.list", Json::object());
    EXPECT_EQ(listed.intOr("count"), 2);
    const Json::Array& assets = listed.find("assets")->asArray();
    ASSERT_EQ(assets.size(), 2u);
    EXPECT_EQ(assets[0].stringOr("assetId"), a.stringOr("assetId"));
    EXPECT_EQ(assets[0].stringOr("displayName"), "first.mp4");
    EXPECT_EQ(assets[0].stringOr("sourcePath"), a.stringOr("sourcePath"));
    EXPECT_EQ(assets[1].stringOr("assetId"), b.stringOr("assetId"));
    EXPECT_EQ(assets[1].stringOr("displayName"), "second.mp4");

    // project.info agrees with media.list on the asset count.
    EXPECT_EQ(invokeOk(registry(), "project.info", Json::object()).intOr("assetCount"), 2);
}

// ===========================================================================
// Task 4.5 — timeline.add_track and timeline.remove_track
// (Requirements 3.3, 3.8, 3.10)
// ===========================================================================

TEST_F(SessionToolsTest, AddTrackAppendsAfterTheLastTrackOfItsKind) {
    ProjectSession     session;
    const ToolRegistry registry = buildDefaultToolRegistry(session);

    const Json video1 = invokeOk(registry, "timeline.add_track", kindArgs("video"));
    EXPECT_EQ(video1.stringOr("kind"), "video");
    EXPECT_EQ(video1.intOr("index"), 0);
    EXPECT_EQ(video1.intOr("trackCount"), 1);

    const Json audio1 = invokeOk(registry, "timeline.add_track", kindArgs("audio"));
    EXPECT_EQ(audio1.intOr("index"), 1);
    EXPECT_EQ(audio1.intOr("trackCount"), 2);

    // A second video track goes after the LAST VIDEO track, not at the end.
    const Json video2 = invokeOk(registry, "timeline.add_track", kindArgs("video"));
    EXPECT_EQ(video2.intOr("index"), 1);
    EXPECT_EQ(video2.intOr("trackCount"), 3);

    const Project project = session.engine().snapshot();
    ASSERT_EQ(project.tracks.size(), 3u);
    EXPECT_EQ(project.tracks[0].id.toString(), video1.stringOr("trackId"));
    EXPECT_EQ(project.tracks[1].id.toString(), video2.stringOr("trackId"));
    EXPECT_EQ(project.tracks[2].id.toString(), audio1.stringOr("trackId"));
    EXPECT_EQ(project.tracks[1].kind, TrackKind::Video);
    EXPECT_EQ(project.tracks[2].kind, TrackKind::Audio);

    // Every identifier is distinct within the project.
    EXPECT_NE(video1.stringOr("trackId"), video2.stringOr("trackId"));
    EXPECT_NE(video1.stringOr("trackId"), audio1.stringOr("trackId"));
}

TEST_F(SessionToolsTest, TrackToolsAreUndoableThroughTheOrdinaryEditPath) {
    ProjectSession     session;
    const ToolRegistry registry = buildDefaultToolRegistry(session);

    invokeOk(registry, "timeline.add_track", kindArgs("video"));
    EXPECT_EQ(session.engine().undoDepth(), 1u);
    ASSERT_TRUE(session.engine().undo().isOk());
    EXPECT_TRUE(session.engine().snapshot().tracks.empty());
    ASSERT_TRUE(session.engine().redo().isOk());
    ASSERT_EQ(session.engine().snapshot().tracks.size(), 1u);

    // A removal is undoable too, restoring the track AND its clips verbatim.
    Project withClip = session.engine().snapshot();
    withClip.assets.push_back(MediaAssetRef(Uuid::generateV4(), "/media/a.mp4"));
    withClip.tracks[0].clips.push_back(makeClip(withClip.assets[0].assetId, 0, 500'000'000));
    ASSERT_TRUE(session.engine().reset(withClip).isOk());
    const Uuid trackId = withClip.tracks[0].id;

    Json removeArgs = Json::object();
    removeArgs.set("trackId", trackId.toString());
    const Json removed = invokeOk(registry, "timeline.remove_track", removeArgs);
    EXPECT_EQ(removed.stringOr("trackId"), trackId.toString());
    EXPECT_EQ(removed.intOr("trackCount"), 0);
    EXPECT_EQ(removed.intOr("clipCount"), 0);
    EXPECT_EQ(session.engine().undoDepth(), 1u);

    ASSERT_TRUE(session.engine().undo().isOk());
    const Project restored = session.engine().snapshot();
    ASSERT_EQ(restored.tracks.size(), 1u);
    EXPECT_EQ(restored.tracks[0].id, trackId);
    ASSERT_EQ(restored.tracks[0].clips.size(), 1u);
    EXPECT_EQ(restored.tracks[0].clips[0].id, withClip.tracks[0].clips[0].id);
}

TEST_F(SessionToolsTest, RemoveTrackDropsItsClipsAndPreservesTheRemainingOrder) {
    ProjectSession     session;
    const ToolRegistry registry = buildDefaultToolRegistry(session);

    Project project;
    project.id = Uuid::generateV4();
    project.name = "Ordered";
    project.timelineFps = FrameRate::fps30();
    project.canvas = Resolution::hd1080();
    const Uuid assetId = Uuid::generateV4();
    project.assets.push_back(MediaAssetRef(assetId, "/media/a.mp4"));
    for (int i = 0; i < 4; ++i) {
        Track track;
        track.id = Uuid::generateV4();
        track.kind = TrackKind::Video;
        track.clips.push_back(makeClip(assetId, 0, 400'000'000));
        track.clips.push_back(makeClip(assetId, 500'000'000, 900'000'000));
        project.tracks.push_back(std::move(track));
    }
    ASSERT_TRUE(session.engine().reset(project).isOk());
    ASSERT_EQ(clipCount(session.engine().snapshot()), 8u);

    Json args = Json::object();
    args.set("trackId", project.tracks[1].id.toString());
    const Json removed = invokeOk(registry, "timeline.remove_track", args);
    EXPECT_EQ(removed.intOr("trackCount"), 3);
    EXPECT_EQ(removed.intOr("clipCount"), 6);

    const Project after = session.engine().snapshot();
    ASSERT_EQ(after.tracks.size(), 3u);
    EXPECT_EQ(after.tracks[0].id, project.tracks[0].id);
    EXPECT_EQ(after.tracks[1].id, project.tracks[2].id);
    EXPECT_EQ(after.tracks[2].id, project.tracks[3].id);
}

TEST_F(SessionToolsTest, TrackToolsRejectBadArgumentsAndChangeNothing) {
    ProjectSession     session;
    const ToolRegistry registry = buildDefaultToolRegistry(session);
    invokeOk(registry, "timeline.add_track", kindArgs("video"));
    const Project     before = session.engine().snapshot();
    const std::size_t historyBefore = session.engine().undoDepth();

    // An unknown kind: declared as a closed set, and refused by the handler.
    EXPECT_TRUE(registry.find("timeline.add_track")->schema.validate(kindArgs("subtitle")).isError());
    const Result<Json> badKind = registry.invoke("timeline.add_track", kindArgs("subtitle"));
    ASSERT_TRUE(badKind.isError());
    EXPECT_TRUE(contains(badKind.error().message(), "kind"));

    // A track identifier absent from the project.
    Json absent = Json::object();
    absent.set("trackId", Uuid::generateV4().toString());
    const Result<Json> unknownTrack = registry.invoke("timeline.remove_track", absent);
    ASSERT_TRUE(unknownTrack.isError());
    EXPECT_EQ(unknownTrack.error().code(), ErrorCode::NotFound);

    // A malformed identifier is rejected by the published schema and the handler.
    Json malformed = Json::object();
    malformed.set("trackId", std::string("not-a-uuid"));
    EXPECT_TRUE(registry.find("timeline.remove_track")->schema.validate(malformed).isError());
    EXPECT_TRUE(registry.invoke("timeline.remove_track", malformed).isError());

    const Project after = session.engine().snapshot();
    EXPECT_EQ(after.tracks.size(), before.tracks.size());
    EXPECT_EQ(after.tracks[0].id, before.tracks[0].id);
    EXPECT_EQ(session.engine().undoDepth(), historyBefore);
}

TEST_F(SessionToolsTest, AddTrackRefusesToExceedSixtyFourTracksOfOneKind) {
    ProjectSession     session;
    const ToolRegistry registry = buildDefaultToolRegistry(session);

    for (std::size_t i = 0; i < AddTrackCommand::kMaxTracksPerKind; ++i) {
        ASSERT_TRUE(registry.invoke("timeline.add_track", kindArgs("video")).isOk()) << i;
    }
    const Project before = session.engine().snapshot();
    ASSERT_EQ(before.tracks.size(), AddTrackCommand::kMaxTracksPerKind);

    const Result<Json> refused = registry.invoke("timeline.add_track", kindArgs("video"));
    ASSERT_TRUE(refused.isError());
    EXPECT_EQ(session.engine().snapshot().tracks.size(), before.tracks.size());

    // The cap is per kind, so an audio track is still accepted.
    EXPECT_TRUE(registry.invoke("timeline.add_track", kindArgs("audio")).isOk());
}

// ===========================================================================
// Requirement 3.1 — the surface itself
// ===========================================================================

TEST_F(SessionToolsTest, EveryToolRequirementThreeOneNamesIsRegisteredUnderThatExactName) {
    ProjectSession     session;
    const ToolRegistry registry = buildDefaultToolRegistry(session);

    for (const char* name : {"project.create", "project.open", "project.save", "project.info",
                             "media.import", "media.list", "timeline.add_track",
                             "timeline.remove_track", "timeline.export"}) {
        EXPECT_TRUE(registry.has(name)) << name;
        const Tool* tool = registry.find(name);
        ASSERT_NE(tool, nullptr) << name;
        EXPECT_FALSE(tool->description.empty()) << name;
        EXPECT_TRUE(static_cast<bool>(tool->handler)) << name;
    }
}

}  // namespace
}  // namespace palmier::services
