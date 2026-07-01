// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/AuthenticationService.cpp — login flow, session tracking, and the
// consecutive-failure lockout policy (Requirements 9.2, 9.3, 9.4).

#include "services/AuthenticationService.hpp"

#include <utility>

#include "core/Error.hpp"
#include "services/ByokCredentialManager.hpp"

namespace palmier::services {

std::string_view toStringView(EntitlementStatus status) noexcept {
    switch (status) {
        case EntitlementStatus::Active:  return "active";
        case EntitlementStatus::Expired: return "expired";
        case EntitlementStatus::None:    return "none";
    }
    return "none";
}

AuthenticationService::AuthenticationService(AuthBackend& backend,
                                             LockoutPolicy policy,
                                             Clock clock)
    : backend_(backend),
      policy_(policy),
      clock_(clock ? std::move(clock)
                   : Clock{[] { return std::chrono::steady_clock::now(); }}) {}

bool AuthenticationService::isLocked(const std::string& username) const {
    const auto it = attempts_.find(username);
    if (it == attempts_.end() || !it->second.lockedUntil.has_value()) {
        return false;
    }
    return clock_() < *it->second.lockedUntil;
}

unsigned AuthenticationService::consecutiveFailures(const std::string& username) const {
    const auto it = attempts_.find(username);
    return it == attempts_.end() ? 0u : it->second.consecutiveFailures;
}

Result<Session> AuthenticationService::login(const LoginCredentials& credentials) {
    const TimePoint now = clock_();
    AttemptState& state = attempts_[credentials.username];

    // A lockout window that has elapsed is cleared before this attempt so the
    // user gets a fresh set of tries once the 15 minutes are up (Req 9.4).
    if (state.lockedUntil.has_value()) {
        if (now < *state.lockedUntil) {
            return err<Session>(makeError(
                ErrorCode::PermissionDenied,
                "This account is temporarily locked after too many failed login "
                "attempts. Try again later."));
        }
        state.lockedUntil.reset();
        state.consecutiveFailures = 0;
    }

    Result<BackendSession> authResult = backend_.authenticate(credentials);

    if (authResult.isError()) {
        const Error& error = authResult.error();

        // Only invalid-credential failures count toward the lockout threshold.
        // Transport/other backend failures are surfaced unchanged and do not
        // penalize the account (Requirement 9.4 concerns invalid credentials).
        if (error.code() != ErrorCode::Unauthenticated) {
            return err<Session>(error);
        }

        ++state.consecutiveFailures;
        if (state.consecutiveFailures >= policy_.maxConsecutiveFailures) {
            state.lockedUntil = now + policy_.lockoutDuration;
            return err<Session>(makeError(
                ErrorCode::PermissionDenied,
                "This account is temporarily locked after too many failed login "
                "attempts. Try again later."));
        }

        return err<Session>(makeError(
            ErrorCode::Unauthenticated,
            "The credentials provided are invalid."));
    }

    // Success: reset the failure state and establish the session (Req 9.2).
    state.consecutiveFailures = 0;
    state.lockedUntil.reset();

    BackendSession backendSession = std::move(authResult).value();
    session_ = Session{credentials.username,
                       std::move(backendSession.token),
                       backendSession.entitlement};
    return *session_;
}

void AuthenticationService::logout() noexcept { session_.reset(); }

Result<void> AuthenticationService::saveByokCredentials(const ByokCredential& credential) {
    if (byokManager_ == nullptr) {
        return err(makeError(
            ErrorCode::FailedPrecondition,
            "BYOK credential storage is not configured for this session."));
    }
    return byokManager_->saveCredential(credential);
}

bool AuthenticationService::isByokAuthorized(const std::string& provider) const {
    return byokManager_ != nullptr && byokManager_->isAuthorized(provider);
}

} // namespace palmier::services
