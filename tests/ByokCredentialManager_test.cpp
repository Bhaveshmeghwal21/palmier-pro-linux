// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/ByokCredentialManager_test.cpp — unit tests for BYOK credential
// validation, secure persistence, and authorization (Requirements 9.5, 9.6),
// plus the AuthenticationService BYOK integration.
//
// The external provider check is driven through a scriptable ByokProviderValidator
// fake, and persistence uses the header-only InMemorySecretStore, so these tests
// run without a network connection or a Secret Service daemon.

#include "services/ByokCredentialManager.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "core/Error.hpp"
#include "services/AuthenticationService.hpp"
#include "services/ByokCredentials.hpp"
#include "services/SecretStore.hpp"

namespace {

using namespace palmier;
using palmier::services::ByokCredential;
using palmier::services::ByokCredentialManager;
using palmier::services::ByokProviderValidator;
using palmier::services::InMemorySecretStore;
using palmier::services::SecretStore;

// A validator whose verdict is decided by an injected callable, so each test can
// script "accepted", "rejected (invalid)", or "transport error" precisely.
class FakeValidator : public ByokProviderValidator {
public:
    using Handler = std::function<Result<void>(const ByokCredential&)>;

    explicit FakeValidator(Handler handler) : handler_(std::move(handler)) {}

    Result<void> validate(const ByokCredential& c) override {
        ++calls;
        lastSeen = c;
        return handler_(c);
    }

    int            calls = 0;
    ByokCredential lastSeen;

private:
    Handler handler_;
};

// A secret store whose store() always fails, to exercise the "validated but
// could not persist" path.
class FailingSecretStore final : public SecretStore {
public:
    Result<void> store(const std::string&, const std::string&) override {
        return err(makeError(ErrorCode::Io, "disk full"));
    }
    Result<std::optional<std::string>> lookup(const std::string&) const override {
        return Result<std::optional<std::string>>(std::optional<std::string>{});
    }
    Result<void> remove(const std::string&) override { return ok(); }
};

FakeValidator::Handler accept() {
    return [](const ByokCredential&) -> Result<void> { return ok(); };
}

FakeValidator::Handler rejectInvalid() {
    return [](const ByokCredential&) -> Result<void> {
        return err(makeError(ErrorCode::Unauthenticated, "provider rejected key"));
    };
}

FakeValidator::Handler transportError() {
    return [](const ByokCredential&) -> Result<void> {
        return err(makeError(ErrorCode::Timeout, "provider unreachable"));
    };
}

const ByokCredential kCred{"openai", "sk-secret-123"};

// --- Requirement 9.5: valid credentials are validated, persisted, authorized --

TEST(ByokCredentialManagerTest, ValidCredentialIsPersistedAndAuthorized) {
    FakeValidator validator(accept());
    InMemorySecretStore store;
    ByokCredentialManager mgr(validator, store);

    Result<void> result = mgr.saveCredential(kCred);

    ASSERT_TRUE(result.isOk());
    EXPECT_EQ(validator.calls, 1);
    EXPECT_TRUE(mgr.isAuthorized(kCred.provider));
    // The secret is persisted under the per-user, per-provider key.
    EXPECT_TRUE(store.contains(mgr.storageKey(kCred.provider)));
    EXPECT_EQ(store.size(), 1u);
}

TEST(ByokCredentialManagerTest, AuthorizedCredentialIsRetrievableForSubsequentRequests) {
    FakeValidator validator(accept());
    InMemorySecretStore store;
    ByokCredentialManager mgr(validator, store);
    ASSERT_TRUE(mgr.saveCredential(kCred).isOk());

    Result<ByokCredential> fetched = mgr.credentialFor(kCred.provider);

    ASSERT_TRUE(fetched.isOk());
    EXPECT_EQ(fetched.value().provider, kCred.provider);
    EXPECT_EQ(fetched.value().apiKey, kCred.apiKey);
}

TEST(ByokCredentialManagerTest, CredentialForUnauthorizedProviderIsNotFound) {
    FakeValidator validator(accept());
    InMemorySecretStore store;
    ByokCredentialManager mgr(validator, store);

    Result<ByokCredential> fetched = mgr.credentialFor("anthropic");

    ASSERT_TRUE(fetched.isError());
    EXPECT_EQ(fetched.error().code(), ErrorCode::NotFound);
}

TEST(ByokCredentialManagerTest, PerUserPerProviderKeysDoNotCollide) {
    FakeValidator validator(accept());
    InMemorySecretStore store;
    ByokCredentialManager alice(validator, store, "alice");
    ByokCredentialManager bob(validator, store, "bob");

    ASSERT_TRUE(alice.saveCredential(kCred).isOk());
    ASSERT_TRUE(bob.saveCredential(ByokCredential{"openai", "sk-bob"}).isOk());

    EXPECT_NE(alice.storageKey("openai"), bob.storageKey("openai"));
    EXPECT_EQ(store.size(), 2u);
    EXPECT_EQ(alice.credentialFor("openai").value().apiKey, "sk-secret-123");
    EXPECT_EQ(bob.credentialFor("openai").value().apiKey, "sk-bob");
}

// --- Requirement 9.6: invalid credentials are rejected and discarded ----------

TEST(ByokCredentialManagerTest, InvalidCredentialIsRejectedAndNotPersisted) {
    FakeValidator validator(rejectInvalid());
    InMemorySecretStore store;
    ByokCredentialManager mgr(validator, store);

    Result<void> result = mgr.saveCredential(kCred);

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::Unauthenticated);
    EXPECT_FALSE(result.error().message().empty());   // clear indication
    EXPECT_FALSE(mgr.isAuthorized(kCred.provider));    // not authorized
    EXPECT_EQ(store.size(), 0u);                       // discarded, nothing stored
}

