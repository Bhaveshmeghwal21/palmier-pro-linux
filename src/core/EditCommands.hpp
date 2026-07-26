// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/EditCommands.hpp — the concrete, undoable timeline editing commands (task 3.3).
//
// These are the EditCommand subclasses that realize the interactive editing
// operations of the Timeline_Editor (design.md Component 1; Requirement 2). Every
// one of them mutates a Project through the same apply()/revert() contract the
// engine drives, so the UI, the MCP server, and the in-app agent all share one
// undoable, observable editing path (design.md "All mutations flow through a
// Command object").
//
// The commands implemented here:
//
//   * AddClipCommand      — insert a clip onto a track at its timelineStart
//                           (Requirement 2.1 placement). The clip is inserted in
//                           timelineStart order so the track stays sorted.
//   * DeleteClipCommand   — remove a clip by id, capturing its exact prior
//                           track and position so revert reinserts it verbatim.
//   * MoveClipCommand     — reposition a clip to a new timelineStart on its track
//                           (Requirement 2.2). A drop that would overlap another
//                           clip on the same track is rejected with a clear error
//                           and the clip is left at its original position
//                           (Requirement 2.3 — invalid-drop indication).
//   * TrimClipCommand     — trim a clip's start or end edge, constrained to a
//                           minimum duration of one frame and a maximum equal to
//                           the source media duration (Requirement 2.4).
//   * SplitClipCommand    — split a clip at an interior playhead into two
//                           contiguous clips whose combined duration and source
//                           range equal the original (Requirement 2.5); fails
//                           cleanly when the playhead is outside the clip so the
//                           editor can surface the "nothing to split" indication
//                           (Requirement 2.6).
//   * ReorderClipsCommand — reorder a track's clips into a caller-supplied
//                           permutation, repacking them contiguously while
//                           preserving the track's clip count (Requirement 2.7).
//   * AddEffectCommand    — append an effect to a clip's effect chain.
//   * AddTrackCommand     — append a track after the last existing track of the
//                           same kind, capped at 64 tracks per kind
//                           (Requirements 3.3, 3.8).
//   * RemoveTrackCommand  — remove a track and every clip on it, preserving the
//                           relative order of the remaining tracks
//                           (Requirement 3.10).
//   * SetTransitionCommand— set a clip's incoming transition. Promoted here from
//                           a registry-local command in
//                           services/ToolRegistry.cpp so `timeline.add_transition`
//                           runs through the same core command path as every
//                           other edit (audit finding).
//
// Atomicity / invariants: the TimelineEngine snapshots the project before every
// apply() and rolls back on failure or on any timeline-invariant violation (no
// negative/zero-length clips; each track ordered by timelineStart and
// non-overlapping outside a transition region). Each command below is written to
// leave the project unchanged on the failure paths it detects itself, and to
// produce an invariant-satisfying result on success. Every apply() captures the
// exact prior state it needs so a subsequent revert() is an exact inverse and so
// redo (a second apply() from the restored state) reproduces the same result.

#ifndef PALMIER_CORE_EDITCOMMANDS_HPP
#define PALMIER_CORE_EDITCOMMANDS_HPP

#include <cstddef>
#include <optional>
#include <vector>

#include "core/Clip.hpp"
#include "core/Duration.hpp"
#include "core/EditCommand.hpp"
#include "core/Effect.hpp"
#include "core/FrameRate.hpp"
#include "core/Result.hpp"
#include "core/Track.hpp"
#include "core/Transition.hpp"
#include "core/Uuid.hpp"

namespace palmier {

struct Project;  // core/Project.hpp — the mutation target (forward-declared).

// ---------------------------------------------------------------------------
// AddClipCommand — place a clip on a track.
// ---------------------------------------------------------------------------
//
// Inserts `clip` into the track identified by `trackId`, keeping the track's
// clips ordered by timelineStart (the clip is inserted at the first position
// whose existing clip starts strictly later). Whether the placement overlaps an
// existing clip is left to the engine's invariant check, which rejects and rolls
// back an overlapping add. revert() removes the inserted clip by id.
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
};

// ---------------------------------------------------------------------------
// DeleteClipCommand — remove a clip by id.
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
    ClipId                trackId_;
    ClipId                clipId_;
    std::size_t           index_ = 0;
    std::optional<Clip>   removed_;  // captured on apply for an exact revert
};

// ---------------------------------------------------------------------------
// MoveClipCommand — reposition a clip to a new timelineStart on its own track.
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
    ClipId            clipId_;
    Duration          newStart_;
    Uuid              trackId_;
    std::vector<Clip> priorClips_;  // captured on apply for an exact revert
    bool              captured_ = false;
};

// ---------------------------------------------------------------------------
// TrimClipCommand — trim a clip's start or end edge.
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
    TrimClipCommand(ClipId clipId, Edge edge, Duration newBoundary,
                    FrameRate fps, Duration sourceDuration);

    [[nodiscard]] std::string_view name() const noexcept override { return "TrimClip"; }
    [[nodiscard]] Result<void> apply(Project& project) override;
    [[nodiscard]] Result<void> revert(Project& project) override;

private:
    ClipId    clipId_;
    Edge      edge_;
    Duration  newBoundary_;
    FrameRate fps_;
    Duration  sourceDuration_;

    // Prior edge state captured on apply for an exact revert.
    Duration  priorSourceIn_;
    Duration  priorSourceOut_;
    Duration  priorTimelineStart_;
    bool      captured_ = false;
};

