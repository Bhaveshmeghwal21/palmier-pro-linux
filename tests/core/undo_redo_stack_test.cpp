// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for the command / undo-redo primitives (task 3.1):
//   * EditCommand    — the abstract, undoable edit operation.
//   * CommandResult  — the apply/undo/redo outcome (Applied / NoOp / Failed).
//   * UndoRedoStack  — the bounded undo/redo history.
//
// These exercise the two requirements the primitive is responsible for:
//   * Requirement 2.9  — at least 20 sequential undo operations restore the
//                        prior project state; redo reproduces the post-apply
//                        state.
//   * Requirement 2.10 — undo (or redo) with an empty history leaves the project
//                        unchanged and reports an indication that there is
//                        nothing to undo.
//
// The TimelineEngine and concrete commands are implemented in later tasks
// (3.2-3.3), so these tests use small in-file EditCommand doubles that mutate an
// easily-observable field of the Project.
//
// _Requirements: 2.9, 2.10_

#include "core/UndoRedoStack.hpp"

#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "core/Project.hpp"
#include "core/Uuid.hpp"

namespace palmier {
namespace {

// A minimal reversible command: sets Project.name to a new value and restores
// the previous value on revert. Re-captures the prior name on each apply so
// undo/redo cycles round-trip exactly.
class SetNameCommand : public EditCommand {
public:
    explicit SetNameCommand(std::string newName) : newName_(std::move(newName)) {}

    std::string_view name() const noexcept override { return "SetName"; }

    Result<void> apply(Project& project) override {
        previousName_ = project.name;
        project.name = newName_;
        return ok();
    }

    Result<void> revert(Project& project) override {
        project.name = previousName_;
        return ok();
    }

private:
    std::string newName_;
    std::string previousName_;
};

// A command whose revert always fails without touching the project, used to
// verify failure handling and history consistency.
class RevertFailsCommand : public EditCommand {
public:
    std::string_view name() const noexcept override { return "RevertFails"; }

    Result<void> apply(Project& project) override {
        project.name = "applied";
        return ok();
    }

