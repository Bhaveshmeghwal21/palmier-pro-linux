// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/Subscription.hpp — an RAII handle for a registered change observer.
//
// TimelineEngine::observe(callback) registers a callback and hands back a
// Subscription (design.md Component 1). The Subscription owns the registration:
// while it is alive the callback keeps receiving ChangeSet events; when it is
// destroyed (or reset()) the callback is unregistered. This ties observer
// lifetime to a scope/RAII object rather than requiring an explicit
// "removeObserver" call, so a UI view or MCP session simply holds its
// Subscription for as long as it wants updates.
//
// The handle is intentionally decoupled from the engine: it stores an opaque
// "unsubscribe" thunk supplied by whoever created it. The engine builds that
// thunk from a weak reference to its observer registry, so a Subscription that
// outlives its engine unregisters harmlessly (the weak reference simply fails to
// lock). Subscriptions are move-only — a registration has a single owner.

#ifndef PALMIER_CORE_SUBSCRIPTION_HPP
#define PALMIER_CORE_SUBSCRIPTION_HPP

#include <functional>
#include <utility>

namespace palmier {

class Subscription {
public:
    /// An empty, inactive subscription that unregisters nothing.
    Subscription() noexcept = default;

    /// Wrap an unsubscribe thunk. `unsubscribe` is invoked exactly once, when the
    /// Subscription is reset or destroyed. It must be safe to call even if the
    /// object it targets has since been destroyed (the engine guarantees this via
    /// a weak reference).
    explicit Subscription(std::function<void()> unsubscribe) noexcept
        : unsubscribe_(std::move(unsubscribe)) {}

    Subscription(Subscription&& other) noexcept
        : unsubscribe_(std::move(other.unsubscribe_)) {
        other.unsubscribe_ = nullptr;
    }

    Subscription& operator=(Subscription&& other) noexcept {
        if (this != &other) {
            reset();
            unsubscribe_ = std::move(other.unsubscribe_);
            other.unsubscribe_ = nullptr;
        }
        return *this;
    }

    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;

    ~Subscription() { reset(); }

    /// Unregister now (idempotent). After reset() the subscription is inactive.
    void reset() noexcept {
        if (unsubscribe_) {
            // Move out first so a re-entrant reset() during the thunk is a no-op.
            std::function<void()> thunk = std::move(unsubscribe_);
            unsubscribe_ = nullptr;
            thunk();
        }
    }

    /// True while this handle still holds a live registration.
    [[nodiscard]] bool active() const noexcept { return static_cast<bool>(unsubscribe_); }

private:
    std::function<void()> unsubscribe_;
};

} // namespace palmier

#endif // PALMIER_CORE_SUBSCRIPTION_HPP