// ---------------------------------------------------------------------------
// SplitClipCommand — split a clip at an interior playhead into two clips.
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
    ClipId                clipId_;
    Duration              playhead_;
    Uuid                  trackId_;
    std::size_t           index_ = 0;
    std::optional<Clip>   original_;   // captured on apply for an exact revert
    std::optional<ClipId> rightId_;    // fixed on first apply for deterministic redo
};

// ---------------------------------------------------------------------------
// ReorderClipsCommand — reorder a track's clips into a new sequence.
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
    Uuid                trackId_;
    std::vector<ClipId> newOrder_;
    std::vector<Clip>   priorClips_;  // captured on apply for an exact revert
    bool                captured_ = false;
};

// ---------------------------------------------------------------------------
// AddEffectCommand — append an effect to a clip's effect chain.
// ---------------------------------------------------------------------------
//
// Appends `effect` to the end of the target clip's effects list. revert() removes
// the effect by its id.
class AddEffectCommand final : public EditCommand {
public:
    AddEffectCommand(ClipId clipId, Effect effect);

    [[nodiscard]] std::string_view name() const noexcept override { return "AddEffect"; }
    [[nodiscard]] Result<void> apply(Project& project) override;
    [[nodiscard]] Result<void> revert(Project& project) override;

private:
    ClipId clipId_;
    Effect effect_;
};

// ---------------------------------------------------------------------------
// AddTrackCommand — append a track of a given kind.
// ---------------------------------------------------------------------------
//
// The new track is inserted immediately after the last existing track of the
// same kind, so the project's video and audio lanes each grow at their own tail
// while every existing track — and every clip on it — is left untouched
// (Requirement 3.3). When the project holds no track of that kind yet, the new
// track is appended at the end of the track list.
//
// A project may hold at most `kMaxTracksPerKind` tracks of one kind; a request
// that would exceed the cap is rejected without mutating the project and names
// the offending `kind` argument (Requirement 3.8).
//
// The track's identifier is fixed at construction, so it is available to the
// caller before apply() and an undo/redo cycle reproduces the identical track
// (`trackId()`). revert() removes that track again; because the command only
// ever appends an empty track, revert() cannot lose clip data.
class AddTrackCommand final : public EditCommand {
public:
    /// Maximum number of tracks of one kind a project may hold (Requirement 3.3).
    static constexpr std::size_t kMaxTracksPerKind = 64;

    /// Appends a track of `kind` carrying a freshly generated identifier.
    explicit AddTrackCommand(TrackKind kind);

    /// Appends a track of `kind` carrying the caller-supplied identifier, which
    /// must not already be present in the project.
    AddTrackCommand(TrackKind kind, Uuid trackId);

    [[nodiscard]] std::string_view name() const noexcept override { return "AddTrack"; }
    [[nodiscard]] Result<void> apply(Project& project) override;
    [[nodiscard]] Result<void> revert(Project& project) override;

    /// The identifier the added track carries (stable from construction).
    [[nodiscard]] Uuid trackId() const noexcept { return trackId_; }

    /// The kind of track this command appends.
    [[nodiscard]] TrackKind kind() const noexcept { return kind_; }

    /// The index the track was inserted at, or std::nullopt before the first
    /// successful apply().
    [[nodiscard]] std::optional<std::size_t> insertedIndex() const noexcept { return index_; }

private:
    TrackKind                  kind_;
    Uuid                       trackId_;
    std::optional<std::size_t> index_;  // set on apply; also the revert anchor
};

// ---------------------------------------------------------------------------
// RemoveTrackCommand — remove a track and every clip on it.
// ---------------------------------------------------------------------------
//
// Erases the track carrying `trackId` together with all of its clips. The
// remaining tracks keep their relative order (Requirement 3.10). The removed
// track is captured whole — clips, mute and lock state — together with its
// index, so revert() reinserts it verbatim at its former position. An unknown
// track identifier is rejected without mutating the project (Requirement 3.8).
class RemoveTrackCommand final : public EditCommand {
public:
    explicit RemoveTrackCommand(Uuid trackId);

    [[nodiscard]] std::string_view name() const noexcept override { return "RemoveTrack"; }
    [[nodiscard]] Result<void> apply(Project& project) override;
    [[nodiscard]] Result<void> revert(Project& project) override;

    /// The number of clips removed with the track, or std::nullopt before the
    /// first successful apply().
    [[nodiscard]] std::optional<std::size_t> removedClipCount() const noexcept;

private:
    Uuid                 trackId_;
    std::size_t          index_ = 0;
    std::optional<Track> removed_;  // captured on apply for an exact revert
};

// ---------------------------------------------------------------------------
// SetTransitionCommand — set a clip's incoming transition.
// ---------------------------------------------------------------------------
//
// Replaces the target clip's `transitionIn` with `transition`, capturing the
// prior value (which may be absent) so revert() restores it exactly. This is the
// command behind the `timeline.add_transition` tool; it lives in the core so that
// transitions are applied through the same command path — atomic, undoable,
// observable, invariant-checked — as every other edit.
class SetTransitionCommand final : public EditCommand {
public:
    SetTransitionCommand(ClipId clipId, Transition transition);

    [[nodiscard]] std::string_view name() const noexcept override { return "AddTransition"; }
    [[nodiscard]] Result<void> apply(Project& project) override;
    [[nodiscard]] Result<void> revert(Project& project) override;

    /// The identifier of the transition this command installs.
    [[nodiscard]] Uuid transitionId() const noexcept { return transition_.id; }

private:
    ClipId                    clipId_;
    Transition                transition_;
    std::optional<Transition> prior_;      // captured on apply for an exact revert
    bool                      captured_ = false;
};

}  // namespace palmier

#endif  // PALMIER_CORE_EDITCOMMANDS_HPP
