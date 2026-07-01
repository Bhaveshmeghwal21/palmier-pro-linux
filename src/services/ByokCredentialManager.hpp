// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/ByokCredentialManager.hpp — BYOK credential validation, secure
// persistence, and authorization policy (Requirements 9.5, 9.6).
//
// This is the policy layer that ties together the two abstractions:
//   * ByokProviderValidator — the external "does the provider accept this key?"
//     check (a network call in production, a scriptable fake in tests);
//   * SecretStore           — non-plaintext persistence via the platform Secret
//     Service (libsecret) in production, an in-memory fake in tests.
//
// The save flow implements the two acceptance criteria directly:
//   * 9.5: WHEN the user saves BYOK credentials, validate them and — on success
//          — persist them securely for the current user and mark them usable so
//          subsequent generative requests are authorized.
//   * 9.6: IF the credentials fail validation with the provider, reject and
//          discard them WITHOUT persisting, and surface a clear indication.
//
// A transport/connectivity failure during validation is neither an acceptance
// (we cannot confirm the key) nor a 9.6 rejection (the key was not proven
// invalid): the error is surfaced unchanged and nothing is persisted or marked
// authorized.

#ifndef PALMIER_SERVICES_BYOKCREDENTIALMANAGER_HPP
#define PALMIER_SERVICES_BYOKCREDENTIALMANAGER_HPP

#include <string>
#include <unordered_set>

#include "core/Result.hpp"
#include "services/ByokCredentials.hpp"
#include "services/SecretStore.hpp"

namespace palmier::services {

/// Owns the BYOK save/validate/authorize policy for a single user.
///
/// `validator` and `store` must outlive this manager. `userId` scopes the secret
/// store keys so multiple accounts on one machine keep separate credentials.
class ByokCredentialManager {
public:
    ByokCredentialManager(ByokProviderValidator& validator, SecretStore& store,
                          std::string userId = "default");

    /// Validate `credential` with its provider and, on success, persist it
    /// securely and authorize subsequent generative requests for that provider
    /// (Requirement 9.5). On validation rejection the credential is discarded
    /// without being persisted and an Unauthenticated error is returned
    /// (Requirement 9.6). Malformed input yields InvalidArgument; a transport
    /// failure during validation is propagated unchanged and persists nothing.
    [[nodiscard]] Result<void> saveCredential(const ByokCredential& credential);

    /// True iff a validated credential for `provider` is currently authorized —
    /// i.e. it was saved successfully (9.5) and has not since been forgotten.
    [[nodiscard]] bool isAuthorized(const std::string& provider) const;

    /// Retrieve the stored secret for `provider` so a generative request can be
    /// authorized with it. Returns NotFound when no authorized credential exists
    /// for the provider.
    [[nodiscard]] Result<ByokCredential> credentialFor(const std::string& provider) const;

    /// Forget a provider's credential: remove it from secure storage and revoke
    /// its authorization. Forgetting an unknown provider succeeds (idempotent).
    [[nodiscard]] Result<void> forget(const std::string& provider);

    /// The secret-store key under which `provider`'s credential is filed for this
    /// user. Exposed primarily so tests can assert on persistence without
    /// duplicating the derivation.
    [[nodiscard]] std::string storageKey(const std::string& provider) const;

private:
    ByokProviderValidator&          validator_;
    SecretStore&                    store_;
    std::string                     userId_;
    std::unordered_set<std::string> authorizedProviders_;
};

} // namespace palmier::services

#endif // PALMIER_SERVICES_BYOKCREDENTIALMANAGER_HPP
