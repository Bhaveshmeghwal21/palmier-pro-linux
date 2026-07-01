// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/ProjectSaveService.cpp — implementation of the save success/failure
// handling declared in ProjectSaveService.hpp (Requirements 3.6, 3.7). Task 5.3.
//
// The core of this file is the atomic write-to-temp-then-rename sequence that
// guarantees a failed save can never truncate or corrupt the last successfully
// saved project file. See the header for the full contract.

#include "services/ProjectSaveService.hpp"

#include <atomic>
#include <fstream>
#include <ios>
#include <string>
#include <system_error>
#include <utility>

#include "core/Error.hpp"
#include "services/ProjectStore.hpp"

namespace palmier::services {

namespace {

// Monotonic counter making each temporary file name unique within a process, so
// concurrent saves (e.g. distinct services or paths) never collide on a temp name.
std::atomic<std::uint64_t> gTempCounter{0};

// Build a temporary path SIBLING to `target` (same parent directory) so the
// subsequent std::filesystem::rename stays on one filesystem and is atomic. The
// name is derived from the target plus a per-process-unique suffix.
std::filesystem::path temporaryPathFor(const std::filesystem::path& target) {
    const std::uint64_t n = gTempCounter.fetch_add(1, std::memory_order_relaxed);
    std::filesystem::path tmp = target;
    tmp += ".palmier-save-" + std::to_string(n) + ".tmp";
    return tmp;
}

} // namespace

Result<void> defaultRawFileWriter(const std::filesystem::path& path,
                                  std::string_view bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return makeError(ErrorCode::Io,
                         "could not open '" + path.string() + "' for writing");
    }
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.flush();
    if (!out) {
        return makeError(ErrorCode::Io,
                         "failed while writing '" + path.string() + "'");
    }
    return ok();
}

ProjectSaveService::ProjectSaveService() : writer_(&defaultRawFileWriter) {}

ProjectSaveService::ProjectSaveService(RawFileWriter writer)
    : writer_(std::move(writer)) {}

Result<SaveOutcome> ProjectSaveService::save(const Project& project,
                                             const std::filesystem::path& path) {
    // 1. Serialize the COMPLETE project state (Requirement 3.6). serializeProject
    //    is total: any project value renders to a `.palmier` document.
    const std::string text = serializeProject(project);

    // 2. The destination directory must exist and be a directory. Rejecting an
    //    inaccessible/nonexistent location here means we never create a temp file
    //    in a bad place and never disturb any existing target (Requirement 3.7).
    const std::filesystem::path directory =
        path.has_parent_path() ? path.parent_path() : std::filesystem::path{"."};
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) {
        return makeError(ErrorCode::Io,
                         "save location is inaccessible: '" + directory.string() +
                             "' is not an existing directory");
    }

    // 3. Write the bytes to a sibling temporary file. A failure here (disk full,
    //    permission denied) aborts the save. The target file — the last
    //    successfully saved state — is never opened for writing, so it is left
    //    byte-for-byte intact.
    const std::filesystem::path tempPath = temporaryPathFor(path);
    Result<void> written = writer_(tempPath, text);
    if (written.isError()) {
        // Best-effort cleanup of a possibly partial temp file; ignore errors.
        std::error_code cleanupEc;
        std::filesystem::remove(tempPath, cleanupEc);
        return std::move(written).error();
    }

    // 4. Atomically replace the target with the temp file. On POSIX rename() is an
    //    atomic swap: a concurrent reader sees either the old or the new file,
    //    never a partial one. If the rename fails, remove the temp and abort with
    //    the previously saved target preserved.
    std::filesystem::rename(tempPath, path, ec);
    if (ec) {
        std::error_code cleanupEc;
        std::filesystem::remove(tempPath, cleanupEc);
        return makeError(ErrorCode::Io,
                         "could not finalize save to '" + path.string() +
                             "': " + ec.message());
    }

    // 5. Success — record the last successfully saved location and report it back
    //    so the UI can confirm the save completed (Requirement 3.6).
    hasSaved_ = true;
    lastSavedPath_ = path;
    return SaveOutcome{path, text.size()};
}

} // namespace palmier::services
