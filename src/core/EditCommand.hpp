// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/EditCommand.hpp — the abstract, undoable edit operation.
//
// Every mutation of a Project flows through an EditCommand (design.md
// Application/Domain Layer: "All mutations flow through a Command object for
// uniform undo/redo and for agent-issued edits"). The UI, the MCP server, and
// the in-app agent all drive edits by constructing an EditCommand and handing it
// to the TimelineEngine (implemented in task 3.2), which applies it and records
// it on the undo/redo stack (UndoRedoStack, task 3.1).
//
// A command is a self-contained, reversible unit of work:
//   * apply(project)  performs the edit and captures whatever prior state is
//                     needed to reverse it later. It is atomic: on failure the
//                     project is left exactly as it was (no partial mutation),
//                     matching the design contract for TimelineEngine::apply.
//   * revert(project) restores the project to the state that preceded the most
//                     recent successful apply() of this command instance.
//
// The apply/revert pair is what makes at least 20 sequential undo operations
// possible (Requirement 2.9): the stack keeps the applied command objects and
// replays their revert() in reverse order.
//
// Concrete commands (AddClipCommand, MoveClipCommand, SplitClipCommand, ...) are
// implemented in task 3.3; this header defines only the interface they share.

#ifndef PALMIER_CORE_EDITCOMMAND_HPP
#define PALMIER_CORE_EDITCOMMAND_HPP

#include <string_view>

#include "core/Result.hpp"

namespace palmier {

struct Project;  // core/Project.hpp — the mutation target (forward-declared).

/// Abstract base for every undoable edit operation on a Project.
///
/// Lifetime/usage contract:
///   1. A command instance is constructed with its parameters (e.g. the target
///      clip id and destination position).
///   2. apply() is invoked exactly once before revert(). A successful apply()
///      captures enough prior state internally to make a subsequent revert()
///      exact.
///   3. revert() and a later apply() (redo) may then alternate any number of
///      times; each apply() re-captures the state it needs and each revert()
///      restores it, so redo/undo cycles round-trip (Requirement 2.9).
class EditCommand {
public:
    virtual ~EditCommand() = default;

    /// A short, stable name for the command kind (e.g. "AddClip", "MoveClip").
    /// Used as the human-readable change description in CommandResult and in
    /// the ChangeSet events the engine emits (task 3.2).
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    /// Perform the edit, mutating `project`, and capture the information needed
    /// to reverse it.
    ///
    /// Atomicity: on failure the command MUST leave `project` byte-for-byte
    /// unchanged and return a descriptive Error — never a partial mutation
    /// (design.md "TimelineEngine::apply" postconditions).
    [[nodiscard]] virtual Result<void> apply(Project& project) = 0;

    /// Reverse the effect of the most recent successful apply(), restoring the
    /// project state that preceded it. Precondition: the immediately preceding
    /// call on this instance was a successful apply(). Like apply(), a failing
    /// revert() must leave `project` unchanged.
    [[nodiscard]] virtual Result<void> revert(Project& project) = 0;

protected:
    EditCommand() = default;
    // Commands are held and moved via std::unique_ptr<EditCommand>; copying a
    // polymorphic command would slice, so copy/move are disabled on the base.
    EditCommand(const EditCommand&) = delete;
    EditCommand& operator=(const EditCommand&) = delete;
};

}  // namespace palmier

#endif  // PALMIER_CORE_EDITCOMMAND_HPP
