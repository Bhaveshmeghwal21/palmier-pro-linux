// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/services/project_save_service_test.cpp — example-based unit tests for the
// save success/failure handling (task 5.3; Requirements 3.6, 3.7).
//
// These verify the user-facing save policy layered on top of the `.palmier`
// serializer:
//   * a completed save writes the complete project state to the single target
//     location, reports success (SaveOutcome), and records the last-saved status
//     (Requirement 3.6);
//   * a save that fails due to an inaccessible location, insufficient permissions,
//     or insufficient disk space preserves the last successfully saved file
//     byte-for-byte and leaves the recorded last-saved status unchanged, while
//     reporting a descriptive error (Requirement 3.7).
//
// Disk-space / permission failures are simulated deterministically through the
// injectable RawFileWriter seam, so the tests need neither a real full disk nor
// root privileges. The atomic-write guarantee (a failed write never truncates the
// previous file) is exercised end to end with the default writer.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "core/Clip.hpp"
#include "core/ColorSpace.hpp"
#include "core/Duration.hpp"
#include "core/FrameRate.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/Project.hpp"
#include "core/Resolution.hpp"
#include "core/Track.hpp"
#include "core/Uuid.hpp"
#include "services/ProjectSaveService.hpp"
#include "services/ProjectStore.hpp"

