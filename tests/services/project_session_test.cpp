// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/services/project_session_test.cpp — example-based unit tests for
// services::ProjectSession (tasks 2.1 and 2.2).
//
// Coverage, by the behaviour each group pins down:
//
//   Default construction ......... an empty project at the documented default
//                                  frame rate, canvas and colour space, reported
//                                  unmodified with no on-disk location (1.10).
//   createProject ................ carries exactly the requested settings, is
//                                  current, unmodified, path-less (3.2), and
//                                  rejects every out-of-range argument by name
//                                  while changing nothing (3.8).
//   openProject .................. replaces the current project, records the
//                                  location, reports unmodified, and rebuilds the
//                                  media library (3.4); a missing / malformed /
//                                  unsupported / illegal document leaves the
//                                  previous project, its path, its modified flag
//                                  and its undo history untouched (3.9, 4.10).
//   Engine identity .............. the one TimelineEngine survives every project
//                                  switch, keeping observers and the reference
//                                  itself valid, and a switch clears BOTH history
//                                  stacks and emits ChangeOrigin::Reset — the
//                                  session-level view of TimelineEngine::reset.
//   Revision / modified .......... the revision increases monotonically and the
//                                  modified flag derives from it (4.6).
//   requestSave .................. returns immediately even while the write is
//                                  blocked (14.6); a successful save records the
//                                  path and clears the dirty flag; a save the user
//                                  edited through records the path but stays
//                                  modified (the revision guard, D6); a failed save
//                                  preserves the in-memory project, its modified
//                                  state and any previously saved file (4.4, 14.7).
//
// Save failures are injected deterministically through the services::RawFileWriter
// seam, so no test needs a full disk or elevated privileges, and no test sleeps to
// "wait for" the worker: a latch-backed writer makes the in-flight window explicit.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "core/ChangeSet.hpp"
#include "core/Clip.hpp"
#include "core/ColorSpace.hpp"
#include "core/Duration.hpp"
#include "core/EditCommands.hpp"
#include "core/Error.hpp"
#include "core/FrameRate.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/Project.hpp"
#include "core/Resolution.hpp"
#include "core/SchemaVersion.hpp"
#include "core/Track.hpp"
#include "core/Uuid.hpp"
#include "services/ProjectSession.hpp"
#include "services/ProjectStore.hpp"

namespace palmier::services {
namespace {

namespace fs = std::filesystem;

// --- Fixtures ---------------------------------------------------------------

// A unique scratch path under the OS temp directory, removed on construction and
// destruction so every test is hermetic.
class ScratchFile {
public:
    explicit ScratchFile(std::string name) : path_(fs::temp_directory_path() / std::move(name)) {
        std::error_code ec;
        fs::remove(path_, ec);
    }
    ~ScratchFile() {
        std::error_code ec;
        fs::remove(path_, ec);
    }
    ScratchFile(const ScratchFile&) = delete;
    ScratchFile& operator=(const ScratchFile&) = delete;

    [[nodiscard]] const fs::path& path() const noexcept { return path_; }

