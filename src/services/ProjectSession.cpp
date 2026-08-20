// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/ProjectSession.cpp — implementation of the Project_Session declared in
// ProjectSession.hpp (design.md D1 and D6; tasks 2.1 and 2.2).
//
// Two ideas drive this file:
//
//   1. Commit-on-full-success. createProject and openProject build a complete
//      Project value (and, for an open, a complete replacement media library) in
//      locals, validate it, and only then call TimelineEngine::reset. Nothing the
//      session owns is touched until that call has succeeded, which is what makes
//      a failed open a no-op down to the modified flag and the undo history.
//
//   2. The save worker touches nothing but the completion queue. It works from a
//      value copy of the project, a copy of the destination path and a copy of the
//      injected RawFileWriter, and hands its outcome back through a mutex-guarded
//      queue. Every decision that depends on session state — the revision guard,
//      the dirty flag, the document path — is taken later, on the owning thread,
//      in pumpSaveCompletions().

#include "services/ProjectSession.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "core/ChangeSet.hpp"
#include "core/ProjectValidation.hpp"
#include "core/Track.hpp"
#include "services/ProjectStore.hpp"

namespace palmier::services {

namespace {

/// The empty project a session starts from: the documented default frame rate,
/// canvas resolution and colour space, a fresh identity, and no content
/// (Requirement 1.10).
Project makeDefaultProject() {
    Project project;
    project.id = Uuid::generateV4();
    project.name = kDefaultProjectName;
    project.timelineFps = kDefaultTimelineFps;
    project.canvas = kDefaultCanvas;
    project.colorSpace = kDefaultProjectColorSpace;
    project.version = SchemaVersion::current();
    return project;
}

std::size_t countClips(const Project& project) {
    std::size_t clips = 0;
    for (const Track& track : project.tracks) {
        clips += track.clips.size();
    }
    return clips;
}

/// True iff `colorSpace` is one of the named spaces the domain core exposes.
/// ColorSpace::Unknown is the "no such space" sentinel and is not selectable.
bool isSelectableColorSpace(ColorSpace colorSpace) {
    switch (colorSpace) {
        case ColorSpace::Srgb:
        case ColorSpace::Rec709:
        case ColorSpace::Rec2020:
        case ColorSpace::Rec2100Pq:
        case ColorSpace::Rec2100Hlg:
        case ColorSpace::DisplayP3:
        case ColorSpace::LinearSrgb:
            return true;
        case ColorSpace::Unknown:
            return false;
    }
    return false;
}

/// Build the media library that matches a loaded project: one entry per asset the
/// document references, in document order. A duplicate or nil asset id in the
/// document is a malformed document, so the whole open is rejected.
Result<MediaManager> libraryFor(const Project& project) {
    MediaManager library;
    for (const MediaAssetRef& asset : project.assets) {
        if (Result<void> imported = library.importAsset(asset); imported.isError()) {
            return makeError(ErrorCode::InvalidArgument,
                             "media asset '" + asset.assetId.toString() +
                                 "' could not be catalogued: " +
                                 std::move(imported).error().message());
        }
    }
    return library;
}

} // namespace

ProjectSession::ProjectSession() : ProjectSession(RawFileWriter{}) {}

ProjectSession::ProjectSession(RawFileWriter writer)
    : writer_(writer ? std::move(writer) : RawFileWriter(&defaultRawFileWriter)),
      engine_(makeDefaultProject()),
      statusObservers_(std::make_shared<StatusObserverRegistry>()) {
    observeEngine();
}

ProjectSession::~ProjectSession() {
    // Let every worker finish (each only appends to the completion queue, so this
    // cannot deadlock) before any member it might still touch goes away. Queued
    // completions are deliberately not delivered: their callbacks may reference
    // objects that are themselves being torn down.
    waitForPendingSaves();
    joinAllWorkers();
    engineSub_.reset();
}

void ProjectSession::observeEngine() {
    engineSub_ = engine_.observe([this](const ChangeSet& change) {
        // A create/open is accounted for by createProject/openProject themselves,
        // which own the "now in step with disk" decision. Every other origin is an
        // edit that leaves the session out of step with disk.
        if (change.origin == ChangeOrigin::Reset) {
            return;
        }
        bumpRevision();
        notifyStatus();
    });
}

ProjectSession::Status ProjectSession::status() const {
    const Project project = engine_.snapshot();
    Status s;
    s.projectId = project.id;
    s.name = project.name;
    s.modified = modified();
    s.documentPath = documentPath_;
    s.trackCount = project.tracks.size();
    s.clipCount = countClips(project);
    s.revision = revision_;
    return s;
}

Subscription ProjectSession::observeStatus(std::function<void(const Status&)> callback) {
    if (!callback) {
        return Subscription{};
    }

    const std::uint64_t id = statusObservers_->nextId++;
    statusObservers_->callbacks.emplace(id, std::move(callback));

    std::weak_ptr<StatusObserverRegistry> weak = statusObservers_;
    return Subscription([weak, id]() noexcept {
        if (auto registry = weak.lock()) {
            registry->callbacks.erase(id);
        }
    });
}

void ProjectSession::notifyStatus() const {
    if (statusObservers_->callbacks.empty()) {
        return;
    }
    const Status snapshot = status();
    // Iterate over a copy so an observer that unsubscribes (or subscribes) while
    // being notified cannot invalidate the iteration.
    const auto callbacks = statusObservers_->callbacks;
    for (const auto& [id, callback] : callbacks) {
        if (callback) {
            callback(snapshot);
        }
    }
}

Result<Uuid> ProjectSession::createProject(std::string name, FrameRate fps, Resolution canvas,
                                          ColorSpace colorSpace) {
    // --- Argument validation (Requirement 3.8): nothing is touched until every
    //     argument is inside its accepted range, and the error names the argument.
    if (name.size() < kMinProjectNameLength || name.size() > kMaxProjectNameLength) {
        return invalidArgument("rejected argument 'name': length " +
                               std::to_string(name.size()) + " is outside " +
                               std::to_string(kMinProjectNameLength) + "-" +
                               std::to_string(kMaxProjectNameLength) + " characters");
    }
    if (!fps.isValid() || fps.numerator() < kMinFramesPerSecond * fps.denominator() ||
        fps.numerator() > kMaxFramesPerSecond * fps.denominator()) {
        return outOfRange("rejected argument 'frameRate': " + std::to_string(fps.numerator()) +
                          "/" + std::to_string(fps.denominator()) + " is outside " +
                          std::to_string(kMinFramesPerSecond) + "-" +
                          std::to_string(kMaxFramesPerSecond) + " frames per second");
    }
    if (canvas.width < kMinCanvasWidth || canvas.width > kMaxCanvasWidth ||
        canvas.height < kMinCanvasHeight || canvas.height > kMaxCanvasHeight) {
        return outOfRange("rejected argument 'canvas': " + std::to_string(canvas.width) + "x" +
                          std::to_string(canvas.height) + " is outside " +
                          std::to_string(kMinCanvasWidth) + "x" +
                          std::to_string(kMinCanvasHeight) + " to " +
                          std::to_string(kMaxCanvasWidth) + "x" +
                          std::to_string(kMaxCanvasHeight) + " pixels");
    }
    if (!isSelectableColorSpace(colorSpace)) {
        return invalidArgument("rejected argument 'colorSpace': not a color space the "
                               "domain core exposes");
    }

    // --- Build the complete value locally, commit only on success.
    Project project;
    project.id = Uuid::generateV4();
    project.name = std::move(name);
    project.timelineFps = fps;
    project.canvas = canvas;
    project.colorSpace = colorSpace;
    project.version = SchemaVersion::current();

    const Uuid newId = project.id;
    if (CommandResult reset = engine_.reset(std::move(project)); !reset.isOk()) {
        return makeError(reset.error().code(),
                         "could not create project: " + reset.error().message());
    }

    // Committed: a new project has an empty media library, no on-disk location,
    // and is unmodified at the (advanced) current revision.
    mediaLibrary_ = MediaManager{};
    documentPath_.reset();
    bumpRevision();
    savedRevision_ = revision_;
    notifyStatus();
    return newId;
}

Result<ProjectSession::Status> ProjectSession::openProject(const std::filesystem::path& path) {
    const std::string where = "'" + path.string() + "'";

    Result<Project> loaded = loadProjectFromFile(path);
    if (loaded.isError()) {
        const Error& cause = loaded.error();
        return makeError(cause.code(), "could not open project " + where + ": " + cause.message());
    }
    Project project = std::move(loaded).value();

    // A structurally parseable document can still be an illegal project (bad frame
    // rate or canvas, an unsupported version, a clip referencing an absent asset).
    if (Result<void> valid = validateProject(project); valid.isError()) {
        const Error& cause = valid.error();
        return makeError(cause.code(), "could not open project " + where + ": " + cause.message());
    }

    Result<MediaManager> library = libraryFor(project);
    if (library.isError()) {
        const Error& cause = library.error();
        return makeError(cause.code(), "could not open project " + where + ": " + cause.message());
    }

    // reset() re-checks the timeline invariants and leaves the current project and
    // both history stacks untouched if the loaded value violates them.
    if (CommandResult reset = engine_.reset(project); !reset.isOk()) {
        return makeError(reset.error().code(),
                         "could not open project " + where + ": " + reset.error().message());
    }

    // Committed: the loaded document is now the current project, its location is
    // known, and it is unmodified.
    mediaLibrary_ = std::move(library).value();
    documentPath_ = path;
    bumpRevision();
    savedRevision_ = revision_;
    notifyStatus();
    return status();
}

void ProjectSession::markModified() {
    bumpRevision();
    notifyStatus();
}

Result<void> ProjectSession::requestSave(const std::filesystem::path& path,
                                         SaveCompletion onDone) {
    if (path.empty()) {
        return invalidArgument("rejected argument 'path': the save destination is empty");
    }

    // Capture (snapshot, revision) on the calling thread; everything the worker
    // needs is a copy, so no session state is read off-thread.
    Project       snapshot = engine_.snapshot();
    const std::uint64_t requestedRevision = revision_;

    CompletionNotifier notifier;
    {
        std::lock_guard<std::mutex> guard(completionMutex_);
        notifier = notifier_;
        ++inFlightSaves_;
    }

    // Thread objects of already-finished saves are reaped here, so a long-lived
    // session does not accumulate them.
    reapFinishedWorkers();

    auto finished = std::make_shared<std::atomic<bool>>(false);
    {
        std::lock_guard<std::mutex> guard(workerMutex_);
        SaveWorker worker;
        worker.finished = finished;
        worker.thread = std::jthread([this, project = std::move(snapshot), path, requestedRevision,
                                      writer = writer_, notifier = std::move(notifier),
                                      onDone = std::move(onDone), finished]() mutable {
            // A service instance per save: the atomic write-to-temp-then-rename
            // sequence (and hence "a previously saved file survives a failure
            // byte-for-byte") lives there.
            ProjectSaveService  service(std::move(writer));
            Result<SaveOutcome> outcome = service.save(project, path);

            QueuedCompletion completion;
            completion.path = path;
            completion.requestedRevision = requestedRevision;
            completion.onDone = std::move(onDone);
            if (outcome.isOk()) {
                completion.succeeded = true;
                completion.bytesWritten = std::move(outcome).value().bytesWritten;
            } else {
                completion.succeeded = false;
                completion.error =
                    makeError(outcome.error().code(),
                              "could not save project to '" + path.string() +
                                  "': " + outcome.error().message());
            }

            {
                std::lock_guard<std::mutex> guard(completionMutex_);
                completions_.push_back(std::move(completion));
                --inFlightSaves_;
            }
            completionCv_.notify_all();

            // Wake the owner so it can pump on its own thread. Called after the
            // completion is queued, so it is already visible when the owner runs.
            if (notifier) {
                notifier();
            }

            // Last action of the worker body: only now may its thread object be
            // reaped, so a reap can never block inside a running callback.
            finished->store(true, std::memory_order_release);
        });
        workers_.push_back(std::move(worker));
    }

    return ok();
}

void ProjectSession::setSaveCompletionNotifier(CompletionNotifier notifier) {
    std::lock_guard<std::mutex> guard(completionMutex_);
    notifier_ = std::move(notifier);
}

std::size_t ProjectSession::pumpSaveCompletions() {
    std::vector<QueuedCompletion> batch;
    {
        std::lock_guard<std::mutex> guard(completionMutex_);
        batch.swap(completions_);
    }

    for (QueuedCompletion& completion : batch) {
        SaveCompletionInfo info;
        info.path = completion.path;
        info.requestedRevision = completion.requestedRevision;
        info.succeeded = completion.succeeded;
        info.bytesWritten = completion.bytesWritten;
        info.error = completion.error;

        if (completion.succeeded) {
            // The bytes on disk are a valid document either way, so the location is
            // always recorded. The dirty flag is only cleared when the project has
            // not moved on since the snapshot was taken (design.md D6).
            documentPath_ = completion.path;
            if (revision_ == completion.requestedRevision) {
                savedRevision_ = completion.requestedRevision;
            }
            info.stillModified = modified();
            notifyStatus();
        } else {
            // A failed save changes nothing: not the project, not its modified
            // state, not its document path, not any previously saved file
            // (Requirements 4.4, 14.7).
            info.stillModified = modified();
        }

        if (completion.onDone) {
            completion.onDone(info);
        }
    }

    return batch.size();
}

void ProjectSession::waitForPendingSaves() {
    std::unique_lock<std::mutex> lock(completionMutex_);
    completionCv_.wait(lock, [this] { return inFlightSaves_ == 0; });
}

std::size_t ProjectSession::awaitSaveCompletions() {
    waitForPendingSaves();
    return pumpSaveCompletions();
}

std::size_t ProjectSession::pendingSaveCount() const {
    std::lock_guard<std::mutex> guard(completionMutex_);
    return inFlightSaves_;
}

void ProjectSession::reapFinishedWorkers() {
    std::lock_guard<std::mutex> guard(workerMutex_);
    std::erase_if(workers_, [](const SaveWorker& worker) {
        return worker.finished->load(std::memory_order_acquire);
    });
}

void ProjectSession::joinAllWorkers() {
    std::lock_guard<std::mutex> guard(workerMutex_);
    workers_.clear(); // std::jthread's destructor joins.
}

} // namespace palmier::services
