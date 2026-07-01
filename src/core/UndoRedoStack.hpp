// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/UndoRedoStack.hpp — the bounded undo/redo history primitive.
//
// This is the reusable primitive that gives the timeline its undo/redo
// behaviour (design.md Application/Domain Layer: "Command / Undo-Redo Stack").
// The TimelineEngine (task 3.2) owns one of these and delegates its undo()/redo()
// to it; this class does not itself know about the engine, so it can be unit
// tested in isolation.
//
// It holds two ordered histories of applied EditCommand objects:
//   * the undo history — commands that have been applied and can be reverted;
//   * the redo history — commands that were undone and can be re-applied.
//
// Behaviour it guarantees:
//   * record()      pushes an already-applied command onto the undo history and
//                   discards the redo history (a fresh edit invalidates the redo
//                   branch, as in every conventional editor).
//   * undo(project) reverts the most recently applied command; redo(project)
//                   re-applies the most recently undone one. Both return a
//                   CommandResult.
//   * At least 20 sequential undo operations are supported (Requirement 2.9):
//                   the default capacity is far above 20 and the constructor
//                   refuses to configure a capacity below the required floor.
//   * undo()/redo() on an empty history are a no-op with an indication and leave
//                   the project unchanged (Requirement 2.10).
//   * The history is bounded: once it reaches capacity, recording a new command
//                   drops the oldest one so memory stays bounded on long
//                   sessions.

#ifndef PALMIER_CORE_UNDOREDOSTACK_HPP
#define PALMIER_CORE_UNDOREDOSTACK_HPP

#include <cstddef>
#include <deque>
#include <memory>

#include "core/CommandResult.hpp"
#include "core/EditCommand.hpp"

namespace palmier {

struct Project;  // core/Project.hpp — the mutation target (forward-declared).

class UndoRedoStack {
public:
    /// The minimum undo depth the product must support (Requirement 2.9): at
    /// least 20 sequential undo operations. The configured capacity can never be
    /// set below this floor.
    static constexpr std::size_t kMinCapacity = 20;

    /// The default retained history depth. Chosen well above kMinCapacity so a
    /// typical editing session never loses reachable history.
    static constexpr std::size_t kDefaultCapacity = 100;

    static_assert(kDefaultCapacity >= kMinCapacity,
                  "default capacity must honor the >= 20 undo requirement");

    /// Constructs a stack retaining up to `capacity` applied commands. Values
    /// below kMinCapacity are raised to kMinCapacity so Requirement 2.9 always
    /// holds regardless of caller input.
    explicit UndoRedoStack(std::size_t capacity = kDefaultCapacity);

    /// Records an EditCommand that has ALREADY been applied to the project,
    /// making it available to undo(). Clears the redo history. A null command is
    /// ignored. If the undo history is at capacity, the oldest command is
    /// dropped to keep the history bounded.
    void record(std::unique_ptr<EditCommand> cmd);

    /// Reverts the most recently applied command against `project`.
    ///   * Empty history  -> CommandResult::noOp (project unchanged, Req 2.10).
    ///   * revert() fails  -> CommandResult::failed; the command is kept on the
    ///                        undo history and the project is left unchanged.
    ///   * Otherwise       -> CommandResult::applied; the command moves to the
    ///                        redo history.
    [[nodiscard]] CommandResult undo(Project& project);

    /// Re-applies the most recently undone command against `project`.
    ///   * Empty redo history -> CommandResult::noOp (project unchanged).
    ///   * apply() fails       -> CommandResult::failed; the command is kept on
    ///                            the redo history and the project is unchanged.
    ///   * Otherwise           -> CommandResult::applied; the command moves back
    ///                            onto the undo history.
    [[nodiscard]] CommandResult redo(Project& project);

    /// True if there is at least one command that can be undone.
    [[nodiscard]] bool canUndo() const noexcept { return !undo_.empty(); }
    /// True if there is at least one command that can be redone.
    [[nodiscard]] bool canRedo() const noexcept { return !redo_.empty(); }

    /// Number of commands currently available to undo / redo.
    [[nodiscard]] std::size_t undoDepth() const noexcept { return undo_.size(); }
    [[nodiscard]] std::size_t redoDepth() const noexcept { return redo_.size(); }

    /// The configured maximum retained undo depth (>= kMinCapacity).
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    /// Discards all undo and redo history. Does not touch any project.
    void clear() noexcept;

private:
    std::deque<std::unique_ptr<EditCommand>> undo_;  // back = most recent
    std::deque<std::unique_ptr<EditCommand>> redo_;  // back = most recently undone
    std::size_t capacity_;
};

}  // namespace palmier

#endif  // PALMIER_CORE_UNDOREDOSTACK_HPP
