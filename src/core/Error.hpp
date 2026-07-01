// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/Error.hpp — shared error type used across Palmier interfaces.
//
// Palmier's domain core is UI-agnostic and headless-capable (it is driven by the
// UI, the MCP server, and the in-app agent alike). Rather than propagate failures
// via exceptions across those boundaries, fallible operations return a Result<T>
// (see Result.hpp) carrying either a value or this Error. An Error pairs a coarse,
// machine-inspectable ErrorCode with a human-readable message.

#ifndef PALMIER_CORE_ERROR_HPP
#define PALMIER_CORE_ERROR_HPP

#include <string>
#include <string_view>
#include <utility>

namespace palmier {

/// Coarse, machine-inspectable classification of a failure.
///
/// Codes are intentionally general so they can be mapped onto MCP/HTTP error
/// responses and UI notifications without leaking implementation detail. The
/// human-readable message on the Error carries the specifics.
enum class ErrorCode {
    Unknown = 0,        ///< Unclassified failure.
    InvalidArgument,    ///< A caller-supplied argument was malformed or empty.
    OutOfRange,         ///< A value fell outside its permitted range.
    NotFound,           ///< A referenced entity (clip, asset, id) does not exist.
    AlreadyExists,      ///< An entity that must be unique already exists.
    FailedPrecondition, ///< The operation is invalid for the current state.
    Unsupported,        ///< The requested format/feature/version is not supported.
    PermissionDenied,   ///< The operation was refused for permission reasons.
    Unauthenticated,    ///< The operation requires an authenticated session.
    Timeout,            ///< The operation did not complete within its budget.
    Cancelled,          ///< The operation was cancelled before completion.
    Io,                 ///< An underlying I/O operation failed.
    Internal,           ///< An unexpected internal invariant was violated.
};

/// Human-readable, stable name for an ErrorCode (useful for logs/tests).
[[nodiscard]] constexpr std::string_view toStringView(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::Unknown:             return "Unknown";
        case ErrorCode::InvalidArgument:     return "InvalidArgument";
        case ErrorCode::OutOfRange:          return "OutOfRange";
        case ErrorCode::NotFound:            return "NotFound";
        case ErrorCode::AlreadyExists:       return "AlreadyExists";
        case ErrorCode::FailedPrecondition:  return "FailedPrecondition";
        case ErrorCode::Unsupported:         return "Unsupported";
        case ErrorCode::PermissionDenied:    return "PermissionDenied";
        case ErrorCode::Unauthenticated:     return "Unauthenticated";
        case ErrorCode::Timeout:             return "Timeout";
        case ErrorCode::Cancelled:           return "Cancelled";
        case ErrorCode::Io:                  return "Io";
        case ErrorCode::Internal:            return "Internal";
    }
    return "Unknown";
}

/// A failure value: a coarse code plus a descriptive message.
class Error {
public:
    Error() = default;

    explicit Error(ErrorCode code, std::string message = {})
        : code_(code), message_(std::move(message)) {}

    [[nodiscard]] ErrorCode code() const noexcept { return code_; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }

    /// A single-line "Code: message" (or just "Code" when no message) rendering.
    [[nodiscard]] std::string toString() const {
        std::string out{toStringView(code_)};
        if (!message_.empty()) {
            out += ": ";
            out += message_;
        }
        return out;
    }

    friend bool operator==(const Error& lhs, const Error& rhs) {
        return lhs.code_ == rhs.code_ && lhs.message_ == rhs.message_;
    }
    friend bool operator!=(const Error& lhs, const Error& rhs) { return !(lhs == rhs); }

private:
    ErrorCode   code_ = ErrorCode::Unknown;
    std::string message_;
};

// --- Convenience constructors for the most common failure kinds -------------

[[nodiscard]] inline Error makeError(ErrorCode code, std::string message = {}) {
    return Error(code, std::move(message));
}
[[nodiscard]] inline Error invalidArgument(std::string message) {
    return Error(ErrorCode::InvalidArgument, std::move(message));
}
[[nodiscard]] inline Error outOfRange(std::string message) {
    return Error(ErrorCode::OutOfRange, std::move(message));
}
[[nodiscard]] inline Error notFound(std::string message) {
    return Error(ErrorCode::NotFound, std::move(message));
}
[[nodiscard]] inline Error unsupported(std::string message) {
    return Error(ErrorCode::Unsupported, std::move(message));
}
[[nodiscard]] inline Error failedPrecondition(std::string message) {
    return Error(ErrorCode::FailedPrecondition, std::move(message));
}

} // namespace palmier

#endif // PALMIER_CORE_ERROR_HPP
