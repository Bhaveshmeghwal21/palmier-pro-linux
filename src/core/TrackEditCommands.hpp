// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/TrackEditCommands.hpp - adding, removing and muting tracks.
//
// One of the six command-family headers behind core/EditCommands.hpp. The grouping mirrors
// TrackEditCommands.cpp exactly, so a command's declaration and its
// implementation are always in the matching pair of files.
//
// The apply()/revert() contract these all share, and the invariants the engine enforces
// around them, are documented once in core/EditCommands.hpp.

#ifndef PALMIER_CORE_TRACKEDITCOMMANDS_HPP
#define PALMIER_CORE_TRACKEDITCOMMANDS_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "core/Clip.hpp"
#include "core/EditCommand.hpp"
#include "core/Result.hpp"
#include "core/Track.hpp"
#include "core/Uuid.hpp"

namespace palmier {

struct Project;  // core/Project.hpp - the mutation target (forward-declared).

// ---------------------------------------------------------------------------
// AddTrackCommand â€” append a track of a given kind.
// ---------------------------------------------------------------------------
//
// The new track is inserted immediately after the last existing track of the
// same kind, so the project's video and audio lanes each grow at their own tail
// while every existing track â€” and every clip on it â€” is left untouched
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
    TrackKind kind_;
    Uuid trackId_;
    std::optional<std::size_t> index_;  // set on apply; also the revert anchor
};

// ---------------------------------------------------------------------------
// RemoveTrackCommand â€” remove a track and every clip on it.
// ---------------------------------------------------------------------------
//
// Erases the track carrying `trackId` together with all of its clips. The
// remaining tracks keep their relative order (Requirement 3.10). The removed
// track is captured whole â€” clips, mute and lock state â€” together with its
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
    Uuid trackId_;
    std::size_t index_ = 0;
    std::optional<Track> removed_;  // captured on apply for an exact revert
};

// ---------------------------------------------------------------------------
// SetTrackMutedCommand â€” set a track's muted flag.
// ---------------------------------------------------------------------------
//
// Sets the muted flag of the track carrying `trackId` to `muted`, capturing the
// prior value so revert() restores it exactly. Nothing else about the track â€” its
// kind, its name, its clips or their order â€” is touched, so muting is a pure
// per-track state change.
//
// This is the command behind the `timeline.set_track_muted` tool, which is what
// the offline interpreter's "mute track N" / "unmute track N" phrases resolve to.
// It lives in the core so that muting travels the same command path â€” atomic,
// undoable, observable, invariant-checked â€” as every other edit, which is what
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
    Uuid trackId_;
    bool muted_;
    std::optional<bool> prior_;  // captured on apply for an exact revert
};

}  // namespace palmier

#endif  // PALMIER_CORE_TRACKEDITCOMMANDS_HPP
