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
//   * SetTextContentCommand— change a text clip's displayed string (Requirement 9).
//   * SetTextStyleCommand — change a text clip's font, size, colour, alignment
//                           and/or screen position (Requirement 9). A text clip
//                           is itself created through the existing
//                           AddClipCommand — see the "Text and titles" section
//                           below for why that needed no new command.
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
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/Clip.hpp"
#include "core/ColorSpace.hpp"
#include "core/Duration.hpp"
#include "core/EditCommand.hpp"
#include "core/Effect.hpp"
#include "core/FrameRate.hpp"
#include "core/Resolution.hpp"
#include "core/Result.hpp"
#include "core/ToneCurve.hpp"
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
//
// The clip's asset is registered in Project.assets as part of the SAME command
// (as PlaceGeneratedClipCommand already does for a generated clip), because
// Project.assets is the only asset table a document carries: it is what
// ProjectStore serializes and what ProjectSession::openProject rebuilds the media
// library from, and validateProject requires every Clip.assetRef to resolve in it.
// Registering here — rather than in whichever service produced the asset — is what
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
// SetTrackMutedCommand — set a track's muted flag.
// ---------------------------------------------------------------------------
//
// Sets the muted flag of the track carrying `trackId` to `muted`, capturing the
// prior value so revert() restores it exactly. Nothing else about the track — its
// kind, its name, its clips or their order — is touched, so muting is a pure
// per-track state change.
//
// This is the command behind the `timeline.set_track_muted` tool, which is what
// the offline interpreter's "mute track N" / "unmute track N" phrases resolve to.
// It lives in the core so that muting travels the same command path — atomic,
// undoable, observable, invariant-checked — as every other edit, which is what
// makes a mute reversible by one undo like any other edit.
//
// An unknown track identifier is rejected without mutating the project. Setting
// the flag to the value it already holds is applied rather than refused: it is a
// legal, idempotent edit whose revert is equally a no-op, and refusing it would
// make the tool's success depend on state the caller cannot see.
class SetTrackMutedCommand final : public EditCommand {
public:
    SetTrackMutedCommand(Uuid trackId, bool muted);

    [[nodiscard]] std::string_view name() const noexcept override { return "SetTrackMuted"; }
    [[nodiscard]] Result<void> apply(Project& project) override;
    [[nodiscard]] Result<void> revert(Project& project) override;

    /// The target track.
    [[nodiscard]] Uuid trackId() const noexcept { return trackId_; }

    /// The flag value this command installs.
    [[nodiscard]] bool muted() const noexcept { return muted_; }

    /// The flag value the track carried before apply(), or std::nullopt before the
    /// first successful apply().
    [[nodiscard]] std::optional<bool> priorMuted() const noexcept { return prior_; }

private:
    Uuid                trackId_;
    bool                muted_;
    std::optional<bool> prior_;  // captured on apply for an exact revert
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

// ---------------------------------------------------------------------------
// RippleDeleteCommand — remove a clip and close the gap it leaves.
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
    ClipId            clipId_;
    Uuid              trackId_;
    std::vector<Clip> priorClips_;  // captured on apply for an exact revert
    bool              captured_ = false;
};

// ---------------------------------------------------------------------------
// RippleTrimCommand — trim a clip's edge and move the rest of the track with it.
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
    RippleTrimCommand(ClipId clipId, Edge edge, Duration newBoundary,
                      FrameRate fps, Duration sourceDuration);

    [[nodiscard]] std::string_view name() const noexcept override { return "RippleTrim"; }
    [[nodiscard]] Result<void> apply(Project& project) override;
    [[nodiscard]] Result<void> revert(Project& project) override;

    /// The timeline shift the last successful apply() produced for the named clip:
    /// the change in its duration for Edge::End, or the negated change in its
    /// leading edge for Edge::Start. Exposed so a caller can report what a trim
    /// actually did after clamping.
    [[nodiscard]] Duration appliedDelta() const noexcept { return appliedDelta_; }

private:
    ClipId    clipId_;
    Edge      edge_;
    Duration  newBoundary_;
    FrameRate fps_;
    Duration  sourceDuration_;

    Duration           appliedDelta_;
    std::vector<Track> priorTracks_;  // captured on apply for an exact revert
    bool               captured_ = false;
};

// ---------------------------------------------------------------------------
// CloseGapCommand — close the gap immediately following a clip.
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
    ClipId            clipId_;
    Uuid              trackId_;
    Duration          closedGap_;
    std::vector<Clip> priorClips_;  // captured on apply for an exact revert
    bool              captured_ = false;
};

