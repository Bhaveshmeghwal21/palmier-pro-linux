// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/ProjectSession.hpp — the Project_Session: the single owner of the
// currently open project (design.md decision D1; tasks 2.1 and 2.2).
//
// The session is the answer to "which project is current, where does it live on
// disk, and does it have unsaved changes?". It owns, for its whole lifetime:
//
//   * exactly ONE TimelineEngine. Opening a different project does NOT replace
//     that object: openProject()/createProject() commit through
//     TimelineEngine::reset(), which swaps the project value in place, clears
//     both history stacks and emits a ChangeSet with ChangeOrigin::Reset. Every
//     observer, view model and frame provider holding `TimelineEngine&` plus a
//     Subscription therefore stays valid across a project load, and the panels
//     refresh through the ordinary ChangeSet broadcast (Requirements 3.4, 4.3).
//   * the MediaManager media library of the current project. It is rebuilt from
//     the loaded document's assets when a project is opened and emptied when a
//     project is created, so mediaLibrary() always describes the current project.
//   * the project identity, the optional on-disk document path, a monotonically
//     increasing revision counter, and the modified flag derived from it.
//
// Revision counter and the modified flag
// --------------------------------------
// `revision()` never decreases for the lifetime of the session. It advances on
// every state change: an engine-originated edit (ChangeOrigin::Apply/Undo/Redo),
// an out-of-engine change reported through markModified(), and a project
// create/open. The session separately remembers the revision that is known to be
// on disk, and `modified()` is simply "the current revision is not the saved
// revision". That gives both the dirty flag (Requirement 4.6) and the off-thread
// save guard (design.md D6) a single precise definition.
//
// The session marks ITSELF modified for every engine-originated change: it
// observes its own engine and advances the revision on ChangeOrigin::Apply,
// ::Undo and ::Redo. markModified() exists for state that changes OUTSIDE the
// engine — for example a media-library import — and must therefore NOT be called
// after an engine edit, which has already been accounted for.
//
// createProject / openProject are not undoable
// --------------------------------------------
// Neither is an EditCommand. Each builds a complete Project value in a local
// variable, validates it, and only calls reset() on FULL success. A failed
// create or open therefore leaves the previous project, its document path, its
// modified flag and its undo history completely unchanged (Requirements 3.9,
// 4.10). And because reset() emits ChangeOrigin::Reset rather than
// ChangeOrigin::Apply, a consumer that counts applied commands to drive rollback
// never counts a project load and will never try to "undo" one.
//
// Saving never blocks the caller (design.md D6; upstream PR 403)
// -------------------------------------------------------------
// requestSave(path, onDone) captures (Project snapshot, revision r), hands them
// to a std::jthread that runs ProjectSaveService::save, and returns immediately —
// so a slow or failing write cannot stall the UI thread (Requirements 14.6).
// Completion is NOT applied on the worker: the worker only appends a record to an
// internal, mutex-guarded completion queue and wakes the owner through the
// optional save-completion notifier. The owning (main) thread then applies it in
// pumpSaveCompletions():
//
//   * success and revision() == r  -> record the path and clear the dirty flag;
//   * success and revision() >  r  -> the user edited during the write, so the
//                                     written file is still a valid document:
//                                     record the path but stay modified;
//   * failure                      -> the in-memory project, its modified state,
//                                     its document path and any previously saved
//                                     file are left untouched, and the reported
//                                     error names the destination (Requirements
//                                     4.4, 14.7).
//
// Threading contract (design.md D5): every member of this class has main-thread
// affinity except the completion queue. Only the save worker touches that queue,
// and it touches nothing else — it works from a value copy of the project, a copy
// of the destination path and a copy of the injected RawFileWriter. An injected
// writer must therefore be safe to call on a worker thread.

#ifndef PALMIER_SERVICES_PROJECTSESSION_HPP
#define PALMIER_SERVICES_PROJECTSESSION_HPP

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/ColorSpace.hpp"
#include "core/Error.hpp"
#include "core/FrameRate.hpp"
#include "core/MediaManager.hpp"
#include "core/Project.hpp"
#include "core/Resolution.hpp"
#include "core/Result.hpp"
#include "core/Subscription.hpp"
#include "core/TimelineEngine.hpp"
#include "core/Uuid.hpp"
#include "services/ProjectSaveService.hpp"

