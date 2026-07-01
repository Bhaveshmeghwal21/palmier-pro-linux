// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/ByokCredentialManager.cpp — BYOK validate/persist/authorize policy
// (Requirements 9.5, 9.6).

#include "services/ByokCredentialManager.hpp"

#include <utility>

#include "core/Error.hpp"

namespace palmier::services {

ByokCredentialManager::ByokCredentialManager(ByokProviderValidator& validator,
                                             SecretStore& store,
                                             std::string userId)
    : validator_(validator), store_(store), userId_(std::move(userId)) {}

std::string ByokCredentialManager::storageKey(const std::string& provider) const {
    // Namespaced, per-user key so multiple accounts on one machine and multiple
    // providers per account never collide in the secret store.
    return "palmier/byok/" + userId_ + "/" + provider;
}

Result<void> ByokCredentialManager::saveCredential(const ByokCredential& credential) {
    // A credential missing its provider or key can never be validated or filed;
    // reject it up front without contacting the provider.
    if (!credential.isWellFormed()) {
        return err(makeError(
            ErrorCode::InvalidArgument,
            "BYOK credentials require both a provider and a non-empty API key."));
    }

    // 1) Validate with the provider (external call, behind the interface).
    Result<void> validation = validator_.validate(credential);
    if (validation.isError()) {
        const Error& error = validation.error();

        // Requirement 9.6: a provider rejection means the key is invalid — do not
        // persist it, revoke any prior authorization, and indicate clearly.
        if (error.code() == ErrorCode::Unauthenticated) {
            authorizedProviders_.erase(credential.provider);
            return err(makeError(
                ErrorCode::Unauthenticated,
                "The BYOK credentials are invalid: the provider rejected the "
                "supplied API key."));
        }

        // A transport/other failure leaves validity unknown: surface it unchanged
        // and persist nothing (the credential is not proven valid, nor invalid).
        return err(error);
    }

    // 2) Validation succeeded — persist securely (Requirement 9.5). If the secure
    //    store cannot be written we must not mark the credential authorized.
    Result<void> stored = store_.store(storageKey(credential.provider), credential.apiKey);
    if (stored.isError()) {
        authorizedProviders_.erase(credential.provider);
        return stored;
    }

    // 3) Mark the provider authorized so subsequent generative requests using
    //    these credentials are permitted (Requirement 9.5).
    authorizedProviders_.insert(credential.provider);
    return ok();
}

bool ByokCredentialManager::isAuthorized(const std::string& provider) const {
    return authorizedProviders_.find(provider) != authorizedProviders_.end();
}

Result<ByokCredential> ByokCredentialManager::credentialFor(
    const std::string& provider) const {
    if (authorizedProviders_.find(provider) == authorizedProviders_.end()) {
        return err<ByokCredential>(makeError(
            ErrorCode::NotFound,
            "No authorized BYOK credentials are available for provider: " + provider));
    }

    Result<std::optional<std::string>> looked = store_.lookup(storageKey(provider));
    if (looked.isError()) {
        return err<ByokCredential>(std::move(looked).error());
    }

    const std::optional<std::string>& secret = looked.value();
    if (!secret.has_value()) {
        // Authorized in memory but absent from the store: treat as not found so
        // callers re-prompt rather than proceed with a phantom credential.
        return err<ByokCredential>(makeError(
            ErrorCode::NotFound,
            "The stored BYOK credential for provider '" + provider +
                "' could not be found in the secure store."));
    }

    return ByokCredential{provider, *secret};
}

Result<void> ByokCredentialManager::forget(const std::string& provider) {
    authorizedProviders_.erase(provider);
    return store_.remove(storageKey(provider));
}

} // namespace palmier::services
