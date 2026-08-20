// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/ByokGenerativeBackend.hpp — the bring-your-own-key provider client
// (task 10.5; Requirements 12.1, 12.2, 12.4, 12.6).
//
// The BYOK client differs from the hosted one in exactly two respects, and both
// are configuration rather than code:
//
//   * WHERE the credential comes from. It is the user's own provider key, filed by
//     `ByokCredentialManager` under `palmier/byok/<user>/<provider>`. This class
//     re-derives that key rather than depending on the manager, so it stays
//     linkable in the service-layer test binaries that do not compile the manager;
//     `credentialKey()` is public so a test pins the derivation to the manager's
//     instead of trusting the comment.
//   * HOW it is presented. Providers overwhelmingly take an API key in a dedicated
//     header rather than as a bearer, so the default scheme is `X-Api-Key` with the
//     header name configurable, and the provider is named in a header so one
//     endpoint can route for several.
//
// Everything else — the job protocol, the error mapping, the read-at-request-time
// credential policy, the injected transport seam — is shared with the hosted
// client and lives in GenerativeHttpTransport.hpp. No provider key value appears
// anywhere in this repository (Requirement 12.6).

#ifndef PALMIER_SERVICES_BYOKGENERATIVEBACKEND_HPP
#define PALMIER_SERVICES_BYOKGENERATIVEBACKEND_HPP

#include <string>
#include <string_view>

#include "core/Result.hpp"
#include "services/GenerativeBackendRegistry.hpp"
#include "services/GenerativeHttpTransport.hpp"

namespace palmier::services {

class SecretStore;  // services/SecretStore.hpp

/// The BYOK provider client. Selected by the configuration id `byok`.
class ByokGenerativeBackend final : public GenerativeBackend {
public:
    struct Options {
        /// Where the provider's job API lives. A location, never a credential.
        GenerativeEndpoint endpoint;

        /// Where the provider key is read from, at request time.
        SecretStore* secretStore = nullptr;

        /// The provider whose stored key authorizes requests. Empty is an unmet
        /// precondition: without a provider there is no key to look up.
        std::string provider;

        /// The secret-store key scope, matching `ByokCredentialManager`'s.
        std::string userId = "default";

        /// The header the key is sent in. Configurable because providers disagree.
        std::string apiKeyHeaderName = "X-Api-Key";
    };

    /// The secret-store key `provider`'s BYOK credential is filed under for
    /// `userId`. This MUST equal `ByokCredentialManager::storageKey(provider)` for
    /// the same user, and a test asserts that it does.
    [[nodiscard]] static std::string credentialKey(std::string_view userId,
                                                   std::string_view provider);

    /// `transport` must outlive this backend.
    ByokGenerativeBackend(GenerativeHttpTransport& transport, Options options);

    // --- GenerativeBackend --------------------------------------------------

    [[nodiscard]] std::string_view backendId() const noexcept override {
        return kGenerativeBackendByok;
    }

    /// Empty iff a provider is named, a secret store is configured, that store
    /// holds a non-empty key for the provider, and the endpoint is a configured
    /// `https://` URL. One secret-store read; no network activity.
    [[nodiscard]] std::string unmetPrecondition() const override;

    // --- IGenerativeBackend -------------------------------------------------

    [[nodiscard]] Result<JobId> submit(const GenerationRequest& request,
                                       std::string_view authToken) override;
    [[nodiscard]] Result<GenerationStatus> poll(const JobId& id,
                                                std::string_view authToken) override;
    [[nodiscard]] Result<MediaAsset> fetchResult(const JobId& id,
                                                 std::string_view authToken) override;
    [[nodiscard]] Result<void> cancel(const JobId& id, std::string_view authToken) override;

    /// The exact request `submit()` would send, without sending it.
    [[nodiscard]] Result<GenerativeHttpRequest> buildSubmitRequest(
        const GenerationRequest& request, std::string_view authToken) const;

private:
    /// The provider key to authorize one request with, read now. Unlike the hosted
    /// client this does NOT fall back to the gate's bearer: a hosted session token
    /// is not a provider key, and sending one to a third-party provider would leak
    /// it to a party that has no business holding it.
    [[nodiscard]] Result<std::string> credentialFor() const;

    Options options_;
    HttpGenerativeJobProtocol protocol_;
};

}  // namespace palmier::services

#endif  // PALMIER_SERVICES_BYOKGENERATIVEBACKEND_HPP