namespace palmier::services {

/// The documented defaults a session's empty project is created with when no
/// project path is supplied at startup (Requirement 1.10).
inline constexpr FrameRate  kDefaultTimelineFps = FrameRate::fps30();
inline constexpr Resolution kDefaultCanvas = Resolution::hd1080();
inline constexpr ColorSpace kDefaultProjectColorSpace = defaultColorSpace();
inline constexpr const char* kDefaultProjectName = "Untitled Project";

/// The accepted argument ranges for createProject (Requirements 3.2, 3.8). They
/// are declared here so the `project.create` tool schema and this session enforce
/// exactly one set of bounds.
inline constexpr std::size_t   kMinProjectNameLength = 1;
inline constexpr std::size_t   kMaxProjectNameLength = 128;
inline constexpr std::int64_t  kMinFramesPerSecond = 1;
inline constexpr std::int64_t  kMaxFramesPerSecond = 240;
inline constexpr std::uint32_t kMinCanvasWidth = 16;
inline constexpr std::uint32_t kMaxCanvasWidth = 7680;
inline constexpr std::uint32_t kMinCanvasHeight = 16;
inline constexpr std::uint32_t kMaxCanvasHeight = 4320;

class ProjectSession {
public:
    /// An immutable description of the session at one point in time. Everything a
    /// title bar, a `project.info` reply or a save prompt needs.
    struct Status {
        Uuid                                 projectId;
        std::string                          name;
        bool                                 modified = false;
        std::optional<std::filesystem::path> documentPath;
        std::size_t                          trackCount = 0;
        std::size_t                          clipCount = 0;
        std::uint64_t                        revision = 0;
    };

    /// The outcome of one requestSave(), delivered on the owning thread after the
    /// revision guard has been applied.
    struct SaveCompletionInfo {
        std::filesystem::path path;              ///< The destination that was requested.
        std::uint64_t         requestedRevision = 0; ///< The revision the snapshot was taken at.
        bool                  succeeded = false;
        std::size_t           bytesWritten = 0;  ///< Meaningful only on success.
        Error                 error;             ///< Meaningful only on failure; names the destination.
        /// The session's modified flag AFTER the guard ran. True after a
        /// successful save means the user edited the project during the write.
        bool                  stillModified = false;
    };

    /// Called on the owning thread when a save finishes. May be empty.
    using SaveCompletion = std::function<void(const SaveCompletionInfo&)>;

    /// Called on the SAVE WORKER thread once a completion has been queued, so the
    /// owner can schedule pumpSaveCompletions() on its own thread (the Qt
    /// composition posts to the main thread). Must be thread-safe. May be empty,
    /// in which case the owner is expected to pump on its own cadence.
    using CompletionNotifier = std::function<void()>;

    /// Constructs a session holding an empty project at the documented default
    /// frame rate, canvas resolution and colour space, with no document path and
    /// an unmodified state (Requirement 1.10).
    ProjectSession();

    /// As above, but with the low-level file writer used by saves injected. This
    /// is the seam through which the save-failure tests inject insufficient
    /// space / insufficient permissions / inaccessible location. The writer is
    /// copied into each save worker, so it must be safe to call off the owning
    /// thread. An empty writer falls back to the default std::ofstream writer.
    explicit ProjectSession(RawFileWriter writer);

    /// Waits for every in-flight save worker to finish before tearing down. Queued
    /// completions are NOT delivered during destruction.
    ~ProjectSession();

    ProjectSession(const ProjectSession&) = delete;
    ProjectSession& operator=(const ProjectSession&) = delete;
    ProjectSession(ProjectSession&&) = delete;
    ProjectSession& operator=(ProjectSession&&) = delete;

    // --- The owned components ---------------------------------------------

    /// The one timeline engine of this session. The reference is stable for the
    /// session's whole lifetime, across every createProject/openProject.
    [[nodiscard]] TimelineEngine& engine() noexcept { return engine_; }
    [[nodiscard]] const TimelineEngine& engine() const noexcept { return engine_; }

    /// The media library of the CURRENT project. Emptied by createProject and
    /// rebuilt from the loaded document's assets by openProject.
    [[nodiscard]] MediaManager& mediaLibrary() noexcept { return mediaLibrary_; }
    [[nodiscard]] const MediaManager& mediaLibrary() const noexcept { return mediaLibrary_; }

    // --- Observation -------------------------------------------------------

    [[nodiscard]] Status status() const;
    [[nodiscard]] bool modified() const noexcept { return revision_ != savedRevision_; }
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    [[nodiscard]] const std::optional<std::filesystem::path>& documentPath() const noexcept {
        return documentPath_;
    }

    /// Register `callback` to receive the new Status after every session state
    /// change (create, open, edit, out-of-engine modification, applied save
    /// completion). Returns an RAII Subscription; destroying it unregisters.
    [[nodiscard]] Subscription observeStatus(std::function<void(const Status&)> callback);

    // --- Session-level operations (not EditCommands, not undoable) ----------

