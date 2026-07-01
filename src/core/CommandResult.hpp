// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/CommandResult.hpp — the outcome of an apply/undo/redo on the timeline.
//
// TimelineEngine::apply / undo / redo each return a CommandResult (design.md
// Component 1: "all return a Command result for undo/redo + MCP responses").
// Where the lower-level EditCommand::apply/revert return a plain Result<void>
// (success or Error), CommandResult adds the extra distinction the editor, MCP
// server, and agent need at the operation boundary: a request can succeed and
// change the project, succeed but do nothing, or fail.
//
// The "succeed but do nothing" outcome is what Requirement 2.10 calls for: an
// undo requested with an empty history leaves the project unchanged and reports
// an indication that no operation is available to undo. That is not an error —
// nothing went wrong — so it is modelled as a distinct no-op outcome carrying an
// indication message, rather than as a failure.

#ifndef PALMIER_CORE_COMMANDRESULT_HPP
#define PALMIER_CORE_COMMANDRESULT_HPP

#include <string>
#include <utility>

#include "core/Error.hpp"

namespace palmier {

/// The three mutually exclusive outcomes of an apply/undo/redo request.
enum class CommandOutcome {
    Applied,  ///< The operation succeeded and changed the project state.
    NoOp,     ///< Nothing to do (e.g. undo with empty history). State unchanged.
    Failed,   ///< The operation failed; the project state is left unchanged.
};

/// The result of a TimelineEngine apply/undo/redo request.
///
/// Exactly one of three shapes:
///   * Applied — carries a human-readable change description.
///   * NoOp    — carries an indication message for the user (Requirement 2.10).
///   * Failed  — carries the Error describing why.
class CommandResult {
public:
    /// The operation succeeded and mutated the project. `description` is a short
    /// human-readable summary of the change (typically the command's name()).
    [[nodiscard]] static CommandResult applied(std::string description = {}) {
        return CommandResult(CommandOutcome::Applied, std::move(description), Error{});
    }

    /// The operation had nothing to do and left the project unchanged.
    /// `indication` is the message to surface to the user (e.g. "Nothing to
    /// undo") per Requirement 2.10.
    [[nodiscard]] static CommandResult noOp(std::string indication) {
        return CommandResult(CommandOutcome::NoOp, std::move(indication), Error{});
    }

    /// The operation failed; the project is unchanged. Carries the cause.
    [[nodiscard]] static CommandResult failed(Error error) {
        return CommandResult(CommandOutcome::Failed, std::string{}, std::move(error));
    }

    [[nodiscard]] CommandOutcome outcome() const noexcept { return outcome_; }

    /// True unless the operation failed (i.e. Applied or NoOp).
    [[nodiscard]] bool isOk() const noexcept { return outcome_ != CommandOutcome::Failed; }
    /// True only when the operation actually changed the project.
    [[nodiscard]] bool changed() const noexcept { return outcome_ == CommandOutcome::Applied; }
    /// True when the operation succeeded but made no change (empty history, etc.).
    [[nodiscard]] bool isNoOp() const noexcept { return outcome_ == CommandOutcome::NoOp; }
    /// True when the operation failed.
    [[nodiscard]] bool isError() const noexcept { return outcome_ == CommandOutcome::Failed; }

    /// For Applied: the change description. For NoOp: the user-facing
    /// indication. Empty for Failed (see error()).
    [[nodiscard]] const std::string& message() const noexcept { return message_; }

    /// The failure cause. Precondition: isError().
    [[nodiscard]] const Error& error() const noexcept { return error_; }

private:
    CommandResult(CommandOutcome outcome, std::string message, Error error)
        : outcome_(outcome), message_(std::move(message)), error_(std::move(error)) {}

    CommandOutcome outcome_;
    std::string    message_;
    Error          error_;
};

}  // namespace palmier

#endif  // PALMIER_CORE_COMMANDRESULT_HPP
