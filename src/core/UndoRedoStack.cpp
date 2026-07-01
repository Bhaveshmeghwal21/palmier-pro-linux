// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/UndoRedoStack.cpp — implementation of the bounded undo/redo history.
//
// See UndoRedoStack.hpp for the behavioural contract. The two histories are
// std::deques so the newest command is pushed/popped at the back (the common
// path) while the oldest can be dropped from the front once the bound is reached.

#include "core/UndoRedoStack.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "core/Project.hpp"

namespace palmier {

UndoRedoStack::UndoRedoStack(std::size_t capacity)
    // Never allow a capacity below the >= 20 undo floor (Requirement 2.9).
    : capacity_(std::max(capacity, kMinCapacity)) {}

void UndoRedoStack::record(std::unique_ptr<EditCommand> cmd) {
    if (!cmd) {
        return;  // Nothing to record; leave the histories untouched.
    }

    // A new edit invalidates any commands that were undone: the redo branch is
    // no longer reachable from the current state.
    redo_.clear();

    undo_.push_back(std::move(cmd));

    // Keep the retained history bounded by discarding the oldest commands.
    while (undo_.size() > capacity_) {
        undo_.pop_front();
    }
}

CommandResult UndoRedoStack::undo(Project& project) {
    // Requirement 2.10: undo with no prior operation leaves the project
    // unchanged and reports an indication that there is nothing to undo.
    if (undo_.empty()) {
        return CommandResult::noOp("Nothing to undo");
    }

    std::unique_ptr<EditCommand> cmd = std::move(undo_.back());
    undo_.pop_back();

    Result<void> reverted = cmd->revert(project);
    if (reverted.isError()) {
        // revert() must leave the project unchanged on failure; keep the command
        // on the undo history so the history stays consistent with the state.
        Error error = std::move(reverted).error();
        undo_.push_back(std::move(cmd));
        return CommandResult::failed(std::move(error));
    }

    std::string description(cmd->name());
    redo_.push_back(std::move(cmd));
    return CommandResult::applied(std::move(description));
}

CommandResult UndoRedoStack::redo(Project& project) {
    if (redo_.empty()) {
        return CommandResult::noOp("Nothing to redo");
    }

    std::unique_ptr<EditCommand> cmd = std::move(redo_.back());
    redo_.pop_back();

    Result<void> reapplied = cmd->apply(project);
    if (reapplied.isError()) {
        // apply() must leave the project unchanged on failure; keep the command
        // on the redo history so it remains reachable.
        Error error = std::move(reapplied).error();
        redo_.push_back(std::move(cmd));
        return CommandResult::failed(std::move(error));
    }

    std::string description(cmd->name());
    undo_.push_back(std::move(cmd));
    return CommandResult::applied(std::move(description));
}

void UndoRedoStack::clear() noexcept {
    undo_.clear();
    redo_.clear();
}

}  // namespace palmier
