// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/ChangeSet.hpp — the granular change event the TimelineEngine emits.
//
// The TimelineEngine notifies observers (the Qt views and MCP subscribers) after
// every state-changing apply/undo/redo (design.md Component 1: "Emit granular
// ChangeSet events so both the Qt views and MCP subscribers stay in sync"). A
// ChangeSet is that notification: it describes what changed so a subscriber can
// refresh only the affected part of its view rather than rebuilding from the full
// snapshot.
//
// The engine derives a ChangeSet by diffing the project before and after the
// operation, so the event is independent of which concrete EditCommand produced
// it: the same diff-based ChangeSet is emitted whether a change originated from a
// fresh apply(), an undo(), or a redo().
//
// Granularity carried:
//   * origin            — whether the change came from apply / undo / redo, or
//                          from a whole-project replacement (Reset).
//   * description        — the human-readable command name (e.g. "MoveClip").
//   * previousDuration / currentDuration — the total timeline length before and
//                          after, so a ruler/scrollbar can resize without a full
//                          recompute.
//   * addedClips         — ids of clips present after but not before.
//   * removedClips       — ids of clips present before but not after.
//   * modifiedClips      — ids of clips present in both whose fields changed.
//   * affectedTracks     — ids of tracks that gained, lost, or changed a clip.

#ifndef PALMIER_CORE_CHANGESET_HPP
#define PALMIER_CORE_CHANGESET_HPP

#include <string>
#include <vector>

#include "core/Clip.hpp"
#include "core/Duration.hpp"
#include "core/Uuid.hpp"

namespace palmier {

/// Which engine operation produced a ChangeSet.
enum class ChangeOrigin {
    Apply, ///< A fresh command was applied.
    Undo,  ///< A previously applied command was reverted.
    Redo,  ///< A previously undone command was re-applied.
    Reset, ///< The whole project value was replaced (project create/open).
};

// Reset is deliberately distinct from Apply: a project load/create is not an
// undoable edit, so consumers that count applied commands (e.g. the MCP tool
// executor, which counts Apply emissions to know how much to roll back) must not
// treat it as one, while views still get a full refresh notification.

/// A granular description of how the project changed as a result of one
/// state-changing engine operation. Emitted to every observer registered via
/// TimelineEngine::observe.
struct ChangeSet {
    ChangeOrigin origin = ChangeOrigin::Apply;
    std::string  description;

    Duration previousDuration;
    Duration currentDuration;

    std::vector<ClipId> addedClips;
    std::vector<ClipId> removedClips;
    std::vector<ClipId> modifiedClips;
    std::vector<Uuid>   affectedTracks;

    /// True iff no clip was added, removed, or modified. A ChangeSet the engine
    /// emits is never empty (the engine only notifies on an actual change), but
    /// the predicate is useful to observers and tests.
    [[nodiscard]] bool empty() const noexcept {
        return addedClips.empty() && removedClips.empty() && modifiedClips.empty();
    }
};

} // namespace palmier

#endif // PALMIER_CORE_CHANGESET_HPP
