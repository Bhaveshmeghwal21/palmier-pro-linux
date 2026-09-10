// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/ClipEditCommands.cpp — the clip structural edits: add, delete, move, trim, split and reorder.
//
// Split out of core/EditCommands.cpp, which held all 24 command implementations in
// one 1583-line translation unit. Every definition below was moved rather than
// rewritten. The commands are declared in the public core/EditCommands.hpp, which
// is unchanged; the helpers they share are in core/EditCommandSupport.hpp.

#include "core/EditCommands.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "core/EditCommandSupport.hpp"
#include "core/Error.hpp"
#include "core/Project.hpp"
#include "core/Track.hpp"

namespace palmier {

using namespace palmier::detail;  // NOLINT(google-build-using-namespace)

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
    auto pos = std::find_if(track->clips.begin(), track->clips.end(), [&](const Clip& existing) {
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
    clips.erase(
        std::remove_if(clips.begin(), clips.end(), [&](const Clip& c) { return c.id == clip_.id; }),
        clips.end());

    // Undo the asset registration too, and ONLY when this command performed it, so
    // revert() is an exact inverse and an asset another clip still references (or
    // one the table already carried) is never removed.
    if (assetAdded_) {
        auto& assets = project.assets;
        assets.erase(std::remove_if(assets.begin(), assets.end(),
                                    [this](const MediaAssetRef& a) { return a == clip_.assetRef; }),
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
    loc->track->clips.erase(loc->track->clips.begin() + static_cast<std::ptrdiff_t>(loc->index));
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
    track->clips.insert(track->clips.begin() + static_cast<std::ptrdiff_t>(index), *removed_);
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
        return err(failedPrecondition("MoveClipCommand: destination position must be >= 0"));
    }

    Track* track = loc->track;
    trackId_ = track->id;
    priorClips_ = track->clips;  // capture for an exact revert / rollback
    captured_ = true;

    // Move the clip and restore the track's timelineStart ordering.
    track->clips[loc->index].timelineStart = newStart_;
    std::stable_sort(track->clips.begin(), track->clips.end(), [](const Clip& a, const Clip& b) {
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

TrimClipCommand::TrimClipCommand(ClipId clipId, Edge edge, Duration newBoundary, FrameRate fps,
                                 Duration sourceDuration)
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
    track->clips.insert(track->clips.begin() + static_cast<std::ptrdiff_t>(loc->index) + 1, right);
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
    track->clips.insert(track->clips.begin() + static_cast<std::ptrdiff_t>(index_), *original_);
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

}  // namespace palmier