    Result<void> revert(Project&) override {
        return err(failedPrecondition("revert intentionally failed"));
    }
};

Project makeProject(std::string name = "initial") {
    Project p;
    p.id = Uuid::generateV4();
    p.name = std::move(name);
    return p;
}

// Applies a SetNameCommand to `project` and records it, mirroring what the
// TimelineEngine will do in task 3.2.
void applyAndRecord(UndoRedoStack& stack, Project& project, std::string newName) {
    auto cmd = std::make_unique<SetNameCommand>(std::move(newName));
    ASSERT_TRUE(cmd->apply(project).isOk());
    stack.record(std::move(cmd));
}

// --- CommandResult shapes --------------------------------------------------

TEST(CommandResult, AppliedIsOkAndChanged) {
    const auto r = CommandResult::applied("MoveClip");
    EXPECT_TRUE(r.isOk());
    EXPECT_TRUE(r.changed());
    EXPECT_FALSE(r.isNoOp());
    EXPECT_FALSE(r.isError());
    EXPECT_EQ(r.message(), "MoveClip");
    EXPECT_EQ(r.outcome(), CommandOutcome::Applied);
}

TEST(CommandResult, NoOpIsOkButNotChanged) {
    const auto r = CommandResult::noOp("Nothing to undo");
    EXPECT_TRUE(r.isOk());
    EXPECT_FALSE(r.changed());
    EXPECT_TRUE(r.isNoOp());
    EXPECT_FALSE(r.isError());
    EXPECT_EQ(r.message(), "Nothing to undo");
}

TEST(CommandResult, FailedCarriesError) {
    const auto r = CommandResult::failed(notFound("clip missing"));
    EXPECT_FALSE(r.isOk());
    EXPECT_FALSE(r.changed());
    EXPECT_TRUE(r.isError());
    EXPECT_EQ(r.error().code(), ErrorCode::NotFound);
}

// --- Empty-history behaviour (Requirement 2.10) ----------------------------

TEST(UndoRedoStack, UndoOnEmptyHistoryIsNoOpAndLeavesProjectUnchanged) {
    UndoRedoStack stack;
    Project project = makeProject("unchanged");

    const auto result = stack.undo(project);

    EXPECT_TRUE(result.isNoOp());
    EXPECT_FALSE(result.changed());
    EXPECT_FALSE(result.message().empty());  // an indication is provided
    EXPECT_EQ(project.name, "unchanged");
    EXPECT_FALSE(stack.canUndo());
}

TEST(UndoRedoStack, RedoOnEmptyHistoryIsNoOpAndLeavesProjectUnchanged) {
    UndoRedoStack stack;
    Project project = makeProject("unchanged");

    const auto result = stack.redo(project);

    EXPECT_TRUE(result.isNoOp());
    EXPECT_EQ(project.name, "unchanged");
    EXPECT_FALSE(stack.canRedo());
}

// --- Basic round-trip (Requirement 2.9) ------------------------------------

TEST(UndoRedoStack, UndoRestoresPriorStateAndRedoReproducesIt) {
    UndoRedoStack stack;
    Project project = makeProject("v0");

    applyAndRecord(stack, project, "v1");
    ASSERT_EQ(project.name, "v1");
    EXPECT_TRUE(stack.canUndo());

    const auto undo = stack.undo(project);
    EXPECT_TRUE(undo.changed());
    EXPECT_EQ(project.name, "v0");
    EXPECT_FALSE(stack.canUndo());
    EXPECT_TRUE(stack.canRedo());

    const auto redo = stack.redo(project);
    EXPECT_TRUE(redo.changed());
    EXPECT_EQ(project.name, "v1");
    EXPECT_TRUE(stack.canUndo());
    EXPECT_FALSE(stack.canRedo());
}

// --- At least 20 sequential undo operations (Requirement 2.9) --------------

TEST(UndoRedoStack, SupportsAtLeastTwentySequentialUndoOperations) {
    UndoRedoStack stack;
    Project project = makeProject("v0");

    constexpr int kEdits = 25;  // more than the required 20
    for (int i = 1; i <= kEdits; ++i) {
        applyAndRecord(stack, project, "v" + std::to_string(i));
    }
    ASSERT_EQ(project.name, "v25");
    ASSERT_EQ(stack.undoDepth(), static_cast<std::size_t>(kEdits));

    // Undo every edit; each step restores the immediately prior state.
    for (int i = kEdits - 1; i >= 0; --i) {
        const auto result = stack.undo(project);
        EXPECT_TRUE(result.changed());
        EXPECT_EQ(project.name, "v" + std::to_string(i));
    }

    EXPECT_FALSE(stack.canUndo());
    EXPECT_EQ(project.name, "v0");
    EXPECT_EQ(stack.redoDepth(), static_cast<std::size_t>(kEdits));
}

// --- Recording a new edit invalidates the redo branch ----------------------

TEST(UndoRedoStack, RecordingAfterUndoClearsRedoHistory) {
    UndoRedoStack stack;
    Project project = makeProject("v0");

    applyAndRecord(stack, project, "v1");
    ASSERT_TRUE(stack.undo(project).changed());
    ASSERT_TRUE(stack.canRedo());

    applyAndRecord(stack, project, "branch");
    EXPECT_FALSE(stack.canRedo());
    EXPECT_EQ(stack.redoDepth(), 0u);
}

// --- Boundedness -----------------------------------------------------------

TEST(UndoRedoStack, HistoryIsBoundedByCapacityDroppingOldest) {
    UndoRedoStack stack(UndoRedoStack::kMinCapacity);  // smallest allowed: 20
    Project project = makeProject("v0");

    // Record 30 edits into a capacity-20 stack.
    for (int i = 1; i <= 30; ++i) {
        applyAndRecord(stack, project, "v" + std::to_string(i));
    }
    EXPECT_EQ(stack.undoDepth(), UndoRedoStack::kMinCapacity);

    // Only the 20 most recent commands remain undoable.
    std::size_t undone = 0;
    while (stack.canUndo()) {
        ASSERT_TRUE(stack.undo(project).changed());
        ++undone;
    }
    EXPECT_EQ(undone, UndoRedoStack::kMinCapacity);
}

TEST(UndoRedoStack, CapacityBelowFloorIsRaisedToMinimum) {
    UndoRedoStack stack(3);  // below the 20 floor
    EXPECT_EQ(stack.capacity(), UndoRedoStack::kMinCapacity);
}

// --- Failure handling ------------------------------------------------------

TEST(UndoRedoStack, FailedRevertKeepsCommandOnUndoHistory) {
    UndoRedoStack stack;
    Project project = makeProject("before");

    auto cmd = std::make_unique<RevertFailsCommand>();
    ASSERT_TRUE(cmd->apply(project).isOk());
    ASSERT_EQ(project.name, "applied");
    stack.record(std::move(cmd));

    const auto result = stack.undo(project);
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::FailedPrecondition);
    // The command remains undoable and did not migrate to the redo history.
    EXPECT_TRUE(stack.canUndo());
    EXPECT_FALSE(stack.canRedo());
}

TEST(UndoRedoStack, NullCommandIsIgnored) {
    UndoRedoStack stack;
    stack.record(nullptr);
    EXPECT_FALSE(stack.canUndo());
}

TEST(UndoRedoStack, ClearDiscardsAllHistory) {
    UndoRedoStack stack;
    Project project = makeProject("v0");
    applyAndRecord(stack, project, "v1");
    ASSERT_TRUE(stack.undo(project).changed());
    ASSERT_TRUE(stack.canRedo());

    stack.clear();
    EXPECT_FALSE(stack.canUndo());
    EXPECT_FALSE(stack.canRedo());
}

}  // namespace
}  // namespace palmier
