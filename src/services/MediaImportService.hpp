// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/MediaImportService.hpp — the Media_Import_Service: the one path by
// which a file on disk becomes an asset in the current project's media library
// (design.md "services::MediaImportService"; task 4.1).
//
// Every importer in the product funnels through this class: the media-browser
// import action, the `media.import` tool (task 4.4) and the agent. That is what
// makes "one asset per distinct filesystem location" true no matter who asked
// (Requirements 2.5, 2.6).
//
// The pipeline is strictly probe -> validate -> register, and it commits nothing
// until all three have succeeded:
//
//   1. classify the request      — an empty path, a path that does not exist, a
//                                  path that cannot be opened or read, and a file
//                                  whose decodable stream fails to decode are the
//                                  four conditions Requirement 2.4 requires the
//                                  error to name alongside the path;
//   2. duplicate check           — the request is canonicalised with
//                                  std::filesystem::weakly_canonical and compared
//                                  against the library, so a second import of the
//                                  same location returns the existing asset id and
//                                  adds no entry (Requirement 2.5);
//   3. probe + validate          — media::validateMediaImport over the injected
//                                  media::MediaProbeBackend, so an unrecognised
//                                  container or a container in which every stream
//                                  uses an undecodable codec is rejected with the
//                                  container format and the rejected codecs named
//                                  (Requirement 2.3);
//   4. register exactly one asset in ProjectSession::mediaLibrary() and mark the
//      session modified (Requirement 2.1).
//
// Nothing before step 4 touches the library, so every rejection leaves the entry
// count and contents exactly as they were (Requirements 2.3, 2.4, 2.6, 2.8).
//
// Injection seams (why this is testable without real media)
// ---------------------------------------------------------
// The probe backend is the media layer's existing media::MediaProbeBackend seam,
// so any container/codec/stream layout can be presented to the service
// synthetically. Two smaller seams complete the picture:
//
//   * Options::accessCheck  — the "does this path exist and can it be opened?"
//                             step. The default consults the real filesystem;
//                             a test may replace it to drive the missing /
//                             unreadable classifications without creating files.
//   * Options::decodeCheck  — the "does the decodable stream actually decode?"
//                             step of Requirement 2.4. The default accepts (the
//                             decoder is wired up in a later stage); a test may
//                             replace it to drive the undecodable classification.
//
// Time limit (Requirement 2.8)
// ----------------------------
// Steps 1 and 3 — access check, probe, validation, decode check — run on a worker
// thread awaited with Options::timeout (30 s by default). A backend that hangs
// therefore cannot hang the caller: the import is abandoned, nothing is
// registered, and the error names the path and the limit that was exceeded. The
// worker is detached and owns copies of everything it touches, so it can outlive
// the abandoned import without dangling.
//
// Pending imports (Requirement 2.9)
// ---------------------------------
// The canonical path is recorded as pending for exactly the window in which the
// probe, the validation and the registration are running, and is removed on every
// exit path. While it is pending the library carries no entry for it — the entry
// is added as the last action of a fully successful import — so `isPending()` is
// true precisely when no library entry and no media-browser row exists yet.
//
// Threading: import() has the session's (main) thread affinity, because it
// registers into the session. isPending() is safe to call from any thread.

#ifndef PALMIER_SERVICES_MEDIAIMPORTSERVICE_HPP
#define PALMIER_SERVICES_MEDIAIMPORTSERVICE_HPP

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include "core/Error.hpp"
#include "core/FrameRate.hpp"
#include "core/Resolution.hpp"
#include "core/Result.hpp"
#include "core/Uuid.hpp"
#include "media/MediaInfo.hpp"
#include "media/MediaProbe.hpp"

