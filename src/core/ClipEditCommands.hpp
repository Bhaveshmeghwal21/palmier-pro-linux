// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/ClipEditCommands.hpp - clip placement, trimming and splitting.
//
// One of the six command-family headers behind core/EditCommands.hpp. The grouping mirrors
// ClipEditCommands.cpp exactly, so a command's declaration and its
// implementation are always in the matching pair of files.
//
// The apply()/revert() contract these all share, and the invariants the engine enforces
// around them, are documented once in core/EditCommands.hpp.

#ifndef PALMIER_CORE_CLIPEDITCOMMANDS_HPP
#define PALMIER_CORE_CLIPEDITCOMMANDS_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/Clip.hpp"
#include "core/Duration.hpp"
#include "core/EditCommand.hpp"
#include "core/FrameRate.hpp"
#include "core/Result.hpp"
#include "core/Uuid.hpp"

namespace palmier {

struct Project;  // core/Project.hpp - the mutation target (forward-declared).

// ---------------------------------------------------------------------------
// AddClipCommand â€” place a clip on a track.
// ---------------------------------------------------------------------------
//
// Inserts `clip` into the track identified by `trackId`, keeping the track's
// clips ordered by timelineStart (the clip is inserted at the first position
// whose existing clip starts strictly later). Whether the placement overlaps an
// existing clip is left to the engine's invariant check, which rejects and rolls
// back an overlapping add. revert() removes the inserted clip by id.
//
// The clip's asset is registered in Project.assets as part of the SAME command
// (as PlaceGeneratedClipCommand already does for a generated clip), because
// Project.assets is the only asset table a document carries: it is what
// ProjectStore serializes and what ProjectSession::openProject rebuilds the media
// library from, and validateProject requires every Clip.assetRef to resolve in it.
// Registering here â€” rather than in whichever service produced the asset â€” is what
// makes "a saved project can be re-opened" hold for EVERY caller that places a
// clip (the `timeline.add_clip` tool, the timeline view model's drag-and-drop, the
// agent), including callers whose asset was registered only in a session-level
// MediaManager. Registration is skipped when the table already resolves the
// identity (resolution is by assetId, so no duplicate entry is created) and when
// the ref is the nil identity (an unregisterable asset id, which the media library
// rebuild rejects); revert() removes the entry only when this command added it, so
// undo/redo round-trips exactly and a rolled-back add leaves the table untouched.
class AddClipCommand final : public EditCommand {
public:
    AddClipCommand(Uuid trackId, Clip clip);

    [[nodiscard]] std::string_view name() const noexcept override { return "AddClip"; }
    [[nodiscard]] Result<void> apply(Project& project) override;
    [[nodiscard]] Result<void> revert(Project& project) override;

    /// The id the added clip carries (its own id).
    [[nodiscard]] ClipId clipId() const noexcept { return clip_.id; }

private:
    Uuid trackId_;
    Clip clip_;
    bool assetAdded_ = false;  ///< True iff apply() appended clip_.assetRef to Project.assets.
};

// ---------------------------------------------------------------------------
// DeleteClipCommand â€” remove a clip by id.
// ---------------------------------------------------------------------------
//
// Removes the clip with `clipId` from whichever track holds it, capturing the
// track id, the clip's index within that track, and a full copy of the clip so
// revert() can reinsert it at exactly its former position.
class DeleteClipCommand final : public EditCommand {
public:
    explicit DeleteClipCommand(ClipId clipId);

    [[nodiscard]] std::string_view name() const noexcept override { return "DeleteClip"; }
    [[nodiscard]] Result<void> apply(Project& project) override;
    [[nodiscard]] Result<void> revert(Project& project) override;

private:
    ClipId trackId_;
    ClipId clipId_;
    std::size_t index_ = 0;
    std::optional<Clip> removed_;  // captured on apply for an exact revert
};

// ---------------------------------------------------------------------------
// MoveClipCommand â€” reposition a clip to a new timelineStart on its own track.
// ---------------------------------------------------------------------------
//
// Sets the target clip's timelineStart to `newStart` and re-sorts its track by
// timelineStart. A negative destination, or a destination that would overlap
// another clip on the same track outside a transition region, is rejected: the
// track is restored to its prior contents and a FailedPrecondition error is
// returned so the editor can show the invalid-drop indication (Requirement 2.3)
// while the clip stays at its original position. revert() restores the track's
// clips to their captured prior state.
class MoveClipCommand final : public EditCommand {
public:
    MoveClipCommand(ClipId clipId, Duration newStart);