TEST(ByokCredentialManagerTest, RejectionRevokesPreviouslyAuthorizedProvider) {
    // First a valid save authorizes the provider; a later rejected save (e.g.
    // the user pastes a bad key over a good one) must revoke authorization.
    bool acceptFirst = true;
    FakeValidator validator([&acceptFirst](const ByokCredential&) -> Result<void> {
        if (acceptFirst) return ok();
        return err(makeError(ErrorCode::Unauthenticated, "bad key"));
    });
    InMemorySecretStore store;
    ByokCredentialManager mgr(validator, store);

    ASSERT_TRUE(mgr.saveCredential(kCred).isOk());
    ASSERT_TRUE(mgr.isAuthorized(kCred.provider));

    acceptFirst = false;
    Result<void> second = mgr.saveCredential(ByokCredential{"openai", "sk-bad"});
    ASSERT_TRUE(second.isError());
    EXPECT_EQ(second.error().code(), ErrorCode::Unauthenticated);
    EXPECT_FALSE(mgr.isAuthorized(kCred.provider));
}

TEST(ByokCredentialManagerTest, EmptyProviderOrKeyIsRejectedWithoutContactingProvider) {
    FakeValidator validator(accept());
    InMemorySecretStore store;
    ByokCredentialManager mgr(validator, store);

    Result<void> noProvider = mgr.saveCredential(ByokCredential{"", "sk-x"});
    Result<void> noKey      = mgr.saveCredential(ByokCredential{"openai", ""});

    ASSERT_TRUE(noProvider.isError());
    EXPECT_EQ(noProvider.error().code(), ErrorCode::InvalidArgument);
    ASSERT_TRUE(noKey.isError());
    EXPECT_EQ(noKey.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(validator.calls, 0);  // never validated
    EXPECT_EQ(store.size(), 0u);    // never stored
}

// --- Transport failures: unknown validity, persist nothing, propagate ---------

TEST(ByokCredentialManagerTest, TransportErrorDuringValidationPersistsNothing) {
    FakeValidator validator(transportError());
    InMemorySecretStore store;
    ByokCredentialManager mgr(validator, store);

    Result<void> result = mgr.saveCredential(kCred);

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::Timeout);  // propagated unchanged
    EXPECT_FALSE(mgr.isAuthorized(kCred.provider));
    EXPECT_EQ(store.size(), 0u);
}

