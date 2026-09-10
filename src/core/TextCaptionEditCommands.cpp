// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/TextCaptionEditCommands.cpp — the text/title and caption cue edits.
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
    : clipId_(clipId),
      fontFamily_(std::move(fontFamily)),
      pointSize_(pointSize),
      colorR_(colorR),
      colorG_(colorG),
      colorB_(colorB),
      colorA_(colorA),
      alignment_(alignment),
      x_(x),
      y_(y) {}

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
        return err(
            failedPrecondition("RetimeCaptionCueCommand: destination position must be >= 0"));
    }
    if (!candidateDuration.isPositive()) {
        return err(
            failedPrecondition("RetimeCaptionCueCommand: the resulting duration must be > 0"));
    }

    Track* track = loc->track;
    trackId_ = track->id;
    priorClips_ = track->clips;  // capture for an exact revert / rollback
    captured_ = true;

    Clip& target = track->clips[loc->index];
    target.timelineStart = candidateStart;
    target.sourceIn = Duration::zero();
    target.sourceOut = candidateDuration;
    std::stable_sort(track->clips.begin(), track->clips.end(), [](const Clip& a, const Clip& b) {
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
        return err(failedPrecondition("RetimeCaptionCueCommand: revert before a successful apply"));
    }
    Track* track = findTrack(project, trackId_);
    if (track == nullptr) {
        return err(notFound("RetimeCaptionCueCommand: track " + idLabel(trackId_) + " not found"));
    }
    track->clips = priorClips_;
    return ok();
}

}  // namespace palmier