    /// Create a project with exactly the supplied settings and make it current
    /// (Requirement 3.2). The project is reported unmodified with no document
    /// path, and its identifier is unique within the session.
    ///
    /// Rejects, leaving the current project completely unchanged, an argument
    /// outside its accepted range, naming the rejected argument (Requirement 3.8):
    /// a name outside 1-128 characters, a frame rate that is not a valid rational
    /// in [1, 240] frames per second, a canvas outside 16-7680 x 16-4320 pixels,
    /// or ColorSpace::Unknown.
    [[nodiscard]] Result<Uuid> createProject(std::string name, FrameRate fps, Resolution canvas,
                                            ColorSpace colorSpace);

    /// Load the `.palmier` document at `path` and make it the current project,
    /// recording `path` as the document path and reporting the project unmodified
    /// (Requirement 3.4). Returns the resulting Status (project id, track count,
    /// clip count, ...).
    ///
    /// A missing, unreadable, malformed, unsupported-schema or invariant-violating
    /// document is rejected with an error naming the path and the failure reason,
    /// and leaves the previous project, its document path, its modified flag and
    /// its undo history unchanged (Requirements 3.9, 4.10).
    [[nodiscard]] Result<Status> openProject(const std::filesystem::path& path);

    /// Start writing the current project to `path` on a worker thread and return
    /// immediately (Requirements 4.1, 14.6). `onDone`, if set, is invoked on the
    /// owning thread from pumpSaveCompletions() once the write has finished and
    /// the revision guard has been applied.
    ///
    /// Returns an error, having started no worker, only for a request that cannot
    /// be attempted at all (an empty destination path).
    [[nodiscard]] Result<void> requestSave(const std::filesystem::path& path,
                                           SaveCompletion onDone = {});

    /// Record that the project changed outside the engine (for example a
    /// media-library import), advancing the revision and marking the session
    /// modified. Must NOT be called after an engine edit: the session already
    /// accounts for every ChangeOrigin::Apply/Undo/Redo itself.
    void markModified();

    // --- Save completion plumbing -----------------------------------------

    /// Install the notifier the save workers use to wake the owning thread.
    void setSaveCompletionNotifier(CompletionNotifier notifier);

    /// Apply and deliver every queued save completion on the calling (owning)
    /// thread. Returns how many completions were delivered.
    [[nodiscard]] std::size_t pumpSaveCompletions();

    /// Block until every in-flight save worker has finished and queued its
    /// completion. Does not deliver anything — call pumpSaveCompletions() next.
    void waitForPendingSaves();

    /// waitForPendingSaves() followed by pumpSaveCompletions(); returns the number
    /// of delivered completions.
    [[nodiscard]] std::size_t awaitSaveCompletions();

    /// Number of save workers that have not yet queued their completion.
    [[nodiscard]] std::size_t pendingSaveCount() const;

private:
    /// A finished save, queued by a worker and applied by the owning thread.
    struct QueuedCompletion {
        std::filesystem::path path;
        std::uint64_t         requestedRevision = 0;
        bool                  succeeded = false;
        std::size_t           bytesWritten = 0;
        Error                 error;
        SaveCompletion        onDone;
    };

    /// One save worker plus the flag it sets as its very last action, so a
    /// finished worker's thread object can be reaped without ever blocking the
    /// owning thread inside a join.
    struct SaveWorker {
        std::shared_ptr<std::atomic<bool>> finished;
        std::jthread                       thread;
    };

    struct StatusObserverRegistry {
        std::unordered_map<std::uint64_t, std::function<void(const Status&)>> callbacks;
        std::uint64_t nextId = 1;
    };

    /// Wire the engine observer that keeps the revision in step with every
    /// engine-originated change. Called by both constructors.
    void observeEngine();

    /// Advance the revision (the session is now out of step with disk).
    void bumpRevision() noexcept { ++revision_; }

    /// Publish the current Status to every registered observer.
    void notifyStatus() const;

    /// Reap the thread objects of workers that have run to completion.
    void reapFinishedWorkers();

    /// Join every save worker thread. Safe to call repeatedly.
    void joinAllWorkers();

    // Order matters: engineSub_ and the worker threads are declared AFTER the
    // state they touch so they are destroyed FIRST.
    RawFileWriter                            writer_;
    TimelineEngine                           engine_;
    MediaManager                             mediaLibrary_;
    std::optional<std::filesystem::path>     documentPath_;
    std::uint64_t                            revision_ = 0;
    std::uint64_t                            savedRevision_ = 0;
    std::shared_ptr<StatusObserverRegistry>  statusObservers_;

    mutable std::mutex                       completionMutex_;
    std::condition_variable                  completionCv_;
    std::vector<QueuedCompletion>            completions_;
    std::size_t                              inFlightSaves_ = 0;
    CompletionNotifier                       notifier_;

    std::mutex                               workerMutex_;
    std::vector<SaveWorker>                  workers_;

    Subscription                             engineSub_;
};

} // namespace palmier::services

#endif // PALMIER_SERVICES_PROJECTSESSION_HPP