namespace palmier::services {

class ProjectSession;

/// The description of one registered asset returned by a successful import
/// (Requirement 2.2). `resolution` and `frameRate` are present only when the
/// asset carries at least one DECODABLE video stream and that stream declares
/// usable values; for an audio-only asset — or one whose only video stream uses a
/// codec the engine cannot decode — both are absent.
struct ImportedAsset {
    Uuid                      assetId;          ///< Identity within the project media library.
    std::filesystem::path     sourcePath;       ///< Resolved absolute (canonical) source location.
    std::string               containerFormat;  ///< Container format name as probed.
    std::int64_t              durationMs = 0;   ///< Duration in milliseconds.
    std::optional<Resolution> resolution;       ///< Present iff a decodable video stream exists.
    std::optional<FrameRate>  frameRate;        ///< Present iff a decodable video stream exists.
    bool                      hasVideo = false; ///< A decodable video stream is present.
    bool                      hasAudio = false; ///< A decodable audio stream is present.
    bool                      duplicate = false;///< The location was already registered (2.5).
};

/// The classification an import rejection carries. Exposed so callers (and tests)
/// can distinguish the four conditions of Requirement 2.4 from the format
/// rejection of Requirement 2.3 without parsing messages.
enum class ImportFailure {
    None = 0,
    EmptyPath,          ///< 2.4 — the requested path is empty.
    FileNotFound,       ///< 2.4 — the path names a file that does not exist.
    FileUnreadable,     ///< 2.4 — the file cannot be opened or read.
    UndecodableContent, ///< 2.4 — the decodable stream failed to decode.
    UnsupportedFormat,  ///< 2.3 — unrecognised container, or no decodable codec.
    NoProjectOpen,      ///< 2.7 — no project is open.
    TimedOut,           ///< 2.8 — probing/validation exceeded the time limit.
    Internal,           ///< The library refused an otherwise valid registration.
};

/// The sentence fragment an error of this classification states, so the four
/// conditions of Requirement 2.4 are named in exactly one place.
[[nodiscard]] std::string_view describeImportFailure(ImportFailure failure) noexcept;

/// Map a rejection classification onto the coarse ErrorCode it is reported with.
[[nodiscard]] ErrorCode importFailureCode(ImportFailure failure) noexcept;

/// Verifies that a path exists and can be opened for reading. Returns ok() when
/// it can, an ErrorCode::NotFound error when the path does not exist, and an
/// ErrorCode::Io error when it exists but cannot be opened or read (including a
/// directory).
using MediaPathAccessCheck = std::function<Result<void>(const std::filesystem::path&)>;

/// Verifies that the asset's decodable stream really decodes. Returns ok() when it
/// does and any error when it does not; the error's message is folded into the
/// "failed to decode" rejection of Requirement 2.4.
using MediaDecodeCheck =
    std::function<Result<void>(const std::filesystem::path&, const media::MediaInfo&)>;

/// The knobs of one MediaImportService. Declared at namespace scope so it can be
/// the type of a defaulted constructor argument.
struct MediaImportOptions {
    /// The Requirement 2.8 limit on probing and validation. A non-positive value
    /// means "no limit", in which case probing runs inline on the caller's thread.
    std::chrono::milliseconds timeout{std::chrono::seconds{30}};
    /// Empty means "consult the real filesystem"
    /// (MediaImportService::filesystemAccessCheck()).
    MediaPathAccessCheck accessCheck;
    /// Empty means "accept": the decoder is not wired up until a later stage.
    MediaDecodeCheck decodeCheck;
};

class MediaImportService {
public:
    using PathAccessCheck = MediaPathAccessCheck;
    using DecodeCheck = MediaDecodeCheck;
    using Options = MediaImportOptions;

    /// The default, filesystem-backed access check.
    [[nodiscard]] static PathAccessCheck filesystemAccessCheck();

    /// Import into `session`'s media library.
    MediaImportService(ProjectSession& session, media::MediaProbeBackend backend,
                       Options options = Options{});

