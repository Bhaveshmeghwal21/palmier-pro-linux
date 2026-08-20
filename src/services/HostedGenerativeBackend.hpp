// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/HostedGenerativeBackend.hpp — the hosted Palmier service client
// (task 10.5; Requirements 12.1, 12.2, 12.4, 12.6).
//
// This is GPLv3 CLIENT code, and only client code. Requirement 12.6 keeps the
// hosted service implementation out of this repository and forbids any
// hosted-service credential value from appearing in it, so what lives here is:
//
//   * the request shapes the hosted job API is called with;
//   * the credential LOOKUP — a `SecretStore` key derivation plus a read
//     performed at request time, never a value;
//   * the mapping of a response, or a transport failure, onto one `ErrorCode`.
//
// The credential is read on EVERY call rather than captured at construction. That
// is not caution for its own sake: a session that expires, or a user who signs
// out mid-session, must stop authorizing requests without the composition root
// being rebuilt, and a value cached in this object would outlive the fact. It
// also means this class holds no secret between calls, so a core dump or a log of
// its state cannot leak one.
//
// The network itself is the injected `GenerativeHttpTransport` seam, which is why
// every branch above can be exercised with no endpoint and no TLS. See
// GenerativeHttpTransport.hpp for the rationale.

#ifndef PALMIER_SERVICES_HOSTEDGENERATIVEBACKEND_HPP
#define PALMIER_SERVICES_HOSTEDGENERATIVEBACKEND_HPP

#include <string>
#include <string_view>

#include "core/Result.hpp"
#include "services/GenerativeBackendRegistry.hpp"
#include "services/GenerativeHttpTransport.hpp"

namespace palmier::services {

class SecretStore;  // services/SecretStore.hpp

/// The hosted service client. Selected by the configuration id `hosted`.
class HostedGenerativeBackend final : public GenerativeBackend {
public:
    struct Options {
        /// Where the hosted job API lives. A location, never a credential; empty
        /// means unconfigured, which `unmetPrecondition()` reports.
        GenerativeEndpoint endpoint;

        /// Where the hosted session credential is read from, at request time. Null
        /// means no store, which is itself an unmet precondition.
        SecretStore* secretStore = nullptr;

        /// The secret-store key scope, matching `ByokCredentialManager`'s.
        std::string userId = "default";
    };

    /// The secret-store key the hosted session credential is filed under for
    /// `userId`. A key NAME, deliberately exposed so a test can seed a store
    /// without duplicating the derivation — and so that what this build reads is
    /// documented rather than implied.
    [[nodiscard]] static std::string credentialKey(std::string_view userId);

    /// `transport` must outlive this backend.
    HostedGenerativeBackend(GenerativeHttpTransport& transport, Options options);

    // --- GenerativeBackend --------------------------------------------------

    [[nodiscard]] std::string_view backendId() const noexcept override {
        return kGenerativeBackendHosted;
    }

    /// Empty iff a secret store is configured, it holds a non-empty hosted
    /// credential, and the endpoint is a configured `https://` URL. Performs one
    /// secret-store read and no network activity.
    [[nodiscard]] std::string unmetPrecondition() const override;

    // --- IGenerativeBackend -------------------------------------------------
    //
    // `authToken` is the bearer the entitlement gate resolved for this request. It
    // is used only as a fallback when the secret store holds nothing, so a caller
    // that already has a session bearer is not forced to file it first.

    [[nodiscard]] Result<JobId> submit(const GenerationRequest& request,
                                       std::string_view authToken) override;
    [[nodiscard]] Result<GenerationStatus> poll(const JobId& id,
                                                std::string_view authToken) override;
    [[nodiscard]] Result<MediaAsset> fetchResult(const JobId& id,
                                                 std::string_view authToken) override;
    [[nodiscard]] Result<void> cancel(const JobId& id, std::string_view authToken) override;

    /// The exact request `submit()` would send, without sending it. This is the
    /// seam a test uses to pin down request construction and credential placement.
    [[nodiscard]] Result<GenerativeHttpRequest> buildSubmitRequest(
        const GenerationRequest& request, std::string_view authToken) const;

private:
    /// The credential to authorize one request with: the stored hosted credential
    /// when present, else `authToken`, else an `Unauthenticated` error naming the
    /// unmet precondition. Reads the store; contacts nothing.
    [[nodiscard]] Result<std::string> credentialFor(std::string_view authToken) const;

    Options options_;
    HttpGenerativeJobProtocol protocol_;
};

}  // namespace palmier::services

#endif  // PALMIER_SERVICES_HOSTEDGENERATIVEBACKEND_HPP
