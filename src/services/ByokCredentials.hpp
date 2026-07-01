// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/ByokCredentials.hpp — the BYOK (Bring Your Own Key) credential type
// and the provider-validation abstraction (design.md "Component 5: Generative
// AI Client & Auth"; Requirements 9.5, 9.6).
//
// A BYOK credential is a user-supplied third-party model-provider secret (e.g.
// an OpenAI / Anthropic / xAI API key) that lets the user drive generative
// features against their own provider account instead of a Palmier subscription.
//
// The actual "is this key accepted by the provider?" check is an external
// network call to the third-party provider, so it is abstracted behind the
// ByokProviderValidator interface. That keeps the save/persist/authorize policy
// (ByokCredentialManager) unit-testable with a scriptable fake and free of any
// network dependency.

#ifndef PALMIER_SERVICES_BYOKCREDENTIALS_HPP
#define PALMIER_SERVICES_BYOKCREDENTIALS_HPP

#include <string>

#include "core/Result.hpp"

namespace palmier::services {

/// A user-supplied third-party model-provider credential.
///
/// `provider` is a stable, lowercase identifier for the model provider the key
/// belongs to (e.g. "openai", "anthropic", "xai"); it selects both the
/// validation endpoint and the storage slot. `apiKey` is the secret itself and
/// is only ever forwarded to the provider for validation or handed to the
/// secure store — it is never logged or written in plaintext.
struct ByokCredential {
    std::string provider; ///< Provider identifier (storage key + routing).
    std::string apiKey;   ///< The secret API key. Handled as sensitive data.

    [[nodiscard]] friend bool operator==(const ByokCredential&,
                                         const ByokCredential&) = default;

    /// A credential is well-formed only when both fields are non-empty; an
    /// empty provider or key can never be validated or stored.
    [[nodiscard]] bool isWellFormed() const noexcept {
        return !provider.empty() && !apiKey.empty();
    }
};

/// Abstraction over the external check that a BYOK credential is accepted by its
/// third-party provider.
///
/// Implementations perform the real (TLS) network call to the provider. The
/// contract the policy layer relies on:
///   * accepted credential  -> Ok();
///   * rejected credential   -> Error with code ErrorCode::Unauthenticated;
///   * transport/other failures (timeout, I/O, ...) -> any other ErrorCode.
///
/// Only an Unauthenticated result means "invalid credential" for the purposes of
/// Requirement 9.6 (reject + discard). A transport failure leaves the outcome
/// unknown and must not be treated as a validation rejection.
class ByokProviderValidator {
public:
    virtual ~ByokProviderValidator() = default;

    [[nodiscard]] virtual Result<void> validate(const ByokCredential& credential) = 0;
};

} // namespace palmier::services

#endif // PALMIER_SERVICES_BYOKCREDENTIALS_HPP
