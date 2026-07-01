// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/AuthenticationService_test.cpp — unit tests for the login flow, session
// establishment, entitlement reporting, and the consecutive-failure lockout
// policy (Requirements 9.2, 9.3, 9.4).
//
// The hosted backend is abstracted behind AuthBackend; these tests drive the
// policy with a scriptable mock and an injectable clock so lockout timing is
// fully deterministic.

#include "services/AuthenticationService.hpp"

#include <chrono>
#include <functional>
#include <string>

#include <gtest/gtest.h>

#include "core/Error.hpp"

namespace {

using namespace palmier;
using palmier::services::AuthBackend;
using palmier::services::AuthenticationService;
using palmier::services::BackendSession;
using palmier::services::EntitlementStatus;
using palmier::services::LockoutPolicy;
using palmier::services::LoginCredentials;
using palmier::services::Session;

// A backend whose response is decided by an injected callable, so each test can
// script "valid", "invalid credentials", or "transport error" precisely.
class MockBackend : public AuthBackend {
public:
    using Handler = std::function<Result<BackendSession>(const LoginCredentials&)>;

    explicit MockBackend(Handler handler) : handler_(std::move(handler)) {}

    Result<BackendSession> authenticate(const LoginCredentials& c) override {
        ++calls;
        return handler_(c);
    }

    int calls = 0;

private:
    Handler handler_;
};

// A manually advanced steady clock for deterministic lockout timing.
class FakeClock {
public:
    AuthenticationService::TimePoint now() const { return now_; }
    void advance(std::chrono::milliseconds delta) { now_ += delta; }

    AuthenticationService::Clock fn() {
        return [this] { return now(); };
    }

private:
    AuthenticationService::TimePoint now_{};
};

// Convenience handlers.
MockBackend::Handler acceptWith(EntitlementStatus entitlement) {
    return [entitlement](const LoginCredentials&) -> Result<BackendSession> {
        return BackendSession{"token-123", entitlement};
    };
}

MockBackend::Handler rejectInvalid() {
    return [](const LoginCredentials&) -> Result<BackendSession> {
        return err<BackendSession>(
            makeError(ErrorCode::Unauthenticated, "no such account"));
    };
}

const LoginCredentials kCreds{"ada@example.com", "correct horse"};

// --- Requirement 9.2: successful login establishes a session + entitlement ---

TEST(AuthenticationServiceTest, ValidLoginEstablishesSessionWithActiveEntitlement) {
    MockBackend backend(acceptWith(EntitlementStatus::Active));
    AuthenticationService svc(backend);

    Result<Session> result = svc.login(kCreds);

    ASSERT_TRUE(result.isOk());
    EXPECT_EQ(result.value().username, kCreds.username);
    EXPECT_EQ(result.value().token, "token-123");
    EXPECT_EQ(result.value().entitlement, EntitlementStatus::Active);
    EXPECT_TRUE(svc.isAuthenticated());
}

TEST(AuthenticationServiceTest, EntitlementStatusIsReportedVerbatim) {
    for (auto status : {EntitlementStatus::Active, EntitlementStatus::Expired,
                        EntitlementStatus::None}) {
        MockBackend backend(acceptWith(status));
        AuthenticationService svc(backend);
        Result<Session> result = svc.login(kCreds);
        ASSERT_TRUE(result.isOk());
        EXPECT_EQ(result.value().entitlement, status);
    }
}

TEST(AuthenticationServiceTest, EntitlementStatusStringNames) {
    EXPECT_EQ(palmier::services::toStringView(EntitlementStatus::Active), "active");
    EXPECT_EQ(palmier::services::toStringView(EntitlementStatus::Expired), "expired");
    EXPECT_EQ(palmier::services::toStringView(EntitlementStatus::None), "none");
}

TEST(AuthenticationServiceTest, LogoutClearsSession) {
    MockBackend backend(acceptWith(EntitlementStatus::Active));
    AuthenticationService svc(backend);
    ASSERT_TRUE(svc.login(kCreds).isOk());

    svc.logout();

    EXPECT_FALSE(svc.isAuthenticated());
    EXPECT_FALSE(svc.currentSession().has_value());
}

// --- Requirement 9.3: invalid credentials are rejected with an indication ----

TEST(AuthenticationServiceTest, InvalidCredentialsRejectedAndSessionUnauthenticated) {
    MockBackend backend(rejectInvalid());
    AuthenticationService svc(backend);

    Result<Session> result = svc.login(kCreds);

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::Unauthenticated);
    EXPECT_FALSE(result.error().message().empty());
    EXPECT_FALSE(svc.isAuthenticated());
}