    void write(std::string_view bytes) const {
        std::ofstream out(path_, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    [[nodiscard]] std::string read() const {
        std::ifstream in(path_, std::ios::binary);
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    [[nodiscard]] bool exists() const {
        std::error_code ec;
        return fs::exists(path_, ec);
    }

private:
    fs::path path_;
};

// A document with one asset, one video track and one clip, so counts and the
// rebuilt media library are both observable.
Project makeDocument(std::string name) {
    Project p;
    p.id = Uuid::generateV4();
    p.name = std::move(name);
    p.timelineFps = FrameRate::fps25();
    p.canvas = Resolution::hd720();
    p.colorSpace = ColorSpace::Rec2020;
    p.version = SchemaVersion::current();

    const MediaAssetRef asset{Uuid::generateV4(), "/media/document.mp4"};
    p.assets = {asset};

    Track video;
    video.id = Uuid::generateV4();
    video.kind = TrackKind::Video;
    video.name = "V1";

    Clip clip;
    clip.id = Uuid::generateV4();
    clip.assetRef = asset;
    clip.timelineStart = Duration::zero();
    clip.sourceIn = Duration::zero();
    clip.sourceOut = Duration::fromMilliseconds(2000);
    video.clips.push_back(clip);

    p.tracks = {video};
    return p;
}

// Apply one real edit through the session's engine, so undo history and the
// engine-driven modified flag are both exercised.
void applyOneEdit(ProjectSession& session) {
    ASSERT_TRUE(session.engine()
                    .apply(std::make_unique<AddTrackCommand>(TrackKind::Video))
                    .changed());
}

// A RawFileWriter that blocks inside the write until it is released, so the window
// in which a save is genuinely in flight is explicit rather than timing-dependent.
class BlockingWriter {
public:
    RawFileWriter writer() {
        auto state = state_;
        return [state](const fs::path& path, std::string_view bytes) -> Result<void> {
            {
                std::unique_lock<std::mutex> lock(state->mutex);
                state->entered = true;
                state->enteredCv.notify_all();
                state->releaseCv.wait(lock, [&state] { return state->released; });
                if (state->fail) {
                    return makeError(ErrorCode::PermissionDenied,
                                     "injected permission failure for '" + path.string() + "'");
                }
            }
            return defaultRawFileWriter(path, bytes);
        };
    }

    void waitUntilWriting() const {
        std::unique_lock<std::mutex> lock(state_->mutex);
        state_->enteredCv.wait(lock, [this] { return state_->entered; });
    }

    void release(bool fail = false) {
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->fail = fail;
            state_->released = true;
        }
        state_->releaseCv.notify_all();
    }

private:
    struct State {
        std::mutex              mutex;
        std::condition_variable enteredCv;
        std::condition_variable releaseCv;
        bool                    entered = false;
        bool                    released = false;
        bool                    fail = false;
    };
    std::shared_ptr<State> state_ = std::make_shared<State>();
};

// A writer that always fails with the given code, without touching the filesystem.
RawFileWriter failingWriter(ErrorCode code, std::string message) {
    return [code, message](const fs::path& path, std::string_view) -> Result<void> {
        return makeError(code, message + " ('" + path.string() + "')");
    };
}

// --- Default construction (Requirement 1.10) --------------------------------

TEST(ProjectSessionDefaults, HoldsEmptyProjectAtTheDocumentedDefaults) {
    ProjectSession session;

    const Project project = session.engine().snapshot();
    EXPECT_EQ(project.timelineFps, kDefaultTimelineFps);
    EXPECT_EQ(project.timelineFps, FrameRate::fps30());
    EXPECT_EQ(project.canvas, kDefaultCanvas);
    EXPECT_EQ(project.canvas, Resolution::hd1080());
    EXPECT_EQ(project.colorSpace, kDefaultProjectColorSpace);
    EXPECT_TRUE(project.tracks.empty());
    EXPECT_TRUE(project.assets.empty());
    EXPECT_FALSE(project.id.isNil());

    const ProjectSession::Status status = session.status();
    EXPECT_EQ(status.projectId, project.id);
    EXPECT_EQ(status.trackCount, 0u);
    EXPECT_EQ(status.clipCount, 0u);
    EXPECT_FALSE(status.modified);
    EXPECT_FALSE(status.documentPath.has_value());

    EXPECT_FALSE(session.modified());
    EXPECT_FALSE(session.documentPath().has_value());
    EXPECT_EQ(session.mediaLibrary().assetCount(), 0u);
    EXPECT_FALSE(session.engine().canUndo());
    EXPECT_FALSE(session.engine().canRedo());
}

// --- createProject (Requirements 3.2, 3.8) ----------------------------------

TEST(ProjectSessionCreate, CarriesExactlyTheRequestedSettingsAndIsCurrent) {
    ProjectSession session;
    const Uuid     previousId = session.engine().snapshot().id;

    const Result<Uuid> created = session.createProject("Documentary", FrameRate::fps23_976(),
                                                       Resolution::uhd4k(), ColorSpace::Rec2100Pq);
    ASSERT_TRUE(created.isOk()) << created.error().toString();

    const Project project = session.engine().snapshot();
    EXPECT_EQ(project.id, created.value());
    EXPECT_NE(project.id, previousId);
    EXPECT_EQ(project.name, "Documentary");
    EXPECT_EQ(project.timelineFps, FrameRate::fps23_976());
    EXPECT_EQ(project.canvas, Resolution::uhd4k());
    EXPECT_EQ(project.colorSpace, ColorSpace::Rec2100Pq);

    EXPECT_FALSE(session.modified());
    EXPECT_FALSE(session.documentPath().has_value());
    EXPECT_EQ(session.mediaLibrary().assetCount(), 0u);
}

TEST(ProjectSessionCreate, RejectsEveryOutOfRangeArgumentByNameAndChangesNothing) {
    ProjectSession session;
    ASSERT_TRUE(session.createProject("Base", FrameRate::fps30(), Resolution::hd1080(),
                                      ColorSpace::Rec709)
                    .isOk());
    const Project       before = session.engine().snapshot();
    const std::uint64_t revisionBefore = session.revision();

    struct Case {
        const char* label;
        std::string name;
        FrameRate   fps;
        Resolution  canvas;
        ColorSpace  colorSpace;
        const char* expectedArgument;
    };
    const std::vector<Case> cases = {
        {"empty name", "", FrameRate::fps30(), Resolution::hd1080(), ColorSpace::Rec709, "name"},
        {"over-long name", std::string(129, 'x'), FrameRate::fps30(), Resolution::hd1080(),
         ColorSpace::Rec709, "name"},
        {"zero fps", "Ok", FrameRate{0, 1}, Resolution::hd1080(), ColorSpace::Rec709, "frameRate"},
        {"241 fps", "Ok", FrameRate{241, 1}, Resolution::hd1080(), ColorSpace::Rec709, "frameRate"},
        {"canvas too narrow", "Ok", FrameRate::fps30(), Resolution{15, 100}, ColorSpace::Rec709,
         "canvas"},
        {"canvas too wide", "Ok", FrameRate::fps30(), Resolution{7681, 100}, ColorSpace::Rec709,
         "canvas"},
        {"canvas too tall", "Ok", FrameRate::fps30(), Resolution{100, 4321}, ColorSpace::Rec709,
         "canvas"},
        {"unknown color space", "Ok", FrameRate::fps30(), Resolution::hd1080(),
         ColorSpace::Unknown, "colorSpace"},
    };

    for (const Case& c : cases) {
        SCOPED_TRACE(c.label);
        const Result<Uuid> created = session.createProject(c.name, c.fps, c.canvas, c.colorSpace);
        ASSERT_TRUE(created.isError());
        EXPECT_NE(created.error().message().find(c.expectedArgument), std::string::npos)
            << created.error().toString();

        // Nothing moved: same project, same revision, still unmodified.
        const Project after = session.engine().snapshot();
        EXPECT_EQ(after.id, before.id);
        EXPECT_EQ(after.name, before.name);
        EXPECT_EQ(after.timelineFps, before.timelineFps);
        EXPECT_EQ(after.canvas, before.canvas);
        EXPECT_EQ(after.colorSpace, before.colorSpace);
        EXPECT_EQ(session.revision(), revisionBefore);
        EXPECT_FALSE(session.modified());
    }
}

TEST(ProjectSessionCreate, AcceptsTheBoundaryValuesOfEveryRange) {
    ProjectSession session;

    EXPECT_TRUE(session.createProject("x", FrameRate{1, 1}, Resolution{16, 16}, ColorSpace::Srgb)
                    .isOk());
    EXPECT_TRUE(session
                    .createProject(std::string(128, 'y'), FrameRate{240, 1},
                                   Resolution{7680, 4320}, ColorSpace::LinearSrgb)
                    .isOk());
    // A rational rate inside the band is accepted even though it is not an integer.
    EXPECT_TRUE(session
                    .createProject("ntsc", FrameRate::fps29_97(), Resolution::hd1080(),
                                   ColorSpace::Rec709)
                    .isOk());
}

// --- openProject (Requirements 3.4, 3.9, 4.10) ------------------------------

TEST(ProjectSessionOpen, MakesTheLoadedDocumentCurrentAndRebuildsTheMediaLibrary) {
    const ScratchFile file("palmier_session_open.palmier");
    const Project     document = makeDocument("Loaded");
    ASSERT_TRUE(saveProjectToFile(document, file.path()).isOk());

    ProjectSession session;
    applyOneEdit(session);
    ASSERT_TRUE(session.modified());

    const Result<ProjectSession::Status> opened = session.openProject(file.path());
    ASSERT_TRUE(opened.isOk()) << opened.error().toString();

    EXPECT_EQ(opened.value().projectId, document.id);
    EXPECT_EQ(opened.value().name, "Loaded");
    EXPECT_EQ(opened.value().trackCount, 1u);
    EXPECT_EQ(opened.value().clipCount, 1u);
    EXPECT_FALSE(opened.value().modified);
    ASSERT_TRUE(opened.value().documentPath.has_value());
    EXPECT_EQ(*opened.value().documentPath, file.path());

    const Project current = session.engine().snapshot();
    EXPECT_EQ(current.id, document.id);
    EXPECT_EQ(current.timelineFps, document.timelineFps);
    EXPECT_EQ(current.canvas, document.canvas);
    EXPECT_EQ(current.colorSpace, document.colorSpace);

    // The library describes the project that is now current.
    EXPECT_EQ(session.mediaLibrary().assetCount(), 1u);
    EXPECT_TRUE(session.mediaLibrary().hasAsset(document.assets.front().assetId));

    // An opened project is unmodified and has no editing history.
    EXPECT_FALSE(session.modified());
    EXPECT_FALSE(session.engine().canUndo());
    EXPECT_FALSE(session.engine().canRedo());
}

TEST(ProjectSessionOpen, AFailedOpenPreservesThePreviousSessionExactly) {
    const ScratchFile good("palmier_session_open_good.palmier");
    ASSERT_TRUE(saveProjectToFile(makeDocument("Previous"), good.path()).isOk());

    const ScratchFile malformed("palmier_session_open_malformed.palmier");
    malformed.write("{ this is not a palmier document ");

    const ScratchFile futureVersion("palmier_session_open_future.palmier");
    futureVersion.write(R"({"format":"palmier-project","version":"99.0","project":{}})");

    const ScratchFile danglingAsset("palmier_session_open_dangling.palmier");
    {
        Project broken = makeDocument("Dangling");
        broken.assets.clear(); // the clip's assetRef no longer resolves
        danglingAsset.write(serializeProject(broken));
    }

    ProjectSession session;
    ASSERT_TRUE(session.openProject(good.path()).isOk());
    applyOneEdit(session);
    ASSERT_TRUE(session.modified());

    const Project       before = session.engine().snapshot();
    const std::uint64_t revisionBefore = session.revision();
    const std::size_t   assetsBefore = session.mediaLibrary().assetCount();

    const std::vector<fs::path> rejected = {
        fs::temp_directory_path() / "palmier_session_absent.palmier",
        malformed.path(),
        futureVersion.path(),
        danglingAsset.path(),
    };

    for (const fs::path& path : rejected) {
        SCOPED_TRACE(path.string());
        const Result<ProjectSession::Status> opened = session.openProject(path);
        ASSERT_TRUE(opened.isError());
        // The error names the file it could not open.
        EXPECT_NE(opened.error().message().find(path.string()), std::string::npos)
            << opened.error().toString();

        const Project after = session.engine().snapshot();
        EXPECT_EQ(after.id, before.id);
        EXPECT_EQ(after.name, before.name);
        EXPECT_EQ(after.tracks.size(), before.tracks.size());

        // Path, modified flag, revision, media library and undo history all intact.
        ASSERT_TRUE(session.documentPath().has_value());
        EXPECT_EQ(*session.documentPath(), good.path());
        EXPECT_TRUE(session.modified());
        EXPECT_EQ(session.revision(), revisionBefore);
        EXPECT_EQ(session.mediaLibrary().assetCount(), assetsBefore);
        EXPECT_TRUE(session.engine().canUndo());
    }
}

// --- Engine identity and the session-level view of TimelineEngine::reset -----

TEST(ProjectSessionEngineReset, TheOneEngineSurvivesEveryProjectSwitch) {
    const ScratchFile file("palmier_session_identity.palmier");
    ASSERT_TRUE(saveProjectToFile(makeDocument("Switched"), file.path()).isOk());

    ProjectSession        session;
    const TimelineEngine* engineAddress = &session.engine();

    std::vector<ChangeOrigin> origins;
    Subscription              sub = session.engine().observe(
        [&origins](const ChangeSet& change) { origins.push_back(change.origin); });

    applyOneEdit(session);
    ASSERT_TRUE(session.createProject("Fresh", FrameRate::fps50(), Resolution::hd720(),
                                      ColorSpace::Rec709)
                    .isOk());
    ASSERT_TRUE(session.openProject(file.path()).isOk());

    // Same engine object throughout, and the pre-existing subscription kept
    // receiving events across both project switches.
    EXPECT_EQ(&session.engine(), engineAddress);
    ASSERT_EQ(origins.size(), 3u);
    EXPECT_EQ(origins[0], ChangeOrigin::Apply);
    EXPECT_EQ(origins[1], ChangeOrigin::Reset);
    EXPECT_EQ(origins[2], ChangeOrigin::Reset);
    EXPECT_TRUE(sub.active());
}

TEST(ProjectSessionEngineReset, AProjectSwitchClearsBothHistoryStacks) {
    ProjectSession session;
    applyOneEdit(session);
    ASSERT_TRUE(session.engine().undo().changed());
    ASSERT_TRUE(session.engine().canRedo());
    applyOneEdit(session);
    ASSERT_TRUE(session.engine().canUndo());

    ASSERT_TRUE(session.createProject("Fresh", FrameRate::fps30(), Resolution::hd1080(),
                                      ColorSpace::Rec709)
                    .isOk());

    EXPECT_FALSE(session.engine().canUndo());
    EXPECT_FALSE(session.engine().canRedo());
    EXPECT_TRUE(session.engine().undo().isNoOp());
    EXPECT_TRUE(session.engine().redo().isNoOp());
}

// --- Revision counter and the derived modified flag (Requirement 4.6) -------

TEST(ProjectSessionRevision, IncreasesMonotonicallyAndDrivesTheModifiedFlag) {
    ProjectSession session;
    const std::uint64_t start = session.revision();
    EXPECT_FALSE(session.modified());

    // An engine-originated edit marks the session modified on its own.
    applyOneEdit(session);
    const std::uint64_t afterEdit = session.revision();
    EXPECT_GT(afterEdit, start);
    EXPECT_TRUE(session.modified());

    // Undo is a change too: still modified, revision still climbing.
    ASSERT_TRUE(session.engine().undo().changed());
    EXPECT_GT(session.revision(), afterEdit);
    EXPECT_TRUE(session.modified());

    // An out-of-engine change is reported explicitly.
    const std::uint64_t beforeMark = session.revision();
    session.markModified();
    EXPECT_EQ(session.revision(), beforeMark + 1);
    EXPECT_TRUE(session.modified());

    // A project switch resets the session to "in step with disk" without ever
    // decreasing the revision.
    const std::uint64_t beforeCreate = session.revision();
    ASSERT_TRUE(session.createProject("Fresh", FrameRate::fps30(), Resolution::hd1080(),
                                      ColorSpace::Rec709)
                    .isOk());
    EXPECT_GT(session.revision(), beforeCreate);
    EXPECT_FALSE(session.modified());
}

TEST(ProjectSessionStatusObservers, ReceiveEveryChangeUntilUnsubscribed) {
    ProjectSession                          session;
    std::vector<ProjectSession::Status>      seen;
    Subscription                            sub =
        session.observeStatus([&seen](const ProjectSession::Status& s) { seen.push_back(s); });

    applyOneEdit(session);
    ASSERT_FALSE(seen.empty());
    EXPECT_TRUE(seen.back().modified);
    EXPECT_EQ(seen.back().trackCount, 1u);

    session.markModified();
    const std::size_t afterMark = seen.size();
    EXPECT_GT(afterMark, 1u);

    sub.reset();
    session.markModified();
    EXPECT_EQ(seen.size(), afterMark);
}

// --- requestSave: off the calling thread, with the revision guard -----------

TEST(ProjectSessionSave, WritesTheProjectAndClearsTheDirtyFlag) {
    const ScratchFile file("palmier_session_save.palmier");

    ProjectSession session;
    ASSERT_TRUE(session.createProject("Saved", FrameRate::fps30(), Resolution::hd1080(),
                                      ColorSpace::Rec709)
                    .isOk());
    applyOneEdit(session);
    ASSERT_TRUE(session.modified());
    const Project expected = session.engine().snapshot();

    std::optional<ProjectSession::SaveCompletionInfo> info;
    ASSERT_TRUE(session
                    .requestSave(file.path(),
                                 [&info](const ProjectSession::SaveCompletionInfo& i) { info = i; })
                    .isOk());
    EXPECT_EQ(session.awaitSaveCompletions(), 1u);

    ASSERT_TRUE(info.has_value());
    EXPECT_TRUE(info->succeeded);
    EXPECT_GT(info->bytesWritten, 0u);
    EXPECT_EQ(info->path, file.path());
    EXPECT_FALSE(info->stillModified);

    // Reported unmodified, with the written location recorded (Requirements 4.1, 4.6).
    EXPECT_FALSE(session.modified());
    ASSERT_TRUE(session.documentPath().has_value());
    EXPECT_EQ(*session.documentPath(), file.path());

    // The file holds the project that was current when the save was requested.
    const Result<Project> reloaded = loadProjectFromFile(file.path());
    ASSERT_TRUE(reloaded.isOk()) << reloaded.error().toString();
    EXPECT_EQ(reloaded.value().id, expected.id);
    EXPECT_EQ(reloaded.value().name, expected.name);
    EXPECT_EQ(reloaded.value().tracks.size(), expected.tracks.size());

    // The next edit makes it modified again (Requirement 4.6).
    applyOneEdit(session);
    EXPECT_TRUE(session.modified());
}

TEST(ProjectSessionSave, ReturnsImmediatelyWhileTheWriteIsStillRunning) {
    const ScratchFile file("palmier_session_save_async.palmier");

    BlockingWriter blocking;
    ProjectSession session(blocking.writer());
    session.markModified();

    const auto submittedAt = std::chrono::steady_clock::now();
    ASSERT_TRUE(session.requestSave(file.path()).isOk());
    const auto elapsed = std::chrono::steady_clock::now() - submittedAt;

    // The call did not wait for the write (Requirement 14.6): the writer is still
    // parked inside its blocking section right now.
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 250);
    blocking.waitUntilWriting();
    EXPECT_EQ(session.pendingSaveCount(), 1u);

    // The session remains fully usable while the write is in flight.
    EXPECT_NO_THROW((void)session.status());
    applyOneEdit(session);
    EXPECT_TRUE(session.modified());

    blocking.release();
    EXPECT_EQ(session.awaitSaveCompletions(), 1u);
    EXPECT_EQ(session.pendingSaveCount(), 0u);
}

TEST(ProjectSessionSave, AnEditDuringTheWriteRecordsThePathButStaysModified) {
    const ScratchFile file("palmier_session_save_raced.palmier");

    BlockingWriter blocking;
    ProjectSession session(blocking.writer());
    applyOneEdit(session);
    const std::uint64_t requested = session.revision();

    std::optional<ProjectSession::SaveCompletionInfo> info;
    ASSERT_TRUE(session
                    .requestSave(file.path(),
                                 [&info](const ProjectSession::SaveCompletionInfo& i) { info = i; })
                    .isOk());
    blocking.waitUntilWriting();

    // The user edits while the bytes are being written.
    applyOneEdit(session);
    ASSERT_GT(session.revision(), requested);

    blocking.release();
    EXPECT_EQ(session.awaitSaveCompletions(), 1u);

    ASSERT_TRUE(info.has_value());
    EXPECT_TRUE(info->succeeded);
    EXPECT_EQ(info->requestedRevision, requested);
    EXPECT_TRUE(info->stillModified);

    // The written file is valid, so the location is recorded — but the session
    // has moved on, so it stays modified (design.md D6).
    ASSERT_TRUE(session.documentPath().has_value());
    EXPECT_EQ(*session.documentPath(), file.path());
    EXPECT_TRUE(session.modified());
    EXPECT_TRUE(loadProjectFromFile(file.path()).isOk());
}

TEST(ProjectSessionSave, AFailedSavePreservesTheProjectTheFlagAndAnyPreviousFile) {
    const ScratchFile file("palmier_session_save_failure.palmier");
    const std::string previousBytes = "PREVIOUSLY SAVED DOCUMENT BYTES";
    file.write(previousBytes);

    ProjectSession session(failingWriter(ErrorCode::Io, "no space left on device"));
    applyOneEdit(session);
    const Project       before = session.engine().snapshot();
    const std::uint64_t revisionBefore = session.revision();
    ASSERT_TRUE(session.modified());

    std::optional<ProjectSession::SaveCompletionInfo> info;
    ASSERT_TRUE(session
                    .requestSave(file.path(),
                                 [&info](const ProjectSession::SaveCompletionInfo& i) { info = i; })
                    .isOk());
    EXPECT_EQ(session.awaitSaveCompletions(), 1u);

    ASSERT_TRUE(info.has_value());
    EXPECT_FALSE(info->succeeded);
    EXPECT_TRUE(info->stillModified);
    // The error names the destination that did not complete (Requirement 4.4).
    EXPECT_NE(info->error.message().find(file.path().string()), std::string::npos)
        << info->error.toString();

    // Previously saved file byte-for-byte intact; in-memory project untouched;
    // still modified; no document path invented (Requirements 4.4, 14.7).
    ASSERT_TRUE(file.exists());
    EXPECT_EQ(file.read(), previousBytes);
    const Project after = session.engine().snapshot();
    EXPECT_EQ(after.id, before.id);
    EXPECT_EQ(after.tracks.size(), before.tracks.size());
    EXPECT_EQ(session.revision(), revisionBefore);
    EXPECT_TRUE(session.modified());
    EXPECT_FALSE(session.documentPath().has_value());
}

TEST(ProjectSessionSave, RejectsAnEmptyDestinationWithoutStartingAWorker) {
    ProjectSession session;
    session.markModified();

    const Result<void> requested = session.requestSave(fs::path{});
    ASSERT_TRUE(requested.isError());
    EXPECT_EQ(requested.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(session.pendingSaveCount(), 0u);
    EXPECT_EQ(session.pumpSaveCompletions(), 0u);
    EXPECT_TRUE(session.modified());
}

TEST(ProjectSessionSave, DeliversCompletionsOnlyWhenPumped) {
    const ScratchFile file("palmier_session_save_pump.palmier");

    ProjectSession    session;
    std::atomic<int>  notifications{0};
    session.setSaveCompletionNotifier([&notifications] { ++notifications; });
    session.markModified();

    ASSERT_TRUE(session.requestSave(file.path()).isOk());
    session.waitForPendingSaves();

    // The worker has finished but has NOT applied anything: the dirty flag is only
    // cleared on the owning thread, inside the pump.
    EXPECT_TRUE(session.modified());
    EXPECT_GE(notifications.load(), 1);

    EXPECT_EQ(session.pumpSaveCompletions(), 1u);
    EXPECT_FALSE(session.modified());
    EXPECT_EQ(session.pumpSaveCompletions(), 0u);
}

} // namespace
} // namespace palmier::services
