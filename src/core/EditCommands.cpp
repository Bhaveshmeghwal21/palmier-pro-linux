// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/EditCommands.cpp — implementations of the concrete editing commands
// declared in EditCommands.hpp (task 3.3).
//
// The commands share a handful of small helpers (locating a track or clip, a
// per-track ordering/overlap check that mirrors the engine's timeline invariant,
// and a Duration clamp). Each command captures the minimal prior state needed to
// make revert() an exact inverse of apply(); where a command restructures a
// track's ordering (move, reorder) it captures a copy of the affected track's
// clip vector, which is the simplest bulletproof way to restore the prior state.

#include "core/EditCommands.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <utility>

#include "core/Error.hpp"
#include "core/Project.hpp"
#include "core/Track.hpp"

namespace palmier {

namespace {

// Locate a track by id. Returns nullptr when no track carries the id.
Track* findTrack(Project& project, const Uuid& trackId) {
    for (Track& track : project.tracks) {
        if (track.id == trackId) {
            return &track;
        }
    }
    return nullptr;
}

// A located clip: the track that holds it and its index within that track.
struct ClipLocation {
    Track*      track = nullptr;
    std::size_t index = 0;
};

// Locate a clip by id across every track. Returns std::nullopt when not found.
std::optional<ClipLocation> findClip(Project& project, const ClipId& clipId) {
    for (Track& track : project.tracks) {
        for (std::size_t i = 0; i < track.clips.size(); ++i) {
            if (track.clips[i].id == clipId) {
                return ClipLocation{&track, i};
            }
        }
    }
    return std::nullopt;
}

// True iff a track's clips are ordered by timelineStart and do not overlap
// outside the incoming (later) clip's explicit transition region. This mirrors
// the per-track portion of TimelineEngine's checkTimelineInvariants so a command
// can reject an offending edit itself with a clear, domain-specific message
// before the engine's generic invariant check would.
bool trackOrderedAndNonOverlapping(const Track& track) {
    const std::vector<Clip>& clips = track.clips;
    for (std::size_t i = 1; i < clips.size(); ++i) {
        const Clip& previous = clips[i - 1];
        const Clip& current = clips[i];

        if (current.timelineStart < previous.timelineStart) {
            return false;  // not sorted by timelineStart
        }

        const Duration allowedOverlap = current.transitionIn.has_value()
                                            ? current.transitionIn->duration
                                            : Duration::zero();
        const Duration overlap = previous.timelineEnd() - current.timelineStart;
        if (overlap > allowedOverlap) {
            return false;  // overlaps outside a transition region
        }
    }
    return true;
}

// Clamp `value` into the closed interval [lo, hi]. Precondition: lo <= hi.
Duration clampDuration(Duration value, Duration lo, Duration hi) {
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

// A short, stable label for a clip id in error messages.
std::string idLabel(const Uuid& id) {
    return id.isNil() ? std::string{"<nil>"} : id.toString();
}

// The tool-surface spelling of a track kind, used in error messages so a
// rejected `kind` argument is named the way the caller supplied it.
std::string_view trackKindLabel(TrackKind kind) {
    return kind == TrackKind::Audio ? "audio" : "video";
}

}  // namespace

// ===========================================================================
// AddClipCommand
// ===========================================================================

AddClipCommand::AddClipCommand(Uuid trackId, Clip clip)
    : trackId_(trackId), clip_(std::move(clip)) {}

Result<void> AddClipCommand::apply(Project& project) {
    Track* track = findTrack(project, trackId_);
    if (track == nullptr) {
        return err(notFound("AddClipCommand: track " + idLabel(trackId_) + " not found"));
    }

    // Insert keeping the track ordered by timelineStart: before the first clip
    // that starts strictly later than the new clip.
    auto pos = std::find_if(track->clips.begin(), track->clips.end(),
                            [&](const Clip& existing) {
                                return existing.timelineStart > clip_.timelineStart;
                            });
    track->clips.insert(pos, clip_);
    return ok();
}

Result<void> AddClipCommand::revert(Project& project) {
    Track* track = findTrack(project, trackId_);
    if (track == nullptr) {
        return err(notFound("AddClipCommand: track " + idLabel(trackId_) + " not found"));
    }
    auto& clips = track->clips;
    clips.erase(std::remove_if(clips.begin(), clips.end(),
                               [&](const Clip& c) { return c.id == clip_.id; }),
                clips.end());
    return ok();
}

// ===========================================================================
// DeleteClipCommand
// ===========================================================================

DeleteClipCommand::DeleteClipCommand(ClipId clipId) : clipId_(clipId) {}

Result<void> DeleteClipCommand::apply(Project& project) {
    std::optional<ClipLocation> loc = findClip(project, clipId_);
    if (!loc) {
        return err(notFound("DeleteClipCommand: clip " + idLabel(clipId_) + " not found"));
    }

    trackId_ = loc->track->id;
    index_ = loc->index;
    removed_ = loc->track->clips[loc->index];
    loc->track->clips.erase(loc->track->clips.begin() +
                            static_cast<std::ptrdiff_t>(loc->index));
    return ok();
}

Result<void> DeleteClipCommand::revert(Project& project) {
    if (!removed_) {
        return err(failedPrecondition("DeleteClipCommand: revert before a successful apply"));
    }
    Track* track = findTrack(project, trackId_);
    if (track == nullptr) {
        return err(notFound("DeleteClipCommand: track " + idLabel(trackId_) + " not found"));
    }
    const std::size_t index = std::min(index_, track->clips.size());
    track->clips.insert(track->clips.begin() + static_cast<std::ptrdiff_t>(index),
                        *removed_);
    return ok();
}

// ===========================================================================
// MoveClipCommand
// ===========================================================================

MoveClipCommand::MoveClipCommand(ClipId clipId, Duration newStart)
    : clipId_(clipId), newStart_(newStart) {}

Result<void> MoveClipCommand::apply(Project& project) {
    std::optional<ClipLocation> loc = findClip(project, clipId_);
    if (!loc) {
        return err(notFound("MoveClipCommand: clip " + idLabel(clipId_) + " not found"));
    }
    if (newStart_.isNegative()) {
        return err(failedPrecondition(
            "MoveClipCommand: destination position must be >= 0"));
    }

    Track* track = loc->track;
    trackId_ = track->id;
    priorClips_ = track->clips;  // capture for an exact revert / rollback
    captured_ = true;

    // Move the clip and restore the track's timelineStart ordering.
    track->clips[loc->index].timelineStart = newStart_;
    std::stable_sort(track->clips.begin(), track->clips.end(),
                     [](const Clip& a, const Clip& b) {
                         return a.timelineStart < b.timelineStart;
                     });

    // Requirement 2.3: an overlapping drop is rejected and the clip retained at
    // its original position, so the editor can indicate the drop is invalid.
    if (!trackOrderedAndNonOverlapping(*track)) {
        track->clips = priorClips_;
        return err(failedPrecondition(
            "MoveClipCommand: destination overlaps an existing clip on the track; "
            "move rejected"));
    }
    return ok();
}

Result<void> MoveClipCommand::revert(Project& project) {
    if (!captured_) {
        return err(failedPrecondition("MoveClipCommand: revert before a successful apply"));
    }
    Track* track = findTrack(project, trackId_);
    if (track == nullptr) {
        return err(notFound("MoveClipCommand: track " + idLabel(trackId_) + " not found"));
    }
    track->clips = priorClips_;
    return ok();
}

// ===========================================================================
// TrimClipCommand
// ===========================================================================

TrimClipCommand::TrimClipCommand(ClipId clipId, Edge edge, Duration newBoundary,
                                 FrameRate fps, Duration sourceDuration)
    : clipId_(clipId),
      edge_(edge),
      newBoundary_(newBoundary),
      fps_(fps),
      sourceDuration_(sourceDuration) {}

Result<void> TrimClipCommand::apply(Project& project) {
    std::optional<ClipLocation> loc = findClip(project, clipId_);
    if (!loc) {
        return err(notFound("TrimClipCommand: clip " + idLabel(clipId_) + " not found"));
    }
    Clip& clip = loc->track->clips[loc->index];

    // The minimum clip duration is one frame at the timeline rate; fall back to a
    // single tick if the rate is not usable so the clip still stays non-empty.
    Duration minDuration = fps_.isValid() ? fps_.frameDuration() : Duration::fromNanoseconds(1);
    if (!minDuration.isPositive()) {
        minDuration = Duration::fromNanoseconds(1);
    }
    if (sourceDuration_ < minDuration) {
        return err(failedPrecondition(
            "TrimClipCommand: source duration is shorter than the one-frame minimum"));
    }

    // Capture prior edge state for an exact revert.
    priorSourceIn_ = clip.sourceIn;
    priorSourceOut_ = clip.sourceOut;
    priorTimelineStart_ = clip.timelineStart;
    captured_ = true;

    if (edge_ == Edge::End) {
        // New out-point in [sourceIn + 1 frame, sourceDuration]; start unchanged.
        const Duration lo = clip.sourceIn + minDuration;
        if (lo > sourceDuration_) {
            return err(failedPrecondition(
                "TrimClipCommand: cannot keep a one-frame minimum within the source"));
        }
        clip.sourceOut = clampDuration(newBoundary_, lo, sourceDuration_);
    } else {
        // New in-point in [0, sourceOut - 1 frame]; shift the clip so the
        // retained content keeps its position on the timeline.
        const Duration hi = clip.sourceOut - minDuration;
        if (hi.isNegative()) {
            return err(failedPrecondition(
                "TrimClipCommand: cannot keep a one-frame minimum within the source"));
        }
        const Duration newIn = clampDuration(newBoundary_, Duration::zero(), hi);
        const Duration delta = newIn - clip.sourceIn;
        clip.sourceIn = newIn;
        Duration newStart = clip.timelineStart + delta;
        if (newStart.isNegative()) {
            newStart = Duration::zero();  // keep timelineStart within the valid range
        }
        clip.timelineStart = newStart;
    }
    return ok();
}

Result<void> TrimClipCommand::revert(Project& project) {
    if (!captured_) {
        return err(failedPrecondition("TrimClipCommand: revert before a successful apply"));
    }
    std::optional<ClipLocation> loc = findClip(project, clipId_);
    if (!loc) {
        return err(notFound("TrimClipCommand: clip " + idLabel(clipId_) + " not found"));
    }
    Clip& clip = loc->track->clips[loc->index];
    clip.sourceIn = priorSourceIn_;
    clip.sourceOut = priorSourceOut_;
    clip.timelineStart = priorTimelineStart_;
    return ok();
}

// ===========================================================================
// SplitClipCommand
// ===========================================================================

SplitClipCommand::SplitClipCommand(ClipId clipId, Duration playhead)
    : clipId_(clipId), playhead_(playhead) {}

Result<void> SplitClipCommand::apply(Project& project) {
    std::optional<ClipLocation> loc = findClip(project, clipId_);
    if (!loc) {
        return err(notFound("SplitClipCommand: clip " + idLabel(clipId_) + " not found"));
    }
    Track* track = loc->track;
    const Clip original = track->clips[loc->index];

    // Requirement 2.6: the playhead must fall strictly inside the clip. At or
    // outside a boundary there is nothing to split — fail without mutating.
    if (playhead_ <= original.timelineStart || playhead_ >= original.timelineEnd()) {
        return err(failedPrecondition(
            "SplitClipCommand: playhead is not within the clip; nothing to split"));
    }

    // Offset from the clip's leading edge, in both timeline and source time
    // (the model has no speed change, so they coincide).
    const Duration offset = playhead_ - original.timelineStart;

    // Fix the right half's id on the first apply so redo reproduces it exactly.
    if (!rightId_) {
        rightId_ = Uuid::generateV4();
    }

    // Left half: keeps id, timelineStart, transitionIn, effects; ends at split.
    Clip left = original;
    left.sourceOut = original.sourceIn + offset;

    // Right half: fresh id, starts at the playhead, carries the remainder; an
    // interior cut is contiguous so it has no incoming transition.
    Clip right = original;
    right.id = *rightId_;
    right.sourceIn = original.sourceIn + offset;
    right.sourceOut = original.sourceOut;
    right.timelineStart = playhead_;
    right.transitionIn = std::nullopt;

    // Capture for revert, then replace the original with the two halves.
    trackId_ = track->id;
    index_ = loc->index;
    original_ = original;

    track->clips[loc->index] = left;
    track->clips.insert(track->clips.begin() + static_cast<std::ptrdiff_t>(loc->index) + 1,
                        right);
    return ok();
}

Result<void> SplitClipCommand::revert(Project& project) {
    if (!original_) {
        return err(failedPrecondition("SplitClipCommand: revert before a successful apply"));
    }
    Track* track = findTrack(project, trackId_);
    if (track == nullptr) {
        return err(notFound("SplitClipCommand: track " + idLabel(trackId_) + " not found"));
    }
    if (index_ + 1 >= track->clips.size()) {
        return err(failedPrecondition("SplitClipCommand: split halves are no longer present"));
    }
    auto first = track->clips.begin() + static_cast<std::ptrdiff_t>(index_);
    track->clips.erase(first, first + 2);
    track->clips.insert(track->clips.begin() + static_cast<std::ptrdiff_t>(index_),
                        *original_);
    return ok();
}

// ===========================================================================
// ReorderClipsCommand
// ===========================================================================

ReorderClipsCommand::ReorderClipsCommand(Uuid trackId, std::vector<ClipId> newOrder)
    : trackId_(trackId), newOrder_(std::move(newOrder)) {}

Result<void> ReorderClipsCommand::apply(Project& project) {
    Track* track = findTrack(project, trackId_);
    if (track == nullptr) {
        return err(notFound("ReorderClipsCommand: track " + idLabel(trackId_) + " not found"));
    }

    // newOrder must be a permutation of the track's current clip ids.
    if (newOrder_.size() != track->clips.size()) {
        return err(invalidArgument(
            "ReorderClipsCommand: new order size does not match the track's clip count"));
    }
    std::unordered_map<ClipId, std::size_t> present;
    present.reserve(track->clips.size());
    for (const Clip& clip : track->clips) {
        ++present[clip.id];
    }
    for (const ClipId& id : newOrder_) {
        auto it = present.find(id);
        if (it == present.end() || it->second == 0) {
            return err(invalidArgument(
                "ReorderClipsCommand: new order is not a permutation of the track's clips"));
        }
        --it->second;
    }

    priorClips_ = track->clips;  // capture for an exact revert
    captured_ = true;

    // Base position: the earliest current clip start, so the reordered block
    // occupies the same starting point on the timeline.
    Duration base = Duration::zero();
    if (!track->clips.empty()) {
        base = track->clips.front().timelineStart;
        for (const Clip& clip : track->clips) {
            if (clip.timelineStart < base) {
                base = clip.timelineStart;
            }
        }
    }

    // Rebuild in the requested order, repacking contiguously (each clip begins
    // where the previous one ends) so the result stays ordered and non-overlapping
    // while preserving the clip count (Requirement 2.7).
    std::vector<Clip> reordered;
    reordered.reserve(newOrder_.size());
    Duration cursor = base;
    for (const ClipId& id : newOrder_) {
        auto it = std::find_if(priorClips_.begin(), priorClips_.end(),
                               [&](const Clip& c) { return c.id == id; });
        Clip clip = *it;
        clip.timelineStart = cursor;
        cursor += clip.duration();
        reordered.push_back(std::move(clip));
    }
    track->clips = std::move(reordered);
    return ok();
}

Result<void> ReorderClipsCommand::revert(Project& project) {
    if (!captured_) {
        return err(failedPrecondition("ReorderClipsCommand: revert before a successful apply"));
    }
    Track* track = findTrack(project, trackId_);
    if (track == nullptr) {
        return err(notFound("ReorderClipsCommand: track " + idLabel(trackId_) + " not found"));
    }
    track->clips = priorClips_;
    return ok();
}

// ===========================================================================
// AddEffectCommand
// ===========================================================================

AddEffectCommand::AddEffectCommand(ClipId clipId, Effect effect)
    : clipId_(clipId), effect_(std::move(effect)) {}

Result<void> AddEffectCommand::apply(Project& project) {
    std::optional<ClipLocation> loc = findClip(project, clipId_);
    if (!loc) {
        return err(notFound("AddEffectCommand: clip " + idLabel(clipId_) + " not found"));
    }
    loc->track->clips[loc->index].effects.push_back(effect_);
    return ok();
}

Result<void> AddEffectCommand::revert(Project& project) {
    std::optional<ClipLocation> loc = findClip(project, clipId_);
    if (!loc) {
        return err(notFound("AddEffectCommand: clip " + idLabel(clipId_) + " not found"));
    }
    auto& effects = loc->track->clips[loc->index].effects;
    effects.erase(std::remove_if(effects.begin(), effects.end(),
                                 [&](const Effect& e) { return e.id == effect_.id; }),
                  effects.end());
    return ok();
}

// ===========================================================================
// AddTrackCommand
// ===========================================================================

AddTrackCommand::AddTrackCommand(TrackKind kind)
    : kind_(kind), trackId_(Uuid::generateV4()) {}

AddTrackCommand::AddTrackCommand(TrackKind kind, Uuid trackId)
    : kind_(kind), trackId_(trackId) {}

Result<void> AddTrackCommand::apply(Project& project) {
    if (trackId_.isNil()) {
        return err(invalidArgument("AddTrackCommand: track id must not be nil"));
    }
    if (findTrack(project, trackId_) != nullptr) {
        // Requirement 3.3: the returned identifier is unique within the project.
        return err(failedPrecondition("AddTrackCommand: track " + idLabel(trackId_) +
                                      " is already present in the project"));
    }

    // Count the tracks of this kind and remember the position just past the last
    // one so the new lane is appended at that kind's tail (Requirement 3.3).
    std::size_t ofKind = 0;
    std::size_t insertAt = project.tracks.size();
    for (std::size_t i = 0; i < project.tracks.size(); ++i) {
        if (project.tracks[i].kind == kind_) {
            ++ofKind;
            insertAt = i + 1;
        }
    }

    // Requirement 3.8: a request that would exceed the per-kind cap is rejected
    // and names the offending argument.
    if (ofKind >= kMaxTracksPerKind) {
        return err(outOfRange(
            std::string("AddTrackCommand: 'kind' ") + std::string(trackKindLabel(kind_)) +
            " already holds " + std::to_string(ofKind) + " tracks; at most " +
            std::to_string(kMaxTracksPerKind) + " tracks of one kind are allowed"));
    }

    Track track;
    track.id = trackId_;
    track.kind = kind_;
    project.tracks.insert(project.tracks.begin() + static_cast<std::ptrdiff_t>(insertAt),
                          std::move(track));
    index_ = insertAt;
    return ok();
}

Result<void> AddTrackCommand::revert(Project& project) {
    if (!index_) {
        return err(failedPrecondition("AddTrackCommand: revert before a successful apply"));
    }
    auto it = std::find_if(project.tracks.begin(), project.tracks.end(),
                           [&](const Track& t) { return t.id == trackId_; });
    if (it == project.tracks.end()) {
        return err(notFound("AddTrackCommand: track " + idLabel(trackId_) + " not found"));
    }
    project.tracks.erase(it);
    return ok();
}

// ===========================================================================
// RemoveTrackCommand
// ===========================================================================

RemoveTrackCommand::RemoveTrackCommand(Uuid trackId) : trackId_(trackId) {}

std::optional<std::size_t> RemoveTrackCommand::removedClipCount() const noexcept {
    if (!removed_) {
        return std::nullopt;
    }
    return removed_->clips.size();
}

Result<void> RemoveTrackCommand::apply(Project& project) {
    auto it = std::find_if(project.tracks.begin(), project.tracks.end(),
                           [&](const Track& t) { return t.id == trackId_; });
    if (it == project.tracks.end()) {
        // Requirement 3.8: an unknown track identifier changes nothing.
        return err(notFound("RemoveTrackCommand: track " + idLabel(trackId_) +
                            " not found in the current project"));
    }

    index_ = static_cast<std::size_t>(std::distance(project.tracks.begin(), it));
    removed_ = *it;                 // the track and every clip on it
    project.tracks.erase(it);       // erase preserves the order of the rest
    return ok();
}

Result<void> RemoveTrackCommand::revert(Project& project) {
    if (!removed_) {
        return err(failedPrecondition("RemoveTrackCommand: revert before a successful apply"));
    }
    const std::size_t index = std::min(index_, project.tracks.size());
    project.tracks.insert(project.tracks.begin() + static_cast<std::ptrdiff_t>(index),
                          *removed_);
    return ok();
}

// ===========================================================================
// SetTransitionCommand
// ===========================================================================

SetTransitionCommand::SetTransitionCommand(ClipId clipId, Transition transition)
    : clipId_(clipId), transition_(std::move(transition)) {}

Result<void> SetTransitionCommand::apply(Project& project) {
    std::optional<ClipLocation> loc = findClip(project, clipId_);
    if (!loc) {
        return err(notFound("AddTransition: clip " + idLabel(clipId_) + " not found"));
    }
    Clip& clip = loc->track->clips[loc->index];
    prior_ = clip.transitionIn;
    captured_ = true;
    clip.transitionIn = transition_;
    return ok();
}

Result<void> SetTransitionCommand::revert(Project& project) {
    if (!captured_) {
        return err(failedPrecondition("AddTransition: revert before a successful apply"));
    }
    std::optional<ClipLocation> loc = findClip(project, clipId_);
    if (!loc) {
        return err(notFound("AddTransition: clip " + idLabel(clipId_) + " not found"));
    }
    loc->track->clips[loc->index].transitionIn = prior_;
    return ok();
}

}  // namespace palmier
