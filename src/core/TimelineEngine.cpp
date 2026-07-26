// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/TimelineEngine.cpp — implementation of the authoritative timeline model.
//
// See TimelineEngine.hpp for the behavioural contract. Two ideas drive this file:
//
//   1. Atomicity by copy-and-restore. Before applying (or, for undo/redo, before
//      delegating to the stack) the engine keeps a full copy of the Project. If
//      the command fails or the resulting state would violate a timeline
//      invariant, the copy is restored, guaranteeing the project is left exactly
//      as it was (Requirement 6.6 — no partial mutation). Project and all its
//      nested value types are plain copyable structs, so a copy is a faithful
//      snapshot.
//
//   2. Diff-based change notification. The granular ChangeSet emitted to
//      observers is computed by comparing the before/after projects rather than
//      by asking the command what it did. This keeps notification uniform across
//      apply/undo/redo and independent of the concrete command implementations
//      (added in task 3.3).

#include "core/TimelineEngine.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/Error.hpp"
#include "core/ProjectValidation.hpp"
#include "core/Track.hpp"

namespace palmier {

namespace {

// Compare two effects for the purpose of change detection.
bool effectsEqual(const Effect& a, const Effect& b) {
    return a.id == b.id && a.type == b.type && a.parameters == b.parameters;
}

// Compare two optional transitions for change detection.
bool transitionsEqual(const std::optional<Transition>& a,
                      const std::optional<Transition>& b) {
    if (a.has_value() != b.has_value()) {
        return false;
    }
    if (!a.has_value()) {
        return true;
    }
    return a->id == b->id && a->kind == b->kind && a->duration == b->duration;
}

// True iff two clips are observably identical across every model field. Used to
// classify a clip present in both the before and after projects as "modified".
bool clipsEqual(const Clip& a, const Clip& b) {
    if (!(a.id == b.id) || a.assetRef != b.assetRef ||
        a.timelineStart != b.timelineStart || a.sourceIn != b.sourceIn ||
        a.sourceOut != b.sourceOut || a.gain != b.gain || a.opacity != b.opacity) {
        return false;
    }
    if (a.effects.size() != b.effects.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.effects.size(); ++i) {
        if (!effectsEqual(a.effects[i], b.effects[i])) {
            return false;
        }
    }
    return transitionsEqual(a.transitionIn, b.transitionIn);
}

// A clip together with the track it belongs to, indexed by clip id for diffing.
struct LocatedClip {
    const Clip* clip = nullptr;
    Uuid        trackId;
};

std::unordered_map<ClipId, LocatedClip> indexClips(const Project& project) {
    std::unordered_map<ClipId, LocatedClip> byId;
    for (const Track& track : project.tracks) {
        for (const Clip& clip : track.clips) {
            byId.emplace(clip.id, LocatedClip{&clip, track.id});
        }
    }
    return byId;
}

// Record a track id in `affected` if not already present (order preserved).
void noteTrack(std::vector<Uuid>& affected, const Uuid& trackId) {
    if (std::find(affected.begin(), affected.end(), trackId) == affected.end()) {
        affected.push_back(trackId);
    }
}

// Build the granular ChangeSet describing the transition from `before` to
// `after`, tagged with the operation origin and command description.
ChangeSet buildChangeSet(const Project& before, const Project& after,
                         ChangeOrigin origin, std::string description) {
    ChangeSet change;
    change.origin = origin;
    change.description = std::move(description);
    change.previousDuration = timelineDuration(before);
    change.currentDuration = timelineDuration(after);

    const std::unordered_map<ClipId, LocatedClip> beforeById = indexClips(before);
    const std::unordered_map<ClipId, LocatedClip> afterById = indexClips(after);

    // Added / modified: iterate the after-state so we can report the current
    // track of each clip.
    for (const auto& [id, located] : afterById) {
        const auto prior = beforeById.find(id);
        if (prior == beforeById.end()) {
            change.addedClips.push_back(id);
            noteTrack(change.affectedTracks, located.trackId);
        } else if (!clipsEqual(*prior->second.clip, *located.clip)) {
            change.modifiedClips.push_back(id);
            noteTrack(change.affectedTracks, located.trackId);
            // A clip that moved between tracks touches both lanes.
            if (!(prior->second.trackId == located.trackId)) {
                noteTrack(change.affectedTracks, prior->second.trackId);
            }
        }
    }

    // Removed: present before, absent after.
    for (const auto& [id, located] : beforeById) {
        if (afterById.find(id) == afterById.end()) {
            change.removedClips.push_back(id);
            noteTrack(change.affectedTracks, located.trackId);
        }
    }

    return change;
}

} // namespace

Duration timelineDuration(const Project& project) {
    Duration end = Duration::zero();
    for (const Track& track : project.tracks) {
        for (const Clip& clip : track.clips) {
            const Duration clipEnd = clip.timelineEnd();
            if (clipEnd > end) {
                end = clipEnd;
            }
        }
    }
    return end;
}

Result<void> checkTimelineInvariants(const Project& project) {
    for (const Track& track : project.tracks) {
        const std::vector<Clip>& clips = track.clips;

        // Per-clip intrinsic rules plus the non-negative-position rule.
        for (const Clip& clip : clips) {
            if (auto r = validateClip(clip); !r) {
                return r;  // sourceOut > sourceIn, opacity, gain, transition >= 0
            }
            if (clip.timelineStart.isNegative()) {
                return outOfRange(
                    "Clip " + (clip.id.isNil() ? std::string{"<nil>"} : clip.id.toString()) +
                    ": timelineStart must be >= 0");
            }
        }

        // Ordering and non-overlap between consecutive clips. Adjacent clips may
        // overlap only within the incoming clip's explicit transition region
        // (design.md Track validation).
        for (std::size_t i = 1; i < clips.size(); ++i) {
            const Clip& previous = clips[i - 1];
            const Clip& current = clips[i];

            if (current.timelineStart < previous.timelineStart) {
                return failedPrecondition(
                    "Track " + track.id.toString() +
                    ": clips must be ordered by timelineStart");
            }

            // Permitted overlap = the incoming clip's transition region length.
            const Duration allowedOverlap =
                current.transitionIn.has_value() ? current.transitionIn->duration
                                                 : Duration::zero();
            const Duration overlap = previous.timelineEnd() - current.timelineStart;
            if (overlap > allowedOverlap) {
                return failedPrecondition(
                    "Track " + track.id.toString() +
                    ": clips overlap outside an explicit transition region");
            }
        }
    }

    return ok();
}

TimelineEngine::TimelineEngine()
    : project_(), undoStack_(), observers_(std::make_shared<ObserverRegistry>()) {}

TimelineEngine::TimelineEngine(Project initial, std::size_t undoCapacity)
    : project_(std::move(initial)),
      undoStack_(undoCapacity),
      observers_(std::make_shared<ObserverRegistry>()) {
    // The engine assumes a valid starting point; callers constructing a project
    // directly should validate it first. Enforced only in debug builds.
    assert(checkTimelineInvariants(project_).isOk() &&
           "TimelineEngine constructed with an invariant-violating project");
}

TimelineEngine::~TimelineEngine() = default;

Project TimelineEngine::snapshot() const { return project_; }

std::optional<Clip> TimelineEngine::clip(ClipId id) const {
    for (const Track& track : project_.tracks) {
        for (const Clip& clip : track.clips) {
            if (clip.id == id) {
                return clip;
            }
        }
    }
    return std::nullopt;
}

Duration TimelineEngine::duration() const { return timelineDuration(project_); }

CommandResult TimelineEngine::apply(std::unique_ptr<EditCommand> cmd) {
    if (!cmd) {
        return CommandResult::failed(
            invalidArgument("TimelineEngine::apply called with a null command"));
    }

    // Snapshot for atomic rollback and for diff-based change notification.
    Project before = project_;
    std::string description(cmd->name());

    // 1. Perform the edit. On failure, restore the snapshot so the project is
    //    left byte-for-byte unchanged (Requirement 6.6).
    if (Result<void> applied = cmd->apply(project_); applied.isError()) {
        project_ = std::move(before);
        return CommandResult::failed(std::move(applied).error());
    }

    // 2. Enforce the timeline invariants on the resulting state. A command that
    //    produces an invalid timeline is rejected and rolled back — again no
    //    partial mutation.
    if (Result<void> invariants = checkTimelineInvariants(project_); invariants.isError()) {
        project_ = std::move(before);
        return CommandResult::failed(std::move(invariants).error());
    }

    // 3. Success: record the command for undo, then notify observers with a
    //    granular ChangeSet before returning.
    undoStack_.record(std::move(cmd));
    notifyObservers(buildChangeSet(before, project_, ChangeOrigin::Apply, description));
    return CommandResult::applied(std::move(description));
}

CommandResult TimelineEngine::undo() {
    Project before = project_;
    CommandResult result = undoStack_.undo(project_);
    if (result.changed()) {
        notifyObservers(buildChangeSet(before, project_, ChangeOrigin::Undo, result.message()));
    }
    return result;
}

CommandResult TimelineEngine::redo() {
    Project before = project_;
    CommandResult result = undoStack_.redo(project_);
    if (result.changed()) {
        notifyObservers(buildChangeSet(before, project_, ChangeOrigin::Redo, result.message()));
    }
    return result;
}

CommandResult TimelineEngine::reset(Project initial) {
    // A project load is committed only if the incoming value is itself a legal
    // timeline; otherwise the current project and both histories are untouched.
    if (Result<void> invariants = checkTimelineInvariants(initial); invariants.isError()) {
        return CommandResult::failed(std::move(invariants).error());
    }

    Project before = std::move(project_);
    project_ = std::move(initial);

    // The new project carries no editing history: neither the commands applied to
    // the previous project nor the ones undone on it are meaningful here.
    undoStack_.clear();

    // Always notify — the views must refresh to the loaded state even when the
    // diff happens to be empty. Origin Reset (not Apply) keeps this out of any
    // applied-command counter used for rollback.
    notifyObservers(buildChangeSet(before, project_, ChangeOrigin::Reset, "Reset"));
    return CommandResult::applied("Reset");
}

Subscription TimelineEngine::observe(std::function<void(const ChangeSet&)> callback) {
    if (!callback) {
        return Subscription{};  // inactive handle; nothing registered
    }

    const std::uint64_t id = observers_->nextId++;
    observers_->callbacks.emplace(id, std::move(callback));

    // Capture a weak reference so the unsubscribe thunk stays safe even if the
    // engine (and its registry) is destroyed before the Subscription.
    std::weak_ptr<ObserverRegistry> weak = observers_;
    return Subscription([weak, id]() noexcept {
        if (auto registry = weak.lock()) {
            registry->callbacks.erase(id);
        }
    });
}

void TimelineEngine::notifyObservers(const ChangeSet& change) const {
    // Iterate over a snapshot of the callbacks so an observer that unsubscribes
    // (or subscribes) from within its callback cannot invalidate the iteration.
    std::vector<std::function<void(const ChangeSet&)>> current;
    current.reserve(observers_->callbacks.size());
    for (const auto& [id, callback] : observers_->callbacks) {
        current.push_back(callback);
    }
    for (const auto& callback : current) {
        if (callback) {
            callback(change);
        }
    }
}

} // namespace palmier