// --- Requirement 9.4: lock the account after 5 consecutive failures ----------

TEST(AuthenticationServiceTest, FifthConsecutiveFailureLocksAccount) {
    MockBackend backend(rejectInvalid());
    FakeClock clock;
    AuthenticationService svc(backend, LockoutPolicy{}, clock.fn());

    // First four failures report invalid credentials (not locked yet).
    for (int i = 0; i < 4; ++i) {
        Result<Session> r = svc.login(kCreds);
        ASSERT_TRUE(r.isError());
        EXPECT_EQ(r.error().code(), ErrorCode::Unauthenticated) << "attempt " << i;
        EXPECT_FALSE(svc.isLocked(kCreds.username));
    }

    // The fifth invalid attempt trips the lockout.
    Result<Session> fifth = svc.login(kCreds);
    ASSERT_TRUE(fifth.isError());
    EXPECT_EQ(fifth.error().code(), ErrorCode::PermissionDenied);
    EXPECT_TRUE(svc.isLocked(kCreds.username));
}

TEST(AuthenticationServiceTest, WhileLockedBackendIsNotContacted) {
    MockBackend backend(rejectInvalid());
    FakeClock clock;
    AuthenticationService svc(backend, LockoutPolicy{}, clock.fn());

    for (int i = 0; i < 5; ++i) {
        (void)svc.login(kCreds);
    }
    const int callsAtLock = backend.calls;

    // A further attempt during the lockout window is refused without a backend call.
    Result<Session> blocked = svc.login(kCreds);
    ASSERT_TRUE(blocked.isError());
    EXPECT_EQ(blocked.error().code(), ErrorCode::PermissionDenied);
    EXPECT_EQ(backend.calls, callsAtLock);
}

TEST(AuthenticationServiceTest, LockoutExpiresAfterFifteenMinutes) {
    MockBackend backend(rejectInvalid());
    FakeClock clock;
    AuthenticationService svc(backend, LockoutPolicy{}, clock.fn());

    for (int i = 0; i < 5; ++i) {
        (void)svc.login(kCreds);
    }
    ASSERT_TRUE(svc.isLocked(kCreds.username));

    // Just before the window elapses: still locked.
    clock.advance(std::chrono::minutes(15) - std::chrono::milliseconds(1));
    EXPECT_TRUE(svc.isLocked(kCreds.username));

    // After 15 minutes the account is no longer locked and attempts resume.
    clock.advance(std::chrono::milliseconds(1));
    EXPECT_FALSE(svc.isLocked(kCreds.username));

    Result<Session> afterExpiry = svc.login(kCreds);
    ASSERT_TRUE(afterExpiry.isError());
    EXPECT_EQ(afterExpiry.error().code(), ErrorCode::Unauthenticated);
}

TEST(AuthenticationServiceTest, SuccessfulLoginResetsFailureCounter) {
    // Reject the first four attempts, then accept.
    int attempt = 0;
    MockBackend backend([&attempt](const LoginCredentials&) -> Result<BackendSession> {
        if (attempt++ < 4) {
            return err<BackendSession>(
                makeError(ErrorCode::Unauthenticated, "invalid"));
        }
        return BackendSession{"tok", EntitlementStatus::Active};
    });
    FakeClock clock;
    AuthenticationService svc(backend, LockoutPolicy{}, clock.fn());

    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(svc.login(kCreds).isError());
    }
    ASSERT_EQ(svc.consecutiveFailures(kCreds.username), 4u);

    ASSERT_TRUE(svc.login(kCreds).isOk());
    EXPECT_EQ(svc.consecutiveFailures(kCreds.username), 0u);
}

TEST(AuthenticationServiceTest, TransportErrorsDoNotCountTowardLockout) {
    MockBackend backend([](const LoginCredentials&) -> Result<BackendSession> {
        return err<BackendSession>(makeError(ErrorCode::Timeout, "network timeout"));
    });
    FakeClock clock;
    AuthenticationService svc(backend, LockoutPolicy{}, clock.fn());

    for (int i = 0; i < 10; ++i) {
        Result<Session> r = svc.login(kCreds);
        ASSERT_TRUE(r.isError());
        EXPECT_EQ(r.error().code(), ErrorCode::Timeout);
    }

    EXPECT_FALSE(svc.isLocked(kCreds.username));
    EXPECT_EQ(svc.consecutiveFailures(kCreds.username), 0u);
}

} // namespace