    /// As above, where a null `session` models "no project is open": every import
    /// registers nothing and reports that no project is open (Requirement 2.7).
    /// This mirrors how McpToolExecutor represents the same state.
    MediaImportService(ProjectSession* session, media::MediaProbeBackend backend,
                       Options options = Options{});

    MediaImportService(const MediaImportService&) = delete;
    MediaImportService& operator=(const MediaImportService&) = delete;
    MediaImportService(MediaImportService&&) = delete;
    MediaImportService& operator=(MediaImportService&&) = delete;

    /// Probe, validate and register `path` as exactly one asset of the current
    /// project's media library (Requirements 2.1, 2.2).
    ///
    /// Returns the existing asset for a path that resolves to an already
    /// registered location, adding no entry (Requirement 2.5). Every failure
    /// leaves the library's entry count and contents unchanged and names the path
    /// (Requirements 2.3, 2.4, 2.7, 2.8).
    [[nodiscard]] Result<ImportedAsset> import(const std::filesystem::path& path);

    /// True while an import of the location `path` resolves to is running and has
    /// therefore produced no library entry and no media-browser row yet
    /// (Requirement 2.9). Thread-safe.
    [[nodiscard]] bool isPending(const std::filesystem::path& path) const;

    /// Number of imports currently in progress. Thread-safe.
    [[nodiscard]] std::size_t pendingCount() const;

    /// The classification of the most recent rejection, or ImportFailure::None
    /// when the most recent import succeeded. Reported for the caller's benefit;
    /// the same information is carried by the returned Error.
    [[nodiscard]] ImportFailure lastFailure() const noexcept { return lastFailure_; }

    /// The time limit this service applies to probing and validation.
    [[nodiscard]] std::chrono::milliseconds timeout() const noexcept { return timeout_; }

    /// Resolve `path` the way the duplicate check does: absolute and canonicalised
    /// with std::filesystem::weakly_canonical, without touching the library.
    [[nodiscard]] static std::filesystem::path resolvePath(const std::filesystem::path& path);

private:
    /// Everything the timed section produces on success.
    struct ProbeOutcome {
        media::MediaInfo info;
    };

    /// RAII marker for the pending set, so every exit path clears it (2.9).
    class PendingMark {
    public:
        PendingMark(MediaImportService& owner, std::string key);
        ~PendingMark();
        PendingMark(const PendingMark&) = delete;
        PendingMark& operator=(const PendingMark&) = delete;

    private:
        MediaImportService& owner_;
        std::string         key_;
    };

    /// Run access check + probe + validation + decode check under the time limit.
    [[nodiscard]] Result<ProbeOutcome> probeAndValidate(const std::filesystem::path& resolved);

    /// Build the ImportedAsset description of a probed file (2.2).
    [[nodiscard]] static ImportedAsset describe(const Uuid& assetId,
                                                const std::filesystem::path& resolved,
                                                const media::MediaInfo& info);

    /// Record a classified rejection and build the Error naming `path`, the
    /// condition that occurred and, where it applies, the offending detail.
    [[nodiscard]] Error makeRejection(ImportFailure failure, const std::filesystem::path& path,
                                      const std::string& detail = {});

    /// The asset already registered for `resolved`, if any.
    [[nodiscard]] std::optional<Uuid> registeredAsset(const std::filesystem::path& resolved) const;

    ProjectSession*           session_ = nullptr;
    media::MediaProbeBackend  backend_;
    std::chrono::milliseconds timeout_;
    PathAccessCheck           accessCheck_;
    DecodeCheck               decodeCheck_;
    ImportFailure             lastFailure_ = ImportFailure::None;

    /// Metadata of the assets this service registered, keyed by canonical path, so
    /// a duplicate request can answer with the first import's description without
    /// probing again (Requirement 2.5).
    std::map<std::string, ImportedAsset> imported_;

    mutable std::mutex    pendingMutex_;
    std::set<std::string> pending_;
};

} // namespace palmier::services

#endif // PALMIER_SERVICES_MEDIAIMPORTSERVICE_HPP
