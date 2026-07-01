// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/AuthenticationService.hpp — login, session, and subscription
// entitlement handling (design.md "Component 5: Generative AI Client & Auth").
//
// The editor and MCP server are fully usable without logging in (Requirement
// 9.1); authentication only gates the closed-source generative features. This
// component owns the login flow: it establishes an authenticated session and
// reports the account's subscription entitlement (active / expired / none,
// Requirement 9.2), rejects invalid credentials with a clear indication
// (Requirement 9.3), and enforces a brute-force lockout that blocks an account
// for 15 minutes after 5 consecutive failed attempts (Requirement 9.4).
//
// The actual call to the hosted (closed-source) backend is abstracted behind the
// AuthBackend interface so the policy logic here — session tracking, failure
// counting, and lockout timing — is unit-testable with a mock backend and an
// injectable clock. BYOK credential validation and secure storage (task 13.2)
// live in ByokCredentialManager; this service optionally holds a reference to one
// so callers have a single auth entry point for both login and BYOK. The
// generative client itself (task 14.x) remains out of scope for this component.

#ifndef PALMIER_SERVICES_AUTHENTICATIONSERVICE_HPP
#define PALMIER_SERVICES_AUTHENTICATIONSERVICE_HPP

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "core/Result.hpp"
#include "services/ByokCredentials.hpp"

namespace palmier::services {

class ByokCredentialManager;

/// A user's subscription entitlement, as reported by the backend on login.
/// These are exactly the three states Requirement 9.2 requires the service to
/// return: an active subscription, an expired one, or none at all.
enum class EntitlementStatus {
    Active,  ///< A current, paid subscription entitles generative features.
    Expired, ///< A subscription existed but has lapsed.
    None,    ///< The account has no subscription entitlement.
};

/// Stable lowercase name for an EntitlementStatus ("active"/"expired"/"none").
[[nodiscard]] std::string_view toStringView(EntitlementStatus status) noexcept;

/// Plain-text credentials submitted by a user attempting to log in.
struct LoginCredentials {
    std::string username; ///< Account identifier (email/username).
    std::string password; ///< Secret; only forwarded to the backend, never stored.
};

/// The outcome of a successful backend authentication, before it is promoted to
/// a Session. Kept minimal: an opaque session token plus the entitlement.
struct BackendSession {
    std::string       token;                                 ///< Opaque bearer token.
    EntitlementStatus entitlement = EntitlementStatus::None; ///< Subscription state.
};

/// An established, authenticated session. Held by the service after a successful
/// login and handed to the generative client (task 14.x) to authorize requests.
struct Session {
    std::string       username;                              ///< Authenticated account.
    std::string       token;                                 ///< Opaque bearer token.
    EntitlementStatus entitlement = EntitlementStatus::None; ///< Subscription state.
};

/// Abstraction over the hosted (closed-source) authentication backend.
///
/// Implementations perform the real network call. The contract the policy layer
/// relies on:
///   * valid credentials  -> Ok(BackendSession) with the account's entitlement;
///   * invalid credentials -> Error with code ErrorCode::Unauthenticated;
///   * transport/other failures (timeout, I/O, ...) -> any other ErrorCode.
///
/// Only Unauthenticated failures count toward the consecutive-failure lockout;
/// transient transport errors do not (Requirement 9.4 concerns *invalid
/// credentials*, not connectivity problems).
class AuthBackend {
public:
    virtual ~AuthBackend() = default;
    [[nodiscard]] virtual Result<BackendSession> authenticate(
        const LoginCredentials& credentials) = 0;
};

/// Tunable lockout policy. Defaults match Requirement 9.4 (5 failures -> 15 min).
struct LockoutPolicy {
    unsigned                 maxConsecutiveFailures = 5;
    std::chrono::milliseconds lockoutDuration =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::minutes(15));
};

/// Owns the login flow, the current session, and per-account lockout state.
///
/// Thread-affinity: instances are not internally synchronized; callers that
/// share one across threads must provide external synchronization.
class AuthenticationService {
public:
    /// Monotonic clock used for lockout timing. Injectable so tests can advance
    /// time deterministically; defaults to std::chrono::steady_clock.
    using Clock    = std::function<std::chrono::steady_clock::time_point()>;
    using TimePoint = std::chrono::steady_clock::time_point;

    /// `backend` must outlive this service.
    explicit AuthenticationService(AuthBackend& backend,
                                   LockoutPolicy policy = {},
                                   Clock clock = {});

    /// Attempts to establish an authenticated session (Requirements 9.2-9.4).
    ///
    /// Behavior:
    ///   * If the account is currently locked, returns PermissionDenied without
    ///     contacting the backend (Requirement 9.4).
    ///   * On valid credentials, stores and returns the Session carrying the
    ///     account's entitlement, and clears the failure counter (Req 9.2).
    ///   * On invalid credentials, returns Unauthenticated and increments the
    ///     consecutive-failure counter; the Nth (== maxConsecutiveFailures)
    ///     failure locks the account and returns PermissionDenied (Req 9.3/9.4).
    ///   * On a non-credential backend error, propagates it unchanged and does
    ///     not count it toward lockout.
    [[nodiscard]] Result<Session> login(const LoginCredentials& credentials);

    /// Clears the current session (the account remains lockout state as-is).
    void logout() noexcept;

    [[nodiscard]] bool isAuthenticated() const noexcept { return session_.has_value(); }
    [[nodiscard]] const std::optional<Session>& currentSession() const noexcept {
        return session_;
    }

    /// True iff `username` is currently within its lockout window.
    [[nodiscard]] bool isLocked(const std::string& username) const;

    /// Number of consecutive failed attempts recorded for `username` (0 if none
    /// or if the account is not tracked).
    [[nodiscard]] unsigned consecutiveFailures(const std::string& username) const;

    // --- BYOK credential integration (Requirements 9.5, 9.6) -----------------
    //
    // The service can optionally hold a reference to a ByokCredentialManager so
    // that login and BYOK share one auth entry point. Attaching is opt-in and
    // does not change the login/lockout behavior above; when no manager is
    // attached the BYOK helpers return FailedPrecondition.

    /// Attach the BYOK credential manager. `manager` must outlive this service.
    void setByokManager(ByokCredentialManager& manager) noexcept {
        byokManager_ = &manager;
    }

    /// Validate and, on success, securely persist BYOK credentials for the
    /// current user, authorizing subsequent generative requests (Req 9.5); on
    /// provider rejection the credentials are discarded without persisting and an
    /// error is returned (Req 9.6). Returns FailedPrecondition if no manager is
    /// attached.
    [[nodiscard]] Result<void> saveByokCredentials(const ByokCredential& credential);

    /// True iff a validated BYOK credential for `provider` is currently
    /// authorized. False when no manager is attached.
    [[nodiscard]] bool isByokAuthorized(const std::string& provider) const;

private:
    struct AttemptState {
        unsigned                 consecutiveFailures = 0;
        std::optional<TimePoint> lockedUntil;
    };

    AuthBackend&                                  backend_;
    LockoutPolicy                                 policy_;
    Clock                                         clock_;
    std::optional<Session>                        session_;
    std::unordered_map<std::string, AttemptState> attempts_;
    ByokCredentialManager*                        byokManager_ = nullptr;
};

} // namespace palmier::services

#endif // PALMIER_SERVICES_AUTHENTICATIONSERVICE_HPP
