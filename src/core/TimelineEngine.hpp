// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/TimelineEngine.hpp — the authoritative timeline / project model (task 3.2).
//
// The TimelineEngine owns the in-memory Project and is the single source of truth
// the UI, the MCP server, and the in-app agent all operate on (design.md
// Component 1: Timeline Engine). Every mutation flows through an EditCommand so
// undo/redo and agent-issued edits share one uniform path.
//
// Interface (design.md Component 1):
//   * Query
//       Project snapshot() const;                    — immutable value copy for readers
//       std::optional<Clip> clip(ClipId) const;      — look up a clip by id
//       Duration duration() const;                   — total timeline length
//   * Mutation (each returns a CommandResult for undo/redo + MCP responses)
//       CommandResult apply(std::unique_ptr<EditCommand>);
//       CommandResult undo();
//       CommandResult redo();
//   * Change notification
//       Subscription observe(std::function<void(const ChangeSet&)>);
//
// Responsibilities (design.md Component 1 + "TimelineEngine::apply"):
//   * Atomicity (Requirement 6.6): apply() either fully applies a command or
//     leaves the project byte-for-byte unchanged — never a partial mutation. The
//     engine snapshots the project before applying and rolls back on any failure.
//   * Invariant enforcement: after a successful apply the project must still
//     satisfy the timeline invariants — no negative/zero-length clip durations,
//     no negative timeline positions, and every track's clips ordered by
//     timelineStart and non-overlapping outside an explicit transition region.
//     A command whose result would violate an invariant is rejected and rolled
//     back.
//   * Undo/redo (Requirement 2.9): a successful apply is recorded on a bounded
//     UndoRedoStack supporting at least 20 sequential undo operations; undo()/
//     redo() delegate to it. undo()/redo() on empty history are a no-op with an
//     indication and leave the project unchanged (Requirement 2.10).
//   * Change notification: after every state-changing operation the engine emits
//     a granular ChangeSet (diffed from before/after) to all observers.

#ifndef PALMIER_CORE_TIMELINEENGINE_HPP
#define PALMIER_CORE_TIMELINEENGINE_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>

#include "core/ChangeSet.hpp"
#include "core/Clip.hpp"
#include "core/CommandResult.hpp"
#include "core/Duration.hpp"
#include "core/EditCommand.hpp"
#include "core/Project.hpp"
#include "core/Result.hpp"
#include "core/Subscription.hpp"
#include "core/UndoRedoStack.hpp"

namespace palmier {

class TimelineEngine {
public:
    /// Constructs an engine wrapping an empty (default) project and an
    /// undo/redo stack of the default capacity (>= 20 undo operations).
    TimelineEngine();

    /// Constructs an engine that takes ownership of an initial project.
    /// Precondition (debug-asserted): the project already satisfies the timeline
    /// invariants; callers building a project directly should validate it first.
    explicit TimelineEngine(Project initial,
                            std::size_t undoCapacity = UndoRedoStack::kDefaultCapacity);

    ~TimelineEngine();

    TimelineEngine(const TimelineEngine&) = delete;
    TimelineEngine& operator=(const TimelineEngine&) = delete;
    TimelineEngine(TimelineEngine&&) = delete;
    TimelineEngine& operator=(TimelineEngine&&) = delete;

    // --- Query -------------------------------------------------------------

    /// An immutable value copy of the current project for readers (UI/MCP).
    [[nodiscard]] Project snapshot() const;

    /// The clip with the given id, searched across every track, or std::nullopt
    /// if no such clip exists.
    [[nodiscard]] std::optional<Clip> clip(ClipId id) const;

    /// The total timeline length: the latest clip end across all tracks, or
    /// Duration::zero() when the project has no clips.
    [[nodiscard]] Duration duration() const;

    // --- Mutation ----------------------------------------------------------

    /// Apply an edit command atomically.
    ///   * cmd == nullptr           -> CommandResult::failed(InvalidArgument).
    ///   * cmd->apply fails          -> project rolled back to its prior state,
    ///                                  CommandResult::failed with the cause.
    ///   * result violates invariants -> project rolled back, CommandResult::failed
    ///                                  (FailedPrecondition). No partial mutation.
    ///   * success                    -> command recorded for undo, a ChangeSet is
    ///                                  emitted, CommandResult::applied returned.
    [[nodiscard]] CommandResult apply(std::unique_ptr<EditCommand> cmd);

    /// Revert the most recently applied command (Requirement 2.9). Empty history
    /// is a no-op with an indication (Requirement 2.10). On a real change a
    /// ChangeSet with origin Undo is emitted.
    [[nodiscard]] CommandResult undo();

    /// Re-apply the most recently undone command. Empty redo history is a no-op
    /// with an indication. On a real change a ChangeSet with origin Redo is
    /// emitted.
    [[nodiscard]] CommandResult redo();

    // --- Introspection (convenience for UI enablement) ---------------------

    [[nodiscard]] bool canUndo() const noexcept { return undoStack_.canUndo(); }
    [[nodiscard]] bool canRedo() const noexcept { return undoStack_.canRedo(); }

    // --- Change notification ----------------------------------------------

    /// Register `callback` to receive a ChangeSet after every state-changing
    /// operation. Returns an RAII Subscription; destroying it unregisters the
    /// callback. A null callback yields an inactive Subscription.
    [[nodiscard]] Subscription observe(std::function<void(const ChangeSet&)> callback);

private:
    // Callbacks live in a shared registry so a Subscription can unregister via a
    // weak reference that stays safe even if the engine is destroyed first.
    struct ObserverRegistry {
        std::unordered_map<std::uint64_t, std::function<void(const ChangeSet&)>> callbacks;
        std::uint64_t nextId = 1;
    };

    // Emit a change to every currently-registered observer.
    // NB: intentionally NOT named `emit` — that is a Qt keyword-macro (from
    // <QObject>), so a member of that name breaks any Qt translation unit that
    // includes this header (e.g. src/ui/TimelineModel.cpp + AUTOMOC).
    void notifyObservers(const ChangeSet& change) const;

    Project                          project_;
    UndoRedoStack                    undoStack_;
    std::shared_ptr<ObserverRegistry> observers_;
};

// --- Free functions (shared with commands, persistence, and tests) ---------

/// Total timeline length of `project`: the maximum clip end across all tracks,
/// or Duration::zero() if there are no clips.
[[nodiscard]] Duration timelineDuration(const Project& project);

/// Verify the timeline invariants the engine enforces after every apply:
///   * every clip is intrinsically valid (validateClip): sourceOut > sourceIn
///     (positive, non-negative duration), opacity in [0,1], gain >= 0;
///   * no clip starts at a negative timeline position;
///   * within each track, clips are ordered by timelineStart and do not overlap
///     beyond the incoming clip's explicit transition region.
/// Returns the first violation as an error Result, or ok() when all hold.
[[nodiscard]] Result<void> checkTimelineInvariants(const Project& project);

} // namespace palmier

#endif // PALMIER_CORE_TIMELINEENGINE_HPP