// ===========================================================================
// Effect lifecycle management (usable-editor task 9; Requirement 6)
//
// AddEffectCommand already appends an effect; the three commands below cover the
// rest of a clip's effect chain lifecycle: removing one, reordering the chain (the
// rendered result depends on effect order, Requirement 6.4), and changing an
// existing effect's parameter. All three name an effect by its stable Uuid and
// refuse — leaving the project unchanged — when the clip or the named effect is
// not found (Requirement 6.5).
// ===========================================================================

// ---------------------------------------------------------------------------
// RemoveEffectCommand — remove one effect from a clip's chain by id.
// ---------------------------------------------------------------------------
class RemoveEffectCommand final : public EditCommand {
public:
    RemoveEffectCommand(ClipId clipId, Uuid effectId);

    [[nodiscard]] std::string_view name() const noexcept override { return "RemoveEffect"; }
    [[nodiscard]] Result<void> apply(Project& project) override;
    [[nodiscard]] Result<void> revert(Project& project) override;

private:
    ClipId              clipId_;
    Uuid                effectId_;
    std::size_t         index_ = 0;
    std::optional<Effect> removed_;  // captured on apply for an exact revert
};

// ---------------------------------------------------------------------------
// ReorderEffectsCommand — reorder a clip's effect chain (preserves effect count).
// ---------------------------------------------------------------------------
//
// A pure permutation of the chain: unlike ReorderClipsCommand, no positional
// field needs recomputing, since an effect carries no timeline geometry of its
// own. `newOrder` must name every effect currently on the clip exactly once;
// anything else — a wrong count, an unknown id, a repeat — is refused.
class ReorderEffectsCommand final : public EditCommand {
public:
    ReorderEffectsCommand(ClipId clipId, std::vector<Uuid> newOrder);

    [[nodiscard]] std::string_view name() const noexcept override { return "ReorderEffects"; }
    [[nodiscard]] Result<void> apply(Project& project) override;
    [[nodiscard]] Result<void> revert(Project& project) override;

private:
    ClipId              clipId_;
    std::vector<Uuid>   newOrder_;
    std::vector<Effect> priorEffects_;  // captured on apply for an exact revert
    bool                captured_ = false;
};

// ---------------------------------------------------------------------------
// SetEffectParameterCommand — set (or insert) a named parameter on one effect.
// ---------------------------------------------------------------------------
//
// revert() restores the parameter's prior value, or removes the key entirely if
// it did not previously exist, so a parameter that was absent before apply() is
// absent again after revert() rather than left at 0.
class SetEffectParameterCommand final : public EditCommand {
public:
    SetEffectParameterCommand(ClipId clipId, Uuid effectId, std::string parameter, double value);

    [[nodiscard]] std::string_view name() const noexcept override { return "SetEffectParameter"; }
    [[nodiscard]] Result<void> apply(Project& project) override;
    [[nodiscard]] Result<void> revert(Project& project) override;

private:
    ClipId      clipId_;
    Uuid        effectId_;
    std::string parameter_;
    double      value_;
    bool        hadPrior_ = false;
    double      prior_ = 0.0;
    bool        captured_ = false;
};

// ---------------------------------------------------------------------------
// EditCurvePointCommand — add, move or remove one tone-curve control point.
// ---------------------------------------------------------------------------
//
// Requirement 5.7 (monitoring-and-grading): adding, moving and removing a control
// point are each ONE undoable edit. SetEffectParameterCommand cannot serve, because a
// point is a pair: adding one sets two parameters and would therefore be two history
// entries, so a single Undo would leave a half-written point behind — an X with no Y,
// which core::curvePoints deliberately refuses to read as a point. The user would see
// their point vanish and the curve change shape in a way no single action explains.
//
// The three operations are one command rather than three because they share their
// target (clip, effect, channel) and differ only in which coordinates matter. What
// makes them one *undoable* thing is the inverse below, which is identical for all
// three.
//
// revert() restores THE WHOLE CHANNEL'S prior parameter set rather than undoing the
// specific mutation. That is not laziness: removing a middle point renumbers every
// point after it, so the inverse of "remove p1" is not "add a point at p1" but
// "restore the numbering that existed before". Capturing the channel wholesale makes
// the inverse exact for every operation, including that one, without a special case —
// and the captured set is at most a few dozen doubles.
class EditCurvePointCommand final : public EditCommand {
public:
    enum class Operation { Add, Move, Remove };

