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
#include <map>
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
    switch (kind) {
        case TrackKind::Audio:   return "audio";
        case TrackKind::Text:    return "text";
        case TrackKind::Caption: return "caption";
        case TrackKind::Video:   return "video";
    }
    return "video";
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

    // Register the clip's asset in the project asset table if it is not already
    // resolvable there, so the clip's assetRef resolves (project validation rule).
    // Project.assets is the ONLY asset table a saved document carries, so an asset
    // known merely to a session-level MediaManager (as an imported asset is) would
    // otherwise produce a document that validateProject rejects on load. Doing it
    // here makes that impossible for every caller that places a clip, not just for
    // one import path. Resolution is by assetId, so a ref already in the table adds
    // nothing; the nil identity is never added, because it cannot be catalogued
    // when the library is rebuilt from the document.
    assetAdded_ = false;
    if (clip_.assetRef.isValid() &&
        std::none_of(project.assets.begin(), project.assets.end(),
                     [this](const MediaAssetRef& a) { return a == clip_.assetRef; })) {
        project.assets.push_back(clip_.assetRef);
        assetAdded_ = true;
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

    // Undo the asset registration too, and ONLY when this command performed it, so
    // revert() is an exact inverse and an asset another clip still references (or
    // one the table already carried) is never removed.
    if (assetAdded_) {
        auto& assets = project.assets;
        assets.erase(std::remove_if(assets.begin(), assets.end(),
                                    [this](const MediaAssetRef& a) {
                                        return a == clip_.assetRef;
                                    }),
                     assets.end());
        assetAdded_ = false;
    }
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
// SetTrackMutedCommand
// ===========================================================================

SetTrackMutedCommand::SetTrackMutedCommand(Uuid trackId, bool muted)
    : trackId_(trackId), muted_(muted) {}

Result<void> SetTrackMutedCommand::apply(Project& project) {
    auto it = std::find_if(project.tracks.begin(), project.tracks.end(),
                           [&](const Track& t) { return t.id == trackId_; });
    if (it == project.tracks.end()) {
        // Same rule as every other track-addressed edit (Requirement 3.8): an
        // unknown identifier changes nothing.
        return err(notFound("SetTrackMutedCommand: track " + idLabel(trackId_) +
                            " not found in the current project"));
    }
    prior_ = it->muted;
    it->muted = muted_;
    return ok();
}

Result<void> SetTrackMutedCommand::revert(Project& project) {
    if (!prior_) {
        return err(failedPrecondition(
            "SetTrackMutedCommand: revert before a successful apply"));
    }
    auto it = std::find_if(project.tracks.begin(), project.tracks.end(),
                           [&](const Track& t) { return t.id == trackId_; });
    if (it == project.tracks.end()) {
        return err(notFound("SetTrackMutedCommand: track " + idLabel(trackId_) +
                            " not found in the current project"));
    }
    it->muted = *prior_;
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

// ===========================================================================
// RippleDeleteCommand
// ===========================================================================

RippleDeleteCommand::RippleDeleteCommand(ClipId clipId) : clipId_(clipId) {}

Result<void> RippleDeleteCommand::apply(Project& project) {
    std::optional<ClipLocation> loc = findClip(project, clipId_);
    if (!loc) {
        return err(notFound("RippleDeleteCommand: clip " + idLabel(clipId_) + " not found"));
    }

    Track* track = loc->track;
    trackId_ = track->id;
    priorClips_ = track->clips;  // capture for an exact revert / rollback
    captured_ = true;

    // The gap to close is exactly the removed clip's duration (Requirement 5.1).
    const Duration removedDuration = track->clips[loc->index].duration();

    track->clips.erase(track->clips.begin() + static_cast<std::ptrdiff_t>(loc->index));

    // Every clip that followed it moves earlier by that duration. Erasing shifted
    // the later clips down by one index, so the survivors from loc->index onward
    // are precisely the ones that were later.
    for (std::size_t i = loc->index; i < track->clips.size(); ++i) {
        track->clips[i].timelineStart = track->clips[i].timelineStart - removedDuration;
        if (track->clips[i].timelineStart.isNegative()) {
            // A negative position is outside the valid range; the track's ordering
            // makes this unreachable for a well-formed project, so treat it as a
            // rejected edit rather than clamping into a silently wrong result. The
            // label is taken before the rollback, which resizes the vector.
            const std::string offender = idLabel(track->clips[i].id);
            track->clips = priorClips_;
            return err(failedPrecondition(
                "RippleDeleteCommand: closing the gap would move clip " + offender +
                " before the start of the timeline"));
        }
    }

    if (!trackOrderedAndNonOverlapping(*track)) {
        track->clips = priorClips_;
        return err(failedPrecondition(
            "RippleDeleteCommand: closing the gap would overlap a clip on the track; "
            "ripple delete rejected"));
    }
    return ok();
}

Result<void> RippleDeleteCommand::revert(Project& project) {
    if (!captured_) {
        return err(failedPrecondition("RippleDeleteCommand: revert before a successful apply"));
    }
    Track* track = findTrack(project, trackId_);
    if (track == nullptr) {
        return err(notFound("RippleDeleteCommand: track " + idLabel(trackId_) + " not found"));
    }
    track->clips = priorClips_;
    return ok();
}

// ===========================================================================
// RippleTrimCommand
// ===========================================================================

RippleTrimCommand::RippleTrimCommand(ClipId clipId, Edge edge, Duration newBoundary,
                                     FrameRate fps, Duration sourceDuration)
    : clipId_(clipId),
      edge_(edge),
      newBoundary_(newBoundary),
      fps_(fps),
      sourceDuration_(sourceDuration) {}

Result<void> RippleTrimCommand::apply(Project& project) {
    std::optional<ClipLocation> primary = findClip(project, clipId_);
    if (!primary) {
        return err(notFound("RippleTrimCommand: clip " + idLabel(clipId_) + " not found"));
    }

    // The one-frame minimum, exactly as TrimClipCommand computes it, so the two
    // commands agree on which edges are legal.
    Duration minDuration = fps_.isValid() ? fps_.frameDuration() : Duration::fromNanoseconds(1);
    if (!minDuration.isPositive()) {
        minDuration = Duration::fromNanoseconds(1);
    }
    if (sourceDuration_ < minDuration) {
        return err(failedPrecondition(
            "RippleTrimCommand: source duration is shorter than the one-frame minimum"));
    }

    // A group edit spans tracks, so the whole track list is the unit of capture.
    priorTracks_ = project.tracks;
    captured_ = true;

    const auto rollback = [&](Error error) -> Result<void> {
        project.tracks = priorTracks_;
        return err(std::move(error));
    };

    // The source-time delta the named clip is trimmed by, after clamping. Every
    // group member is then trimmed by this same delta.
    Duration delta = Duration::zero();
    {
        Clip& clip = primary->track->clips[primary->index];
        if (edge_ == Edge::End) {
            const Duration lo = clip.sourceIn + minDuration;
            if (lo > sourceDuration_) {
                return rollback(failedPrecondition(
                    "RippleTrimCommand: cannot keep a one-frame minimum within the source"));
            }
            const Duration newOut = clampDuration(newBoundary_, lo, sourceDuration_);
            delta = newOut - clip.sourceOut;
        } else {
            const Duration hi = clip.sourceOut - minDuration;
            if (hi.isNegative()) {
                return rollback(failedPrecondition(
                    "RippleTrimCommand: cannot keep a one-frame minimum within the source"));
            }
            const Duration newIn = clampDuration(newBoundary_, Duration::zero(), hi);
            delta = newIn - clip.sourceIn;
        }
    }

    // The clips this edit trims: the named one, plus every other member of the
    // clipGroup that names it. Groups are the multicam sync mechanism (PR 397);
    // a clip in no group trims alone.
    std::vector<ClipId> targets{clipId_};
    for (const ClipGroup& group : project.clipGroups) {
        const bool namesClip =
            std::find(group.clipIds.begin(), group.clipIds.end(), clipId_) != group.clipIds.end();
        if (!namesClip) {
            continue;
        }
        for (const ClipId& member : group.clipIds) {
            if (member == clipId_) {
                continue;
            }
            if (std::find(targets.begin(), targets.end(), member) == targets.end()) {
                targets.push_back(member);
            }
        }
    }

    for (const ClipId& target : targets) {
        std::optional<ClipLocation> loc = findClip(project, target);
        if (!loc) {
            // A group naming a clip the project does not carry is a document
            // defect; refuse rather than silently trimming only part of the group.
            return rollback(notFound("RippleTrimCommand: grouped clip " + idLabel(target) +
                                     " named by a clip group is not in the project"));
        }

        Track*            track = loc->track;
        const std::size_t index = loc->index;
        Clip&             clip = track->clips[index];

        // Apply the same source-time delta to this member, refusing when the
        // member cannot accommodate it while keeping a frame inside its source.
        if (edge_ == Edge::End) {
            const Duration newOut = clip.sourceOut + delta;
            if (newOut < clip.sourceIn + minDuration || newOut > sourceDuration_) {
                return rollback(failedPrecondition(
                    "RippleTrimCommand: grouped clip " + idLabel(target) +
                    " cannot be trimmed by the same amount within its source"));
            }
            clip.sourceOut = newOut;
        } else {
            const Duration newIn = clip.sourceIn + delta;
            if (newIn.isNegative() || newIn > clip.sourceOut - minDuration) {
                return rollback(failedPrecondition(
                    "RippleTrimCommand: grouped clip " + idLabel(target) +
                    " cannot be trimmed by the same amount within its source"));
            }
            const Duration newStart = clip.timelineStart + delta;
            if (newStart.isNegative()) {
                return rollback(failedPrecondition(
                    "RippleTrimCommand: trimming clip " + idLabel(target) +
                    " would move it before the start of the timeline"));
            }
            clip.sourceIn = newIn;
            clip.timelineStart = newStart;
        }

        // Requirement 5.2: every later clip on the same track shifts by exactly the
        // change in duration. For Edge::Start the leading edge moved by `delta` and
        // the duration changed by `-delta`; for Edge::End the duration changed by
        // `+delta` and the leading edge did not move. In both cases the trailing
        // edge moved by the same amount the followers must move: `delta` for End,
        // and nothing for Start, whose trailing edge is fixed.
        const Duration followerShift = edge_ == Edge::End ? delta : Duration::zero();
        if (!followerShift.isZero()) {
            for (std::size_t i = index + 1; i < track->clips.size(); ++i) {
                const Duration shifted = track->clips[i].timelineStart + followerShift;
                if (shifted.isNegative()) {
                    return rollback(failedPrecondition(
                        "RippleTrimCommand: rippling would move clip " +
                        idLabel(track->clips[i].id) + " before the start of the timeline"));
                }
                track->clips[i].timelineStart = shifted;
            }
        }

        if (!trackOrderedAndNonOverlapping(*track)) {
            return rollback(failedPrecondition(
                "RippleTrimCommand: the trim would overlap a clip on track " +
                idLabel(track->id) + "; ripple trim rejected"));
        }
    }

    appliedDelta_ = delta;
    return ok();
}

Result<void> RippleTrimCommand::revert(Project& project) {
    if (!captured_) {
        return err(failedPrecondition("RippleTrimCommand: revert before a successful apply"));
    }
    project.tracks = priorTracks_;
    return ok();
}

// ===========================================================================
// CloseGapCommand
// ===========================================================================

CloseGapCommand::CloseGapCommand(ClipId clipId) : clipId_(clipId) {}

Result<void> CloseGapCommand::apply(Project& project) {
    std::optional<ClipLocation> loc = findClip(project, clipId_);
    if (!loc) {
        return err(notFound("CloseGapCommand: clip " + idLabel(clipId_) + " not found"));
    }

    Track*            track = loc->track;
    const std::size_t index = loc->index;
    if (index + 1 >= track->clips.size()) {
        return err(failedPrecondition("CloseGapCommand: clip " + idLabel(clipId_) +
                                      " is the last clip on its track, so no gap follows it"));
    }

    const Duration gap = track->clips[index + 1].timelineStart - track->clips[index].timelineEnd();
    if (!gap.isPositive()) {
        return err(failedPrecondition("CloseGapCommand: no gap follows clip " + idLabel(clipId_)));
    }

    trackId_ = track->id;
    priorClips_ = track->clips;  // capture for an exact revert / rollback
    captured_ = true;

    // Only positions change: every later clip moves earlier by the gap, and no
    // duration or source range is touched (Requirement 5.5).
    for (std::size_t i = index + 1; i < track->clips.size(); ++i) {
        const Duration shifted = track->clips[i].timelineStart - gap;
        if (shifted.isNegative()) {
            const std::string offender = idLabel(track->clips[i].id);
            track->clips = priorClips_;
            return err(failedPrecondition(
                "CloseGapCommand: closing the gap would move clip " + offender +
                " before the start of the timeline"));
        }
        track->clips[i].timelineStart = shifted;
    }

    if (!trackOrderedAndNonOverlapping(*track)) {
        track->clips = priorClips_;
        return err(failedPrecondition(
            "CloseGapCommand: closing the gap would overlap a clip on the track; "
            "close gap rejected"));
    }

    closedGap_ = gap;
    return ok();
}

Result<void> CloseGapCommand::revert(Project& project) {
    if (!captured_) {
        return err(failedPrecondition("CloseGapCommand: revert before a successful apply"));
    }
    Track* track = findTrack(project, trackId_);
    if (track == nullptr) {
        return err(notFound("CloseGapCommand: track " + idLabel(trackId_) + " not found"));
    }
    track->clips = priorClips_;
    return ok();
}

// ===========================================================================
// RemoveEffectCommand
// ===========================================================================

RemoveEffectCommand::RemoveEffectCommand(ClipId clipId, Uuid effectId)
    : clipId_(clipId), effectId_(effectId) {}

Result<void> RemoveEffectCommand::apply(Project& project) {
    std::optional<ClipLocation> loc = findClip(project, clipId_);
    if (!loc) {
        return err(notFound("RemoveEffectCommand: clip " + idLabel(clipId_) + " not found"));
    }
    std::vector<Effect>& effects = loc->track->clips[loc->index].effects;
    auto it = std::find_if(effects.begin(), effects.end(),
                           [&](const Effect& e) { return e.id == effectId_; });
    if (it == effects.end()) {
        return err(notFound("RemoveEffectCommand: effect " + idLabel(effectId_) +
                            " not found on clip " + idLabel(clipId_)));
    }
    index_ = static_cast<std::size_t>(std::distance(effects.begin(), it));
    removed_ = *it;
    effects.erase(it);
    return ok();
}

Result<void> RemoveEffectCommand::revert(Project& project) {
    if (!removed_) {
        return err(failedPrecondition("RemoveEffectCommand: revert before a successful apply"));
    }
    std::optional<ClipLocation> loc = findClip(project, clipId_);
    if (!loc) {
        return err(notFound("RemoveEffectCommand: clip " + idLabel(clipId_) + " not found"));
    }
    std::vector<Effect>& effects = loc->track->clips[loc->index].effects;
    const std::size_t index = std::min(index_, effects.size());
    effects.insert(effects.begin() + static_cast<std::ptrdiff_t>(index), *removed_);
    return ok();
}

// ===========================================================================
// ReorderEffectsCommand
// ===========================================================================

ReorderEffectsCommand::ReorderEffectsCommand(ClipId clipId, std::vector<Uuid> newOrder)
    : clipId_(clipId), newOrder_(std::move(newOrder)) {}

Result<void> ReorderEffectsCommand::apply(Project& project) {
    std::optional<ClipLocation> loc = findClip(project, clipId_);
    if (!loc) {
        return err(notFound("ReorderEffectsCommand: clip " + idLabel(clipId_) + " not found"));
    }
    std::vector<Effect>& effects = loc->track->clips[loc->index].effects;

    // newOrder_ must be a permutation of the clip's current effect ids.
    if (newOrder_.size() != effects.size()) {
        return err(invalidArgument(
            "ReorderEffectsCommand: new order size does not match the clip's effect count"));
    }
    std::unordered_map<Uuid, std::size_t> present;
    present.reserve(effects.size());
    for (const Effect& effect : effects) {
        ++present[effect.id];
    }
    for (const Uuid& id : newOrder_) {
        auto it = present.find(id);
        if (it == present.end() || it->second == 0) {
            return err(invalidArgument(
                "ReorderEffectsCommand: new order is not a permutation of the clip's effects"));
        }
        --it->second;
    }

    priorEffects_ = effects;  // capture for an exact revert
    captured_ = true;

    // A pure permutation: no field of an Effect depends on its position in the
    // chain, so the new order is simply the requested ids looked up in the prior
    // list (Requirement 6.4 — rendering, not this command, is what reads the
    // resulting order and changes output accordingly).
    std::vector<Effect> reordered;
    reordered.reserve(newOrder_.size());
    for (const Uuid& id : newOrder_) {
        auto it = std::find_if(priorEffects_.begin(), priorEffects_.end(),
                               [&](const Effect& e) { return e.id == id; });
        reordered.push_back(*it);
    }
    effects = std::move(reordered);
    return ok();
}

Result<void> ReorderEffectsCommand::revert(Project& project) {
    if (!captured_) {
        return err(failedPrecondition("ReorderEffectsCommand: revert before a successful apply"));
    }
    std::optional<ClipLocation> loc = findClip(project, clipId_);
    if (!loc) {
        return err(notFound("ReorderEffectsCommand: clip " + idLabel(clipId_) + " not found"));
    }
    loc->track->clips[loc->index].effects = priorEffects_;
    return ok();
}

// ===========================================================================
// SetEffectParameterCommand
// ===========================================================================

SetEffectParameterCommand::SetEffectParameterCommand(ClipId clipId, Uuid effectId,
                                                     std::string parameter, double value)
    : clipId_(clipId),
      effectId_(effectId),
      parameter_(std::move(parameter)),
      value_(value) {}

Result<void> SetEffectParameterCommand::apply(Project& project) {
    std::optional<ClipLocation> loc = findClip(project, clipId_);
    if (!loc) {
        return err(notFound("SetEffectParameterCommand: clip " + idLabel(clipId_) + " not found"));
    }
    std::vector<Effect>& effects = loc->track->clips[loc->index].effects;
    auto it = std::find_if(effects.begin(), effects.end(),
                           [&](const Effect& e) { return e.id == effectId_; });
    if (it == effects.end()) {
        return err(notFound("SetEffectParameterCommand: effect " + idLabel(effectId_) +
                            " not found on clip " + idLabel(clipId_)));
    }

    // Capture the prior value (or its absence) so revert() is an exact inverse.
    auto param = it->parameters.find(parameter_);
    hadPrior_ = param != it->parameters.end();
    prior_ = hadPrior_ ? param->second : 0.0;
    captured_ = true;

    it->parameters[parameter_] = value_;
    return ok();
}

Result<void> SetEffectParameterCommand::revert(Project& project) {
    if (!captured_) {
        return err(failedPrecondition("SetEffectParameterCommand: revert before a successful apply"));
    }
    std::optional<ClipLocation> loc = findClip(project, clipId_);
    if (!loc) {
        return err(notFound("SetEffectParameterCommand: clip " + idLabel(clipId_) + " not found"));
    }
    std::vector<Effect>& effects = loc->track->clips[loc->index].effects;
    auto it = std::find_if(effects.begin(), effects.end(),
                           [&](const Effect& e) { return e.id == effectId_; });
    if (it == effects.end()) {
        return err(notFound("SetEffectParameterCommand: effect " + idLabel(effectId_) +
                            " not found on clip " + idLabel(clipId_)));
    }
    if (hadPrior_) {
        it->parameters[parameter_] = prior_;
    } else {
        it->parameters.erase(parameter_);
    }
    return ok();
}

// ===========================================================================
// EditCurvePointCommand
// ===========================================================================

EditCurvePointCommand::EditCurvePointCommand(ClipId clipId, Uuid effectId,
                                             CurveChannel channel, Operation operation,
                                             std::size_t index, double x, double y)
    : clipId_(clipId),
      effectId_(effectId),
      channel_(channel),
      operation_(operation),
      index_(index),
      x_(x),
      y_(y) {}

namespace {

/// Locate one effect, or say which of the two things was missing.
Result<std::vector<Effect>::iterator> findEffect(Project& project, ClipId clipId,
                                                const Uuid& effectId, const char* who) {
    std::optional<ClipLocation> loc = findClip(project, clipId);
    if (!loc) {
        return err<std::vector<Effect>::iterator>(
            notFound(std::string(who) + ": clip " + idLabel(clipId) + " not found"));
    }
    std::vector<Effect>& effects = loc->track->clips[loc->index].effects;
    auto it = std::find_if(effects.begin(), effects.end(),
                           [&](const Effect& e) { return e.id == effectId; });
    if (it == effects.end()) {
        return err<std::vector<Effect>::iterator>(
            notFound(std::string(who) + ": effect " + idLabel(effectId) + " not found on clip " +
                     idLabel(clipId)));
    }
    return ok(it);
}

/// Every parameter of one channel's points, keyed by full parameter name.
///
/// Selected by name prefix rather than by walking indices, because the whole point of
/// the capture is to restore state that may not be contiguous — a set walked with
/// curvePoints() would stop at the first gap and quietly fail to restore anything
/// beyond it.
std::map<std::string, double> channelParameters(const std::map<std::string, double>& all,
                                                CurveChannel channel) {
    const std::string prefix = curveChannelParameterPrefix(channel);
    std::map<std::string, double> mine;
    for (const auto& [key, value] : all) {
        if (key.compare(0, prefix.size(), prefix) == 0) {
            mine.emplace(key, value);
        }
    }
    return mine;
}

}  // namespace

Result<void> EditCurvePointCommand::apply(Project& project) {
    auto found = findEffect(project, clipId_, effectId_, "EditCurvePointCommand");
    if (!found) {
        return err(found.error());
    }
    auto it = found.value();

    // The points as they stand, and the whole channel as it stands. The points drive
    // validation; the parameter set is the inverse.
    std::vector<CurvePoint> points = curvePoints(it->parameters, channel_);
    prior_ = channelParameters(it->parameters, channel_);

    if (operation_ != Operation::Add && index_ >= points.size()) {
        return err(notFound("EditCurvePointCommand: point " + std::to_string(index_) +
                            " does not exist on the " + curveChannelName(channel_) +
                            " curve, which has " + std::to_string(points.size()) + " point(s)"));
    }

    switch (operation_) {
        case Operation::Add:
            points.push_back(CurvePoint{x_, y_});
            break;
        case Operation::Move:
            points[index_] = CurvePoint{x_, y_};
            break;
        case Operation::Remove:
            points.erase(points.begin() + static_cast<std::ptrdiff_t>(index_));
            break;
    }

    // Rewrite the channel from scratch. Erasing first is what keeps the indices
    // contiguous after a removal: writing the shortened list over the old one would
    // leave the final point's coordinates behind as a duplicate at the end.
    for (const auto& [key, value] : prior_) {
        (void)value;
        it->parameters.erase(key);
    }
    for (std::size_t i = 0; i < points.size(); ++i) {
        it->parameters[curvePointParameterName(channel_, i, /*isY=*/false)] = points[i].x;
        it->parameters[curvePointParameterName(channel_, i, /*isY=*/true)] = points[i].y;
    }

    captured_ = true;
    return ok();
}

Result<void> EditCurvePointCommand::revert(Project& project) {
    if (!captured_) {
        return err(failedPrecondition("EditCurvePointCommand: revert before a successful apply"));
    }
    auto found = findEffect(project, clipId_, effectId_, "EditCurvePointCommand");
    if (!found) {
        return err(found.error());
    }
    auto it = found.value();

    // Remove whatever this channel holds now, then put back exactly what it held
    // before. Both halves are needed: without the erase, undoing an Add would leave
    // the added point in place; without the restore, undoing a Move or Remove would
    // leave the channel empty.
    const std::map<std::string, double> current = channelParameters(it->parameters, channel_);
    for (const auto& [key, value] : current) {
        (void)value;
        it->parameters.erase(key);
    }
    for (const auto& [key, value] : prior_) {
        it->parameters[key] = value;
    }
    return ok();
}

// ===========================================================================
// SetEffectResourceCommand
// ===========================================================================

SetEffectResourceCommand::SetEffectResourceCommand(ClipId clipId, Uuid effectId,
                                                   std::string path)
    : clipId_(clipId), effectId_(effectId), path_(std::move(path)) {}

Result<void> SetEffectResourceCommand::apply(Project& project) {
    auto found = findEffect(project, clipId_, effectId_, "SetEffectResourceCommand");
    if (!found) {
        return err(found.error());
    }
    auto it = found.value();
    prior_ = it->resourcePath;
    captured_ = true;
    it->resourcePath = path_;
    // Deliberately no existence check. Requirement 7.8 requires a missing LUT to leave the
    // effect in the chain and render un-graded, so refusing an unreadable path here would
    // make a project that opens on one machine un-editable on another.
    return ok();
}

Result<void> SetEffectResourceCommand::revert(Project& project) {
    if (!captured_) {
        return err(failedPrecondition("SetEffectResourceCommand: revert before a successful apply"));
    }
    auto found = findEffect(project, clipId_, effectId_, "SetEffectResourceCommand");
    if (!found) {
        return err(found.error());
    }
    // Restores an EMPTY prior too, which is the common case: an effect just added has no
    // resource, so skipping the restore when the prior was empty would leave the first LUT
    // applied to an effect un-undoable.
    found.value()->resourcePath = prior_;
    return ok();
}

const char* curvePointOperationToolName(EditCurvePointCommand::Operation operation) noexcept {
    switch (operation) {
        case EditCurvePointCommand::Operation::Add:    return "add";
        case EditCurvePointCommand::Operation::Move:   return "move";
        case EditCurvePointCommand::Operation::Remove: return "remove";
    }
    return "add";
}

std::optional<EditCurvePointCommand::Operation> parseCurvePointOperationToolName(
    std::string_view name) {
    // Derived from the name function, so the two directions cannot disagree.
    for (const auto operation : {EditCurvePointCommand::Operation::Add,
                                 EditCurvePointCommand::Operation::Move,
                                 EditCurvePointCommand::Operation::Remove}) {
        if (name == curvePointOperationToolName(operation)) {
            return operation;
        }
    }
    return std::nullopt;
}

const std::vector<std::string>& curvePointOperationToolNames() {
    static const std::vector<std::string> names = {
        curvePointOperationToolName(EditCurvePointCommand::Operation::Add),
        curvePointOperationToolName(EditCurvePointCommand::Operation::Move),
        curvePointOperationToolName(EditCurvePointCommand::Operation::Remove)};
    return names;
}

// ===========================================================================
// SetProjectSettingsCommand
// ===========================================================================

SetProjectSettingsCommand::SetProjectSettingsCommand(std::optional<FrameRate> fps,
                                                     std::optional<Resolution> canvas,
                                                     std::optional<ColorSpace> colorSpace)
    : fps_(fps), canvas_(canvas), colorSpace_(colorSpace) {}

Result<void> SetProjectSettingsCommand::apply(Project& project) {
    // core has no dependency on services::, so the declared numeric ranges
    // (Requirement 7.1's "same ranges project.create accepts") are the
    // Tool_Surface's to enforce; this only rejects a value that is not even
    // internally well-formed, matching every other command's own-invariant check.
    if (fps_ && !fps_->isValid()) {
        return err(invalidArgument("SetProjectSettingsCommand: fps is not a valid frame rate"));
    }
    if (canvas_ && !canvas_->isValid()) {
        return err(invalidArgument("SetProjectSettingsCommand: canvas is not a valid resolution"));
    }

    priorFps_ = project.timelineFps;
    priorCanvas_ = project.canvas;
    priorColorSpace_ = project.colorSpace;
    captured_ = true;

    if (fps_) project.timelineFps = *fps_;
    if (canvas_) project.canvas = *canvas_;
    if (colorSpace_) project.colorSpace = *colorSpace_;
    return ok();
}

Result<void> SetProjectSettingsCommand::revert(Project& project) {
    if (!captured_) {
        return err(failedPrecondition("SetProjectSettingsCommand: revert before a successful apply"));
    }
    project.timelineFps = priorFps_;
    project.canvas = priorCanvas_;
    project.colorSpace = priorColorSpace_;
    return ok();
}

// ===========================================================================
// Text and titles (usable-editor task 12; Requirement 9)
// ===========================================================================

SetTextContentCommand::SetTextContentCommand(ClipId clipId, std::string content)
    : clipId_(clipId), content_(std::move(content)) {}

Result<void> SetTextContentCommand::apply(Project& project) {
    std::optional<ClipLocation> loc = findClip(project, clipId_);
    if (!loc) {
        return err(notFound("SetTextContentCommand: clip " + idLabel(clipId_) + " not found"));
    }
    Clip& clip = loc->track->clips[loc->index];
    if (!clip.isTextClip()) {
        return err(failedPrecondition("SetTextContentCommand: clip " + idLabel(clipId_) +
                                      " is not a text clip"));
    }
    prior_ = clip.textStyle->content;
    captured_ = true;
    clip.textStyle->content = content_;
    return ok();
}

Result<void> SetTextContentCommand::revert(Project& project) {
    if (!captured_) {
        return err(failedPrecondition("SetTextContentCommand: revert before a successful apply"));
    }
    std::optional<ClipLocation> loc = findClip(project, clipId_);
    if (!loc) {
        return err(notFound("SetTextContentCommand: clip " + idLabel(clipId_) + " not found"));
    }
    Clip& clip = loc->track->clips[loc->index];
    if (!clip.isTextClip()) {
        return err(failedPrecondition("SetTextContentCommand: clip " + idLabel(clipId_) +
                                      " is not a text clip"));
    }
    clip.textStyle->content = prior_;
    return ok();
}

SetTextStyleCommand::SetTextStyleCommand(ClipId clipId, std::optional<std::string> fontFamily,
                                         std::optional<double> pointSize,
                                         std::optional<double> colorR, std::optional<double> colorG,
                                         std::optional<double> colorB, std::optional<double> colorA,
                                         std::optional<TextAlignment> alignment,
                                         std::optional<double> x, std::optional<double> y)
    : clipId_(clipId), fontFamily_(std::move(fontFamily)), pointSize_(pointSize),
      colorR_(colorR), colorG_(colorG), colorB_(colorB), colorA_(colorA),
      alignment_(alignment), x_(x), y_(y) {}

Result<void> SetTextStyleCommand::apply(Project& project) {
    std::optional<ClipLocation> loc = findClip(project, clipId_);
    if (!loc) {
        return err(notFound("SetTextStyleCommand: clip " + idLabel(clipId_) + " not found"));
    }
    Clip& clip = loc->track->clips[loc->index];
    if (!clip.isTextClip()) {
        return err(failedPrecondition("SetTextStyleCommand: clip " + idLabel(clipId_) +
                                      " is not a text clip"));
    }

    // Build the candidate style before mutating anything, so a resulting
    // invalid style is rejected with the project left exactly as it was
    // (mirroring SetProjectSettingsCommand's own-invariant check — the
    // declared numeric ranges, if any, beyond internal well-formedness are the
    // Tool_Surface's to enforce).
    TextStyle candidate = *clip.textStyle;
    if (fontFamily_) candidate.fontFamily = *fontFamily_;
    if (pointSize_) candidate.pointSize = *pointSize_;
    if (colorR_) candidate.colorR = *colorR_;
    if (colorG_) candidate.colorG = *colorG_;
    if (colorB_) candidate.colorB = *colorB_;
    if (colorA_) candidate.colorA = *colorA_;
    if (alignment_) candidate.alignment = *alignment_;
    if (x_) candidate.x = *x_;
    if (y_) candidate.y = *y_;

    if (!candidate.isValid()) {
        return err(invalidArgument("SetTextStyleCommand: the resulting style is not internally "
                                   "well-formed (point size must be > 0 and every colour/position "
                                   "value must lie within [0, 1])"));
    }

    prior_ = *clip.textStyle;
    captured_ = true;
    *clip.textStyle = candidate;
    return ok();
}

Result<void> SetTextStyleCommand::revert(Project& project) {
    if (!captured_) {
        return err(failedPrecondition("SetTextStyleCommand: revert before a successful apply"));
    }
    std::optional<ClipLocation> loc = findClip(project, clipId_);
    if (!loc) {
        return err(notFound("SetTextStyleCommand: clip " + idLabel(clipId_) + " not found"));
    }
    Clip& clip = loc->track->clips[loc->index];
    if (!clip.isTextClip()) {
        return err(failedPrecondition("SetTextStyleCommand: clip " + idLabel(clipId_) +
                                      " is not a text clip"));
    }
    *clip.textStyle = prior_;
    return ok();
}

// ===========================================================================
// Captions and transcription (usable-editor task 13; Requirement 10)
// ===========================================================================

SetCaptionTextCommand::SetCaptionTextCommand(ClipId clipId, std::string text)
    : clipId_(clipId), text_(std::move(text)) {}

Result<void> SetCaptionTextCommand::apply(Project& project) {
    std::optional<ClipLocation> loc = findClip(project, clipId_);
    if (!loc) {
        return err(notFound("SetCaptionTextCommand: clip " + idLabel(clipId_) + " not found"));
    }
    Clip& clip = loc->track->clips[loc->index];
    if (!clip.isCaptionCue()) {
        return err(failedPrecondition("SetCaptionTextCommand: clip " + idLabel(clipId_) +
                                      " is not a caption cue"));
    }
    if (text_.empty()) {
        return err(invalidArgument("SetCaptionTextCommand: captionText must not be empty"));
    }
    prior_ = *clip.captionText;
    captured_ = true;
    clip.captionText = text_;
    return ok();
}

Result<void> SetCaptionTextCommand::revert(Project& project) {
    if (!captured_) {
        return err(failedPrecondition("SetCaptionTextCommand: revert before a successful apply"));
    }
    std::optional<ClipLocation> loc = findClip(project, clipId_);
    if (!loc) {
        return err(notFound("SetCaptionTextCommand: clip " + idLabel(clipId_) + " not found"));
    }
    Clip& clip = loc->track->clips[loc->index];
    if (!clip.isCaptionCue()) {
        return err(failedPrecondition("SetCaptionTextCommand: clip " + idLabel(clipId_) +
                                      " is not a caption cue"));
    }
    clip.captionText = prior_;
    return ok();
}

RetimeCaptionCueCommand::RetimeCaptionCueCommand(ClipId clipId,
                                                 std::optional<Duration> newTimelineStart,
                                                 std::optional<Duration> newDuration)
    : clipId_(clipId), newTimelineStart_(newTimelineStart), newDuration_(newDuration) {}

Result<void> RetimeCaptionCueCommand::apply(Project& project) {
    std::optional<ClipLocation> loc = findClip(project, clipId_);
    if (!loc) {
        return err(notFound("RetimeCaptionCueCommand: clip " + idLabel(clipId_) + " not found"));
    }
    Clip& clip = loc->track->clips[loc->index];
    if (!clip.isCaptionCue()) {
        return err(failedPrecondition("RetimeCaptionCueCommand: clip " + idLabel(clipId_) +
                                      " is not a caption cue"));
    }

    const Duration candidateStart = newTimelineStart_.value_or(clip.timelineStart);
    const Duration candidateDuration = newDuration_.value_or(clip.duration());
    if (candidateStart.isNegative()) {
        return err(failedPrecondition(
            "RetimeCaptionCueCommand: destination position must be >= 0"));
    }
    if (!candidateDuration.isPositive()) {
        return err(failedPrecondition(
            "RetimeCaptionCueCommand: the resulting duration must be > 0"));
    }

    Track* track = loc->track;
    trackId_ = track->id;
    priorClips_ = track->clips;  // capture for an exact revert / rollback
    captured_ = true;

    Clip& target = track->clips[loc->index];
    target.timelineStart = candidateStart;
    target.sourceIn = Duration::zero();
    target.sourceOut = candidateDuration;
    std::stable_sort(track->clips.begin(), track->clips.end(),
                     [](const Clip& a, const Clip& b) {
                         return a.timelineStart < b.timelineStart;
                     });

    // Requirement 10.2 (mirrors MoveClipCommand's Requirement 2.3 rejection):
    // a retime that would overlap another cue on the same track is rejected
    // and the track restored to its prior contents.
    if (!trackOrderedAndNonOverlapping(*track)) {
        track->clips = priorClips_;
        return err(failedPrecondition(
            "RetimeCaptionCueCommand: the new timing overlaps another cue on the "
            "track; retime rejected"));
    }
    return ok();
}

Result<void> RetimeCaptionCueCommand::revert(Project& project) {
    if (!captured_) {
        return err(failedPrecondition(
            "RetimeCaptionCueCommand: revert before a successful apply"));
    }
    Track* track = findTrack(project, trackId_);
    if (track == nullptr) {
        return err(notFound("RetimeCaptionCueCommand: track " + idLabel(trackId_) +
                            " not found"));
    }
    track->clips = priorClips_;
    return ok();
}

}  // namespace palmier