    [[nodiscard]] std::string_view name() const noexcept override { return "MoveClip"; }
    [[nodiscard]] Result<void> apply(Project& project) override;
    [[nodiscard]] Result<void> revert(Project& project) override;

private:
    ClipId clipId_;
    Duration newStart_;
    Uuid trackId_;
    std::vector<Clip> priorClips_;  // captured on apply for an exact revert
    bool captured_ = false;
};

// ---------------------------------------------------------------------------
// TrimClipCommand â€” trim a clip's start or end edge.
// ---------------------------------------------------------------------------
//
// Trimming the End edge moves the clip's out-point (sourceOut) to the requested
// boundary, leaving timelineStart fixed. Trimming the Start edge moves the
// in-point (sourceIn) and shifts timelineStart by the same delta so the retained
// content keeps its timeline position. The resulting clip duration is constrained
// to at least one frame (from `fps`) and at most the source media duration
// (`sourceDuration`), and the source range is kept within [0, sourceDuration]
// (Requirement 2.4). revert() restores the captured sourceIn/sourceOut/
// timelineStart.
class TrimClipCommand final : public EditCommand {
public:
    /// Which edge of the clip is being trimmed.
    enum class Edge { Start, End };

    /// `newBoundary` is the requested new source boundary: the new sourceIn for
    /// Edge::Start, or the new sourceOut for Edge::End (both in source time).
    /// `fps` sets the one-frame minimum duration; `sourceDuration` is the full
    /// length of the referenced source media (the maximum extent of the range).
    TrimClipCommand(ClipId clipId, Edge edge, Duration newBoundary, FrameRate fps,
                    Duration sourceDuration);

    [[nodiscard]] std::string_view name() const noexcept override { return "TrimClip"; }
    [[nodiscard]] Result<void> apply(Project& project) override;
    [[nodiscard]] Result<void> revert(Project& project) override;

private:
    ClipId clipId_;
    Edge edge_;
    Duration newBoundary_;
    FrameRate fps_;
    Duration sourceDuration_;

    // Prior edge state captured on apply for an exact revert.
    Duration priorSourceIn_;
    Duration priorSourceOut_;
    Duration priorTimelineStart_;
    bool captured_ = false;
};

// ---------------------------------------------------------------------------
// SplitClipCommand â€” split a clip at an interior playhead into two clips.
// ---------------------------------------------------------------------------
//
// When `playhead` lies strictly between the clip's start and end, the clip is
// replaced by two contiguous clips divided at the playhead: the left half keeps
// the original id, timelineStart, and any transitionIn; the right half gets a
// fresh id, begins at the playhead, and carries the remainder of the source
// range. Their combined duration and source range equal the original's
// (Requirement 2.5). When `playhead` is at or outside the clip's boundaries the
// command fails without mutating anything (Requirement 2.6), so the editor can
// surface the "no clip to split" indication. revert() restores the original
// single clip. The right half's generated id is fixed on the first apply so redo
// reproduces the identical result.
class SplitClipCommand final : public EditCommand {
public:
    SplitClipCommand(ClipId clipId, Duration playhead);

    [[nodiscard]] std::string_view name() const noexcept override { return "SplitClip"; }
    [[nodiscard]] Result<void> apply(Project& project) override;
    [[nodiscard]] Result<void> revert(Project& project) override;

    /// The id assigned to the right-hand half once the split has been applied,
    /// or std::nullopt before the first successful apply.
    [[nodiscard]] std::optional<ClipId> rightClipId() const noexcept { return rightId_; }

private:
    ClipId clipId_;
    Duration playhead_;
    Uuid trackId_;
    std::size_t index_ = 0;
    std::optional<Clip> original_;   // captured on apply for an exact revert
    std::optional<ClipId> rightId_;  // fixed on first apply for deterministic redo
};

// ---------------------------------------------------------------------------
// ReorderClipsCommand â€” reorder a track's clips into a new sequence.
// ---------------------------------------------------------------------------
//
// `newOrder` must be a permutation of the ids of the track's current clips. The
// clips are rebuilt in that order and repacked contiguously (each clip begins
// where the previous one ends), starting at the earliest of the track's current
// clip start positions. The clip count is preserved (Requirement 2.7) and the
// result is ordered and non-overlapping. revert() restores the captured prior
// clip vector.
class ReorderClipsCommand final : public EditCommand {
public:
    ReorderClipsCommand(Uuid trackId, std::vector<ClipId> newOrder);

    [[nodiscard]] std::string_view name() const noexcept override { return "ReorderClips"; }
    [[nodiscard]] Result<void> apply(Project& project) override;
    [[nodiscard]] Result<void> revert(Project& project) override;

private:
    Uuid trackId_;
    std::vector<ClipId> newOrder_;
    std::vector<Clip> priorClips_;  // captured on apply for an exact revert
    bool captured_ = false;
};

}  // namespace palmier

#endif  // PALMIER_CORE_CLIPEDITCOMMANDS_HPP
