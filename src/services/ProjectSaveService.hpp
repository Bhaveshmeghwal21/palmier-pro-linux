// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/ProjectSaveService.hpp — user-facing save success/failure handling for
// the `.palmier` project store (design.md Architecture "Project I/O .palmier";
// Requirements 3.6, 3.7). Task 5.3.
//
// Task 5.1 (ProjectStore) provides the pure (de)serialization mechanism plus the
// thin saveProjectToFile/loadProjectFromFile helpers. This component layers the
// POLICY on top of that mechanism:
//
//   * Requirement 3.6 — WHEN a user saves a project, write the COMPLETE project
//     state to a single project location on disk and indicate to the user that
//     the save completed successfully. save() returns a SaveOutcome describing
//     the location written and the byte count, which the UI surfaces as the
//     "saved successfully" confirmation.
//
//   * Requirement 3.7 — IF a save fails due to insufficient disk space,
//     insufficient permissions, or an inaccessible save location, THEN preserve
//     the last successfully saved project state and report that the save did not
//     complete. The service guarantees this by never writing the target file in
//     place: it writes the serialized bytes to a sibling temporary file first and
//     only then atomically renames that temporary over the target. A failure
//     while writing the temporary (out of space / permission denied) therefore
//     leaves the previously saved target file byte-for-byte intact — it is never
//     truncated or partially overwritten — and leaves the service's recorded
//     "last successfully saved" status unchanged.
//
// Atomic-write guarantee
// ----------------------
// save() performs a write-to-temp-then-rename sequence:
//   1. Serialize the project (serializeProject; never fails).
//   2. Verify the destination directory exists and is a directory; if not, the
//      save location is inaccessible and the call fails WITHOUT touching any file.
//   3. Write the bytes to a temporary file in the SAME directory as the target
//      (so the subsequent rename stays on one filesystem and is atomic). A write
//      failure here (disk full, permission denied) aborts the save; the temp file
//      is cleaned up and the target is left untouched.
//   4. Atomically rename the temporary over the target (std::filesystem::rename).
//      On POSIX this is an atomic replace: a reader either sees the old file or
//      the new file, never a partial one. A rename failure aborts the save with
//      the target preserved.
// Only after a successful rename does the service update its last-saved status.
//
// Testability
// -----------
// The low-level "write these bytes to this path" step is injected through the
// RawFileWriter seam. The default seam writes via std::ofstream. Unit tests
// substitute a seam that returns a PermissionDenied / Io error to simulate a full
// disk or a read-only location deterministically, without needing an actual full
// filesystem, and assert that a previously saved file survives the failed save.

#ifndef PALMIER_SERVICES_PROJECTSAVESERVICE_HPP
#define PALMIER_SERVICES_PROJECTSAVESERVICE_HPP

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string_view>

#include "core/Project.hpp"
#include "core/Result.hpp"

namespace palmier::services {

/// Describes a save that completed successfully. Returned by
/// ProjectSaveService::save on success so the UI can confirm to the user that the
/// project was written and where (Requirement 3.6).
struct SaveOutcome {
    std::filesystem::path path;         ///< The single location the project was written to.
    std::size_t           bytesWritten = 0; ///< Size of the serialized `.palmier` document.
};

/// Low-level seam: write exactly `bytes` to `path`, creating or truncating it.
///
/// Returns an ok Result on a fully successful write, or an error Result on
/// failure (ErrorCode::Io for a general write/space failure,
/// ErrorCode::PermissionDenied for an access failure). The ProjectSaveService
/// only ever asks the writer to populate a TEMPORARY file, so a failure reported
/// here never affects the previously saved target file.
using RawFileWriter =
    std::function<Result<void>(const std::filesystem::path& path, std::string_view bytes)>;

/// The default RawFileWriter: write the bytes to `path` with a truncating binary
/// std::ofstream, flushing and checking the stream state. Returns ErrorCode::Io if
/// the file cannot be opened or a write/flush fails.
[[nodiscard]] Result<void> defaultRawFileWriter(const std::filesystem::path& path,
                                                std::string_view bytes);

/// Saves projects to `.palmier` stores with success confirmation and a
/// crash/failure-safe atomic write that preserves the last successfully saved
/// state (Requirements 3.6, 3.7).
class ProjectSaveService {
public:
    /// Construct with the default (std::ofstream-backed) file writer.
    ProjectSaveService();

    /// Construct with a custom low-level file writer. Used by tests to inject
    /// deterministic disk-space / permission failures; production code uses the
    /// default constructor.
    explicit ProjectSaveService(RawFileWriter writer);

    /// Serialize and save `project` to `path` as a single `.palmier` store.
    ///
    /// On success: returns a SaveOutcome (the written location and byte count) and
    /// records `path` as the last successfully saved location. The complete
    /// project state is written to the single `path` location (Requirement 3.6).
    ///
    /// On failure (inaccessible/nonexistent destination directory, permission
    /// denied, insufficient disk space, or a rename failure): returns a
    /// descriptive error (ErrorCode::Io or ErrorCode::PermissionDenied). The
    /// previously saved file at `path`, if any, is left byte-for-byte intact and
    /// the recorded last-saved status is unchanged (Requirement 3.7).
    [[nodiscard]] Result<SaveOutcome> save(const Project& project,
                                           const std::filesystem::path& path);

    /// True once at least one save has completed successfully via this service.
    [[nodiscard]] bool hasSavedState() const noexcept { return hasSaved_; }

    /// The location of the most recent successful save (empty if none yet).
    [[nodiscard]] const std::filesystem::path& lastSavedPath() const noexcept {
        return lastSavedPath_;
    }

private:
    RawFileWriter         writer_;
    bool                  hasSaved_ = false;
    std::filesystem::path lastSavedPath_;
};

} // namespace palmier::services

#endif // PALMIER_SERVICES_PROJECTSAVESERVICE_HPP
