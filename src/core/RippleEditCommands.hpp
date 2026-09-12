// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/RippleEditCommands.hpp - edits that shift everything after them.
//
// One of the six command-family headers behind core/EditCommands.hpp. The grouping mirrors
// RippleEditCommands.cpp exactly, so a command's declaration and its
// implementation are always in the matching pair of files.
//
// The apply()/revert() contract these all share, and the invariants the engine enforces
// around them, are documented once in core/EditCommands.hpp.

#ifndef PALMIER_CORE_RIPPLEEDITCOMMANDS_HPP
#define PALMIER_CORE_RIPPLEEDITCOMMANDS_HPP

#include <string>
#include <string_view>
#include <vector>

#include "core/Clip.hpp"
#include "core/Duration.hpp"
#include "core/EditCommand.hpp"
#include "core/FrameRate.hpp"
#include "core/Result.hpp"
#include "core/Track.hpp"
#include "core/Uuid.hpp"

namespace palmier {

struct Project;  // core/Project.hpp - the mutation target (forward-declared).

// ---------------------------------------------------------------------------
// RippleDeleteCommand â€” remove a clip and close the gap it leaves.
// ---------------------------------------------------------------------------
//
// Requirement 5.1: removes the clip with `clipId` and shifts every later clip on
// the same track earlier by exactly the removed clip's duration, so the track
// closes up instead of leaving a hole where the clip was. "Later" means later in
// the track's timelineStart order, which is the order the engine's invariant
// already guarantees.
//
// Only the clip's own track is touched: a ripple delete is a single-track edit, so
// clips on other tracks keep their absolute positions and cross-track sync is the
// caller's business. The whole change is one command, hence one Undo
// (Requirement 5.3). Like MoveClipCommand, revert() restores a captured copy of
// the affected track's clip vector, which is exact by construction.
class RippleDeleteCommand final : public EditCommand {
public:
    explicit RippleDeleteCommand(ClipId clipId);

    [[nodiscard]] std::string_view name() const noexcept override { return "RippleDelete"; }
    [[nodiscard]] Result<void> apply(Project& project) override;
    [[nodiscard]] Result<void> revert(Project& project) override;

private:
    ClipId clipId_;
    Uuid trackId_;
    std::vector<Clip> priorClips_;  // captured on apply for an exact revert
    bool captured_ = false;
};

// ---------------------------------------------------------------------------
// RippleTrimCommand â€” trim a clip's edge and move the rest of the track with it.
// ---------------------------------------------------------------------------
//
// Requirement 5.2: changes a clip's out-point (or in-point) and shifts every later
// clip on the same track by exactly the change in duration, so a trim never leaves
// a gap and never causes an overlap. The clamping rules are TrimClipCommand's, so
// the two commands agree on what a legal edge is: the retained range keeps at least
// one frame at `fps` and stays inside [0, sourceDuration].
//
// Edge::End moves `sourceOut` and leaves `timelineStart` fixed; Edge::Start moves
// `sourceIn` and shifts `timelineStart` by the same delta, so the retained content
// stays where it was on the timeline and the clip's leading edge is what moves.
//
// MULTICAM GROUPS (upstream PR 397). When the trimmed clip belongs to a
// `Project.clipGroups` entry, the identical source-time trim is applied to every
// other member of that group and each member's own track is rippled the same way,
// so grouped angles that started aligned stay aligned. Members are trimmed by the
// same delta rather than to the same absolute boundary, because grouped angles
// need not share an in-point. A member that cannot accommodate the delta while
// keeping a one-frame minimum inside its source fails the whole command, which
// then leaves the project exactly as it was.
//
// Because a group spans tracks, apply() captures every track (not just one) and
// revert() restores them wholesale: that is the simplest construction that is
// exactly reversible for a multi-track edit, and it keeps the one-Undo guarantee
// of Requirement 5.3 intact.
class RippleTrimCommand final : public EditCommand {
public:
    /// Which edge of the clip is being trimmed. Matches TrimClipCommand::Edge.
    enum class Edge { Start, End };

    /// `newBoundary` is the requested new source boundary for the named clip: the
    /// new sourceIn for Edge::Start, or the new sourceOut for Edge::End. `fps`
    /// sets the one-frame minimum duration and `sourceDuration` is the length of
    /// the referenced source media.
    RippleTrimCommand(ClipId clipId, Edge edge, Duration newBoundary, FrameRate fps,
                      Duration sourceDuration);

    [[nodiscard]] std::string_view name() const noexcept override { return "RippleTrim"; }
    [[nodiscard]] Result<void> apply(Project& project) override;
    [[nodiscard]] Result<void> revert(Project& project) override;

    /// The timeline shift the last successful apply() produced for the named clip:
    /// the change in its duration for Edge::End, or the negated change in its
    /// leading edge for Edge::Start. Exposed so a caller can report what a trim
    /// actually did after clamping.
    [[nodiscard]] Duration appliedDelta() const noexcept { return appliedDelta_; }

private:
    ClipId clipId_;
    Edge edge_;
    Duration newBoundary_;
    FrameRate fps_;
    Duration sourceDuration_;

    Duration appliedDelta_;
    std::vector<Track> priorTracks_;  // captured on apply for an exact revert
    bool captured_ = false;
};

// ---------------------------------------------------------------------------
// CloseGapCommand â€” close the gap immediately following a clip.
// ---------------------------------------------------------------------------
//
// Requirement 5.5: removes the gap that immediately follows `clipId` by shifting
// that clip's later neighbours earlier by exactly the gap's length, leaving every
// clip's duration and source range untouched. Only positions change.
//
// The command is refused, with the project unchanged, when the clip is the last on
// its track (there is no following gap to close) or when its successor already
// begins at or before the clip's end (there is no gap). Refusing rather than
// silently succeeding is what lets the shell keep the action honest.
class CloseGapCommand final : public EditCommand {
public:
    explicit CloseGapCommand(ClipId clipId);

    [[nodiscard]] std::string_view name() const noexcept override { return "CloseGap"; }
    [[nodiscard]] Result<void> apply(Project& project) override;
    [[nodiscard]] Result<void> revert(Project& project) override;

    /// The gap length the last successful apply() removed.
    [[nodiscard]] Duration closedGap() const noexcept { return closedGap_; }

private:
    ClipId clipId_;
    Uuid trackId_;
    Duration closedGap_;
    std::vector<Clip> priorClips_;  // captured on apply for an exact revert
    bool captured_ = false;
};

}  // namespace palmier

#endif  // PALMIER_CORE_RIPPLEEDITCOMMANDS_HPP
