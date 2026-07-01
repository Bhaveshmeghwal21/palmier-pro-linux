// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/Result.hpp — the error-returning type used across Palmier interfaces.
//
// Fallible operations throughout the domain core, media engine, GPU layer, and
// service layer return Result<T> instead of throwing. A Result<T> holds either a
// value of type T (success) or an Error (failure). Result<void> models fallible
// operations that produce no value. The design's example usage relies on
// `.value()` to extract a success value (e.g. GpuContext::create(policy).value()),
// which this type provides, along with monadic map/andThen helpers.

#ifndef PALMIER_CORE_RESULT_HPP
#define PALMIER_CORE_RESULT_HPP

#include <cassert>
#include <type_traits>
#include <utility>
#include <variant>

#include "core/Error.hpp"

namespace palmier {

/// Tag used to disambiguate the "success with a value" constructor path.
struct OkTag {};
inline constexpr OkTag kOk{};

/// Either a value of type T or an Error. Never both, never neither.
template <typename T>
class [[nodiscard]] Result {
    static_assert(!std::is_same_v<std::decay_t<T>, Error>,
                  "Result<Error> is ambiguous; use Result<void> or a distinct type");

public:
    using value_type = T;

    // Implicit success construction from a value (the common, ergonomic path).
    Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}

    // Implicit failure construction from an Error.
    Result(Error error) : storage_(std::in_place_index<1>, std::move(error)) {}

    [[nodiscard]] bool isOk() const noexcept { return storage_.index() == 0; }
    [[nodiscard]] bool isError() const noexcept { return storage_.index() == 1; }
    explicit operator bool() const noexcept { return isOk(); }

    /// Access the success value. Precondition: isOk().
    [[nodiscard]] T& value() & {
        assert(isOk() && "Result::value() called on an error Result");
        return std::get<0>(storage_);
    }
    [[nodiscard]] const T& value() const& {
        assert(isOk() && "Result::value() called on an error Result");
        return std::get<0>(storage_);
    }
    [[nodiscard]] T&& value() && {
        assert(isOk() && "Result::value() called on an error Result");
        return std::get<0>(std::move(storage_));
    }

    [[nodiscard]] T& operator*() & { return value(); }
    [[nodiscard]] const T& operator*() const& { return value(); }
    [[nodiscard]] T&& operator*() && { return std::move(*this).value(); }

    /// Access the failure value. Precondition: isError().
    [[nodiscard]] const Error& error() const& {
        assert(isError() && "Result::error() called on an ok Result");
        return std::get<1>(storage_);
    }
    [[nodiscard]] Error&& error() && {
        assert(isError() && "Result::error() called on an ok Result");
        return std::get<1>(std::move(storage_));
    }

    /// Return the value if present, otherwise the supplied fallback.
    template <typename U>
    [[nodiscard]] T valueOr(U&& fallback) const& {
        return isOk() ? std::get<0>(storage_) : static_cast<T>(std::forward<U>(fallback));
    }
    template <typename U>
    [[nodiscard]] T valueOr(U&& fallback) && {
        return isOk() ? std::get<0>(std::move(storage_))
                      : static_cast<T>(std::forward<U>(fallback));
    }

    /// Apply `fn` to the contained value, producing Result<U>. Errors pass through.
    template <typename Fn>
    [[nodiscard]] auto map(Fn&& fn) const&
        -> Result<std::invoke_result_t<Fn, const T&>> {
        using U = std::invoke_result_t<Fn, const T&>;
        if (isOk()) return Result<U>(std::forward<Fn>(fn)(std::get<0>(storage_)));
        return Result<U>(std::get<1>(storage_));
    }

    /// Chain a fallible step: `fn` must itself return a Result. Errors pass through.
    template <typename Fn>
    [[nodiscard]] auto andThen(Fn&& fn) const& -> std::invoke_result_t<Fn, const T&> {
        if (isOk()) return std::forward<Fn>(fn)(std::get<0>(storage_));
        return std::invoke_result_t<Fn, const T&>(std::get<1>(storage_));
    }

private:
    std::variant<T, Error> storage_;
};

/// Specialization for operations that succeed with no value.
template <>
class [[nodiscard]] Result<void> {
public:
    using value_type = void;

    /// Success.
    Result() = default;
    Result(OkTag) {}

    /// Failure.
    Result(Error error) : error_(std::move(error)), hasError_(true) {}

    [[nodiscard]] bool isOk() const noexcept { return !hasError_; }
    [[nodiscard]] bool isError() const noexcept { return hasError_; }
    explicit operator bool() const noexcept { return isOk(); }

    [[nodiscard]] const Error& error() const& {
        assert(hasError_ && "Result<void>::error() called on an ok Result");
        return error_;
    }
    [[nodiscard]] Error&& error() && {
        assert(hasError_ && "Result<void>::error() called on an ok Result");
        return std::move(error_);
    }

private:
    Error error_{};
    bool  hasError_ = false;
};

/// Explicit success factory (useful when the value type cannot be deduced).
template <typename T>
[[nodiscard]] Result<std::decay_t<T>> ok(T&& value) {
    return Result<std::decay_t<T>>(std::forward<T>(value));
}
[[nodiscard]] inline Result<void> ok() { return Result<void>{}; }

/// Explicit failure factory.
template <typename T = void>
[[nodiscard]] Result<T> err(Error error) {
    return Result<T>(std::move(error));
}

} // namespace palmier

#endif // PALMIER_CORE_RESULT_HPP
