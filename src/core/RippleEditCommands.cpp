// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/RippleEditCommands.cpp — the ripple edits, which close or open the gap they create: ripple delete,
// ripple trim and close gap.
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
            return err(failedPrecondition("RippleDeleteCommand: closing the gap would move clip " +
                                          offender + " before the start of the timeline"));
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

RippleTrimCommand::RippleTrimCommand(ClipId clipId, Edge edge, Duration newBoundary, FrameRate fps,
                                     Duration sourceDuration)
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

        Track* track = loc->track;
        const std::size_t index = loc->index;
        Clip& clip = track->clips[index];

        // Apply the same source-time delta to this member, refusing when the
        // member cannot accommodate it while keeping a frame inside its source.
        if (edge_ == Edge::End) {
            const Duration newOut = clip.sourceOut + delta;
            if (newOut < clip.sourceIn + minDuration || newOut > sourceDuration_) {
                return rollback(
                    failedPrecondition("RippleTrimCommand: grouped clip " + idLabel(target) +
                                       " cannot be trimmed by the same amount within its source"));
            }
            clip.sourceOut = newOut;
        } else {
            const Duration newIn = clip.sourceIn + delta;
            if (newIn.isNegative() || newIn > clip.sourceOut - minDuration) {
                return rollback(
                    failedPrecondition("RippleTrimCommand: grouped clip " + idLabel(target) +
                                       " cannot be trimmed by the same amount within its source"));
            }
            const Duration newStart = clip.timelineStart + delta;
            if (newStart.isNegative()) {
                return rollback(
                    failedPrecondition("RippleTrimCommand: trimming clip " + idLabel(target) +
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
            return rollback(
                failedPrecondition("RippleTrimCommand: the trim would overlap a clip on track " +
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

    Track* track = loc->track;
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
            return err(failedPrecondition("CloseGapCommand: closing the gap would move clip " +
                                          offender + " before the start of the timeline"));
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

}  // namespace palmier