    /// `index` is ignored for Add (the point is appended); `x`/`y` are ignored for
    /// Remove. Add appends rather than inserting at `index` because rendering sorts by
    /// x regardless, so an insertion position would be a distinction without a
    /// difference — while a point's index IS its identity for a later move or remove.
    EditCurvePointCommand(ClipId clipId, Uuid effectId, CurveChannel channel,
                          Operation operation, std::size_t index, double x, double y);

    [[nodiscard]] std::string_view name() const noexcept override { return "EditCurvePoint"; }
    [[nodiscard]] Result<void> apply(Project& project) override;
    [[nodiscard]] Result<void> revert(Project& project) override;

private:
    ClipId       clipId_;
    Uuid         effectId_;
    CurveChannel channel_;
    Operation    operation_;
    std::size_t  index_;
    double       x_;
    double       y_;
    /// Every parameter of this channel as it stood before apply(), and nothing else:
    /// an edit to the master curve must not restore the red curve.
    std::map<std::string, double> prior_;
    bool captured_ = false;
};

// ---------------------------------------------------------------------------
// SetProjectSettingsCommand — change frame rate, canvas and/or colour space.
// ---------------------------------------------------------------------------
//
// Requirement 7: a project's frame rate, canvas resolution and colour space can
// each be changed after creation, accepting the same ranges `project.create`
// accepts (services::kMinFramesPerSecond..kMaxFramesPerSecond,
// kMinCanvasWidth..kMaxCanvasWidth x kMinCanvasHeight..kMaxCanvasHeight; the
// range check itself is the Tool_Surface's, since core has no reason to depend
// on services:: — this command validates only that a supplied FrameRate/
// Resolution is internally well-formed via isValid()).
//
// Any subset of the three settings may change in one call; each argument left
// as std::nullopt is left exactly as it was. This is what makes "any setting
// changes... undoable in one Undo" (Requirement 7.4) hold even when several
// fields change together: one command, one history entry, one exact revert.
//
// Requirement 7.3: no clip is touched. Every clip's timeline position and
// source range is a Duration (an absolute nanosecond count with no embedded
// frame rate — see Duration.hpp), so changing timelineFps has nothing to
// migrate; the clips are simply interpreted against a different rate from the
// next read onward.
class SetProjectSettingsCommand final : public EditCommand {
public:
    SetProjectSettingsCommand(std::optional<FrameRate> fps, std::optional<Resolution> canvas,
                              std::optional<ColorSpace> colorSpace);

    [[nodiscard]] std::string_view name() const noexcept override {
        return "SetProjectSettings";
    }
    [[nodiscard]] Result<void> apply(Project& project) override;
    [[nodiscard]] Result<void> revert(Project& project) override;

private:
    std::optional<FrameRate>  fps_;
    std::optional<Resolution> canvas_;
    std::optional<ColorSpace> colorSpace_;

    // Captured on apply for an exact revert; only the fields this command
    // actually changes are meaningful, but all three are captured together since
    // Project's settings are a single unit to snapshot.
    FrameRate   priorFps_;
    Resolution  priorCanvas_;
    ColorSpace  priorColorSpace_ = defaultColorSpace();
    bool        captured_ = false;
};

// ===========================================================================
// Text and titles (usable-editor task 12; Requirement 9)
//
// Creating a text clip needs no new command: it is an ordinary AddClipCommand
// whose Clip carries a populated textStyle and an unset (nil, "invalid")
// assetRef — AddClipCommand's own asset-registration step already skips a
// clip whose assetRef.isValid() is false, so a text clip is placed, ordered
// and made undoable by the exact path every other clip already uses, with
// zero changes to that command. What text needs beyond placement is a way to
// change its content and its styling after creation — the two commands below.
// ===========================================================================

// ---------------------------------------------------------------------------
// SetTextContentCommand — change a text clip's displayed string.
// ---------------------------------------------------------------------------
//
// Refused, leaving the project unchanged, when the named clip does not exist
// or is not a text clip (Requirement 9.2's "each undoable in one Undo" only
// makes sense for a clip that actually carries a TextStyle to change).
class SetTextContentCommand final : public EditCommand {
public:
    SetTextContentCommand(ClipId clipId, std::string content);

    [[nodiscard]] std::string_view name() const noexcept override { return "SetTextContent"; }
    [[nodiscard]] Result<void> apply(Project& project) override;
    [[nodiscard]] Result<void> revert(Project& project) override;

private:
    ClipId      clipId_;
    std::string content_;
    std::string prior_;      // captured on apply for an exact revert
    bool        captured_ = false;
};