// --- Persistence failure after a successful validation ------------------------

TEST(ByokCredentialManagerTest, SecureStoreFailureLeavesProviderUnauthorized) {
    FakeValidator validator(accept());
    FailingSecretStore store;
    ByokCredentialManager mgr(validator, store);

    Result<void> result = mgr.saveCredential(kCred);

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::Io);
    EXPECT_FALSE(mgr.isAuthorized(kCred.provider));
}

// --- forget() revokes authorization and clears storage ------------------------

TEST(ByokCredentialManagerTest, ForgetRevokesAuthorizationAndRemovesSecret) {
    FakeValidator validator(accept());
    InMemorySecretStore store;
    ByokCredentialManager mgr(validator, store);
    ASSERT_TRUE(mgr.saveCredential(kCred).isOk());
    ASSERT_TRUE(mgr.isAuthorized(kCred.provider));

    Result<void> result = mgr.forget(kCred.provider);

    ASSERT_TRUE(result.isOk());
    EXPECT_FALSE(mgr.isAuthorized(kCred.provider));
    EXPECT_FALSE(store.contains(mgr.storageKey(kCred.provider)));
}

TEST(ByokCredentialManagerTest, ForgetUnknownProviderSucceeds) {
    FakeValidator validator(accept());
    InMemorySecretStore store;
    ByokCredentialManager mgr(validator, store);

    EXPECT_TRUE(mgr.forget("never-seen").isOk());
}

// --- makeSystemSecretStore stub (built without libsecret in this test binary) -

TEST(SecretStoreTest, SystemSecretStoreUnavailableWithoutLibsecret) {
    // This test binary is compiled without PALMIER_HAVE_LIBSECRET, so the factory
    // reports Unsupported rather than silently persisting insecurely.
    Result<std::unique_ptr<SecretStore>> store = palmier::services::makeSystemSecretStore();
    ASSERT_TRUE(store.isError());
    EXPECT_EQ(store.error().code(), ErrorCode::Unsupported);
}

// --- AuthenticationService BYOK integration -----------------------------------

class NullAuthBackend : public palmier::services::AuthBackend {
public:
    Result<palmier::services::BackendSession> authenticate(
        const palmier::services::LoginCredentials&) override {
        return err<palmier::services::BackendSession>(
            makeError(ErrorCode::Unauthenticated, "unused"));
    }
};

TEST(AuthenticationServiceByokTest, SaveByokDelegatesToAttachedManager) {
    NullAuthBackend backend;
    palmier::services::AuthenticationService svc(backend);
    FakeValidator validator(accept());
    InMemorySecretStore store;
    ByokCredentialManager mgr(validator, store);
    svc.setByokManager(mgr);

    Result<void> result = svc.saveByokCredentials(kCred);

    ASSERT_TRUE(result.isOk());
    EXPECT_TRUE(svc.isByokAuthorized(kCred.provider));
}

TEST(AuthenticationServiceByokTest, SaveByokWithoutManagerReportsFailedPrecondition) {
    NullAuthBackend backend;
    palmier::services::AuthenticationService svc(backend);

    Result<void> result = svc.saveByokCredentials(kCred);

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::FailedPrecondition);
    EXPECT_FALSE(svc.isByokAuthorized(kCred.provider));
}

TEST(AuthenticationServiceByokTest, InvalidByokRejectedThroughService) {
    NullAuthBackend backend;
    palmier::services::AuthenticationService svc(backend);
    FakeValidator validator(rejectInvalid());
    InMemorySecretStore store;
    ByokCredentialManager mgr(validator, store);
    svc.setByokManager(mgr);

    Result<void> result = svc.saveByokCredentials(kCred);

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::Unauthenticated);
    EXPECT_FALSE(svc.isByokAuthorized(kCred.provider));
    EXPECT_EQ(store.size(), 0u);
}

} // namespace
