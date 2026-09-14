// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/MarkerEditCommands.hpp — adding and removing timeline markers.
//
// One command family beside the others core/EditCommands.hpp aggregates. Markers are
// project-level annotations (core/Marker.hpp), not clip or track properties, so they get
// their own small family rather than joining TrackEditCommands or ClipEditCommands.

#ifndef PALMIER_CORE_MARKEREDITCOMMANDS_HPP
#define PALMIER_CORE_MARKEREDITCOMMANDS_HPP

#include <optional>
#include <string>
#include <string_view>

#include "core/Duration.hpp"
#include "core/EditCommand.hpp"
#include "core/Marker.hpp"
#include "core/Result.hpp"
#include "core/Uuid.hpp"

namespace palmier {

struct Project;  // core/Project.hpp — the mutation target (forward-declared).

// ---------------------------------------------------------------------------
// AddMarkerCommand — append a marker at a timeline position.
// ---------------------------------------------------------------------------
//
// Appends unconditionally: unlike a clip, a marker has no length and cannot overlap
// another one, so there is no placement rule to enforce and nothing for this command to
// refuse except a duplicate identifier. The marker's identifier is fixed at construction,
// available to the caller before apply(), and stable across an undo/redo cycle.
class AddMarkerCommand final : public EditCommand {
public:
    /// Appends a marker at `timelineNs` carrying a freshly generated identifier.
    AddMarkerCommand(Duration timelineNs, std::string label);

    /// Appends a marker carrying the caller-supplied identifier, which must not
    /// already be present in the project.
    AddMarkerCommand(Uuid markerId, Duration timelineNs, std::string label);

    [[nodiscard]] std::string_view name() const noexcept override { return "AddMarker"; }
    [[nodiscard]] Result<void> apply(Project& project) override;
    [[nodiscard]] Result<void> revert(Project& project) override;

    /// The identifier the added marker carries (stable from construction).
    [[nodiscard]] Uuid markerId() const noexcept { return markerId_; }

private:
    Uuid markerId_;
    Duration timelineNs_;
    std::string label_;
    bool applied_ = false;  // set on a successful apply; the revert anchor
};

// ---------------------------------------------------------------------------
// RemoveMarkerCommand — remove one marker by id.
// ---------------------------------------------------------------------------
//
// Removes the marker carrying `markerId`. The removed marker is captured whole
// together with its position in the list, so revert() reinserts it verbatim at its
// former index. An unknown marker identifier is rejected without mutating the project,
// matching RemoveTrackCommand's own contract for an unknown track.
class RemoveMarkerCommand final : public EditCommand {
public:
    explicit RemoveMarkerCommand(Uuid markerId);

    [[nodiscard]] std::string_view name() const noexcept override { return "RemoveMarker"; }
    [[nodiscard]] Result<void> apply(Project& project) override;
    [[nodiscard]] Result<void> revert(Project& project) override;

private:
    Uuid markerId_;
    std::size_t index_ = 0;
    std::optional<Marker> removed_;  // captured on apply for an exact revert
};

}  // namespace palmier

#endif  // PALMIER_CORE_MARKEREDITCOMMANDS_HPP