// ---------------------------------------------------------------------------
// SetTextStyleCommand — change a text clip's font, size, colour, alignment
// and/or screen position.
// ---------------------------------------------------------------------------
//
// Every field is std::optional, mirroring SetProjectSettingsCommand: a field
// left std::nullopt is left exactly as it was, so any subset of font family,
// point size, colour, alignment and position changes in one undoable edit
// (Requirement 9.2). core has no services:: dependency, so this command
// validates only that the resulting TextStyle is internally well-formed
// (TextStyle::isValid()) — the same core/services split
// SetProjectSettingsCommand already established. Refused, leaving the project
// unchanged, when the named clip does not exist or is not a text clip, or when
// the requested change would make the style invalid.
class SetTextStyleCommand final : public EditCommand {
public:
    SetTextStyleCommand(ClipId clipId, std::optional<std::string> fontFamily,
                        std::optional<double> pointSize,
                        std::optional<double> colorR, std::optional<double> colorG,
                        std::optional<double> colorB, std::optional<double> colorA,
                        std::optional<TextAlignment> alignment,
                        std::optional<double> x, std::optional<double> y);

    [[nodiscard]] std::string_view name() const noexcept override { return "SetTextStyle"; }
    [[nodiscard]] Result<void> apply(Project& project) override;
    [[nodiscard]] Result<void> revert(Project& project) override;

private:
    ClipId clipId_;
    std::optional<std::string>   fontFamily_;
    std::optional<double>        pointSize_;
    std::optional<double>        colorR_, colorG_, colorB_, colorA_;
    std::optional<TextAlignment> alignment_;
    std::optional<double>        x_, y_;

    TextStyle prior_;        // captured on apply for an exact revert
    bool      captured_ = false;
};

// ===========================================================================
// Captions and transcription (usable-editor task 13; Requirement 10)
//
// Creating a caption cue needs no new command either, for the identical reason
// creating a text clip did not (task 12): it is an ordinary AddClipCommand
// whose Clip carries a populated captionText and an unset (nil, "invalid")
// assetRef. What captions need beyond placement is a way to change a cue's
// text and to retime it — the two commands below.
// ===========================================================================

// ---------------------------------------------------------------------------
// SetCaptionTextCommand — change a caption cue's displayed string.
// ---------------------------------------------------------------------------
//
// Refused, leaving the project unchanged, when the named clip does not exist,
// is not a caption cue, or when the new text is empty (Requirement 10's own
// "a caption cue has text" invariant, checked by TextStyle-analogous
// well-formedness — see Clip::isCaptionCue()'s own doc comment).
class SetCaptionTextCommand final : public EditCommand {
public:
    SetCaptionTextCommand(ClipId clipId, std::string text);

    [[nodiscard]] std::string_view name() const noexcept override { return "SetCaptionText"; }
    [[nodiscard]] Result<void> apply(Project& project) override;
    [[nodiscard]] Result<void> revert(Project& project) override;

private:
    ClipId      clipId_;
    std::string text_;
    std::string prior_;      // captured on apply for an exact revert
    bool        captured_ = false;
};

// ---------------------------------------------------------------------------
// RetimeCaptionCueCommand — change a caption cue's timing in one undoable edit.
// ---------------------------------------------------------------------------
//
// Requirement 10.2's "retime" operation is a change to when a cue starts and/or
// how long it lasts. Rather than requiring two separate calls — MoveClipCommand
// for the start, TrimClipCommand for the duration — retiming sets timelineStart
// and sourceOut (with sourceIn fixed at zero, exactly like a freshly-created
// cue) together, so any combination of "move it", "lengthen it", "shorten it",
// or both at once is the same single command and the same single Undo. Rejects
// a non-positive resulting duration or a negative timelineStart, leaving the
// project unchanged, and — like MoveClipCommand — rejects (and rolls back) a
// destination that would overlap another cue on the same track.
class RetimeCaptionCueCommand final : public EditCommand {
public:
    RetimeCaptionCueCommand(ClipId clipId, std::optional<Duration> newTimelineStart,
                            std::optional<Duration> newDuration);

    [[nodiscard]] std::string_view name() const noexcept override {
        return "RetimeCaptionCue";
    }
    [[nodiscard]] Result<void> apply(Project& project) override;
    [[nodiscard]] Result<void> revert(Project& project) override;

private:
    ClipId                  clipId_;
    std::optional<Duration> newTimelineStart_;
    std::optional<Duration> newDuration_;

    Uuid              trackId_;
    std::vector<Clip> priorClips_;  // captured on apply for an exact revert
    bool              captured_ = false;
};

}  // namespace palmier

#endif  // PALMIER_CORE_EDITCOMMANDS_HPP