namespace palmier::services {
namespace {

namespace fs = std::filesystem;

// A small but non-trivial project so the round-trip after save is meaningful.
Project makeProject(std::string name) {
    Project p;
    p.id = Uuid::generateV4();
    p.name = std::move(name);
    p.timelineFps = FrameRate::fps30();
    p.canvas = Resolution::hd1080();
    p.colorSpace = ColorSpace::Rec709;

    MediaAssetRef asset{Uuid::generateV4(), "/media/clip.mp4"};
    p.assets = {asset};

    Track video;
    video.id = Uuid::generateV4();
    video.kind = TrackKind::Video;

    Clip clip;
    clip.id = Uuid::generateV4();
    clip.assetRef = asset;
    clip.timelineStart = Duration::zero();
    clip.sourceIn = Duration::zero();
    clip.sourceOut = Duration::fromMilliseconds(4000);
    video.clips.push_back(clip);

    p.tracks = {video};
    return p;
}

// A unique scratch path under the OS temp directory; removed on construction and
// destruction so tests are hermetic.
class ScratchFile {
public:
    explicit ScratchFile(std::string name)
        : path_(fs::temp_directory_path() / std::move(name)) {
        std::error_code ec;
        fs::remove(path_, ec);
    }
    ~ScratchFile() {
        std::error_code ec;
        fs::remove(path_, ec);
    }
    [[nodiscard]] const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

std::string readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

// Requirement 3.6: a completed save writes the complete state to the single
// location, reports success, and records the last-saved status.
TEST(ProjectSaveService, SuccessfulSaveWritesStateAndReportsSuccess) {
    ScratchFile scratch("palmier_save_success.palmier");
    ProjectSaveService service;

    EXPECT_FALSE(service.hasSavedState());

    const Project project = makeProject("my project");
    Result<SaveOutcome> outcome = service.save(project, scratch.path());

    ASSERT_TRUE(outcome.isOk()) << outcome.error().toString();
    EXPECT_EQ(outcome.value().path, scratch.path());
    EXPECT_GT(outcome.value().bytesWritten, 0u);

    // Last-saved status is recorded.
    EXPECT_TRUE(service.hasSavedState());
    EXPECT_EQ(service.lastSavedPath(), scratch.path());

    // The single target location holds the complete, reloadable project state.
    ASSERT_TRUE(fs::exists(scratch.path()));
    Result<Project> reloaded = loadProjectFromFile(scratch.path());
    ASSERT_TRUE(reloaded.isOk()) << reloaded.error().toString();
    EXPECT_EQ(reloaded.value().id, project.id);
    EXPECT_EQ(reloaded.value().name, project.name);
    ASSERT_EQ(reloaded.value().tracks.size(), 1u);
    ASSERT_EQ(reloaded.value().tracks[0].clips.size(), 1u);
    EXPECT_EQ(reloaded.value().tracks[0].clips[0].id, project.tracks[0].clips[0].id);
}

// A second successful save to the same location atomically replaces the first.
TEST(ProjectSaveService, SecondSaveReplacesPreviousContent) {
    ScratchFile scratch("palmier_save_replace.palmier");
    ProjectSaveService service;

    ASSERT_TRUE(service.save(makeProject("first"), scratch.path()).isOk());
    const Project second = makeProject("second");
    ASSERT_TRUE(service.save(second, scratch.path()).isOk());

    Result<Project> reloaded = loadProjectFromFile(scratch.path());
    ASSERT_TRUE(reloaded.isOk()) << reloaded.error().toString();
    EXPECT_EQ(reloaded.value().name, "second");
    EXPECT_EQ(reloaded.value().id, second.id);
}

// Requirement 3.7: an inaccessible / nonexistent save location fails without
// creating any file and without changing the last-saved status.
TEST(ProjectSaveService, InaccessibleLocationFailsAndCreatesNothing) {
    ProjectSaveService service;
    const fs::path badPath =
        fs::temp_directory_path() / "palmier_no_such_dir_98765" / "project.palmier";
    std::error_code ec;
    fs::remove_all(fs::temp_directory_path() / "palmier_no_such_dir_98765", ec);

    Result<SaveOutcome> outcome = service.save(makeProject("x"), badPath);

    ASSERT_TRUE(outcome.isError());
    EXPECT_EQ(outcome.error().code(), ErrorCode::Io);
    EXPECT_FALSE(fs::exists(badPath));
    // No successful save ever occurred.
    EXPECT_FALSE(service.hasSavedState());
    EXPECT_TRUE(service.lastSavedPath().empty());
}

// Requirement 3.7 (core guarantee): a save that fails due to insufficient disk
// space or permissions preserves the LAST successfully saved file byte-for-byte
// and leaves the recorded last-saved status unchanged.
TEST(ProjectSaveService, FailedSavePreservesLastGoodState) {
    ScratchFile scratch("palmier_save_preserve.palmier");

    // A writer that succeeds until armed to fail, then reports a disk-full error.
    bool failWrites = false;
    RawFileWriter writer = [&failWrites](const fs::path& path,
                                         std::string_view bytes) -> Result<void> {
        if (failWrites) {
            return makeError(ErrorCode::Io, "simulated: no space left on device");
        }
        return defaultRawFileWriter(path, bytes);
    };
    ProjectSaveService service(writer);

    // First save succeeds and establishes the "last good" on-disk state.
    const Project good = makeProject("last good");
    ASSERT_TRUE(service.save(good, scratch.path()).isOk());
    ASSERT_TRUE(fs::exists(scratch.path()));
    const std::string goodBytes = readFile(scratch.path());
    ASSERT_FALSE(goodBytes.empty());
    const fs::path recordedAfterGood = service.lastSavedPath();

    // Now writes fail: attempt to save a different project to the same location.
    failWrites = true;
    Result<SaveOutcome> outcome = service.save(makeProject("should not persist"),
                                               scratch.path());

    // The failure is reported...
    ASSERT_TRUE(outcome.isError());
    EXPECT_EQ(outcome.error().code(), ErrorCode::Io);

    // ...the previously saved file is preserved byte-for-byte...
    ASSERT_TRUE(fs::exists(scratch.path()));
    EXPECT_EQ(readFile(scratch.path()), goodBytes);
    Result<Project> reloaded = loadProjectFromFile(scratch.path());
    ASSERT_TRUE(reloaded.isOk()) << reloaded.error().toString();
    EXPECT_EQ(reloaded.value().name, "last good");

    // ...and the recorded last-saved status is unchanged.
    EXPECT_TRUE(service.hasSavedState());
    EXPECT_EQ(service.lastSavedPath(), recordedAfterGood);

    // No stray temporary files were left behind in the directory.
    for (const auto& entry : fs::directory_iterator(scratch.path().parent_path())) {
        EXPECT_EQ(entry.path().string().find(".palmier-save-"), std::string::npos)
            << "leftover temp file: " << entry.path();
    }
}

// A permission failure surfaced by the writer is reported as PermissionDenied and
// likewise leaves any prior state untouched.
TEST(ProjectSaveService, PermissionDeniedIsReported) {
    ScratchFile scratch("palmier_save_perm.palmier");
    RawFileWriter denied = [](const fs::path&, std::string_view) -> Result<void> {
        return makeError(ErrorCode::PermissionDenied, "simulated: permission denied");
    };
    ProjectSaveService service(denied);

    Result<SaveOutcome> outcome = service.save(makeProject("x"), scratch.path());

    ASSERT_TRUE(outcome.isError());
    EXPECT_EQ(outcome.error().code(), ErrorCode::PermissionDenied);
    EXPECT_FALSE(fs::exists(scratch.path()));
    EXPECT_FALSE(service.hasSavedState());
}

} // namespace
} // namespace palmier::services
