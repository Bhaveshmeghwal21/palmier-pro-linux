// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/HostedGenerativeBackend.cpp — implementation (task 10.5;
// Requirements 12.1, 12.2, 12.4, 12.6). See the header for the policy.

#include "services/HostedGenerativeBackend.hpp"

#include <optional>
#include <utility>

#include "core/Error.hpp"
#include "services/SecretStore.hpp"

namespace palmier::services {
namespace {

/// The protocol options the hosted API is called with. The hosted service takes
/// its credential as a bearer; no value appears here.
[[nodiscard]] HttpGenerativeJobProtocol::Options protocolOptions(
    const HostedGenerativeBackend::Options& options) {
    HttpGenerativeJobProtocol::Options out;
    out.endpoint = options.endpoint;
    out.scheme = GenerativeAuthScheme::BearerAuthorization;
    return out;
}

}  // namespace

std::string HostedGenerativeBackend::credentialKey(std::string_view userId) {
    // Namespaced per user, in the same shape ByokCredentialManager uses, so one
    // machine with several accounts never collides.
    return "palmier/hosted/" + std::string(userId) + "/session";
}

HostedGenerativeBackend::HostedGenerativeBackend(GenerativeHttpTransport& transport,
                                                 Options options)
    : options_(std::move(options)), protocol_(transport, protocolOptions(options_)) {}

std::string HostedGenerativeBackend::unmetPrecondition() const {
    if (options_.endpoint.baseUrl.empty()) {
        return "generation is unavailable: no reachable network — the hosted generative "
               "endpoint is not configured in this build";
    }
    if (options_.secretStore == nullptr) {
        return "generation is unavailable: no authenticated account — no secret store is "
               "configured, so no hosted account credential can be read";
    }
    const Result<std::optional<std::string>> looked =
        options_.secretStore->lookup(credentialKey(options_.userId));
    if (looked.isError() || !looked.value().has_value() || looked.value()->empty()) {
        return "generation is unavailable: no authenticated account — no hosted account "
               "credential is stored for this user";
    }
    return {};
}

Result<std::string> HostedGenerativeBackend::credentialFor(std::string_view authToken) const {
    if (options_.secretStore != nullptr) {
        const Result<std::optional<std::string>> looked =
            options_.secretStore->lookup(credentialKey(options_.userId));
        if (looked.isOk() && looked.value().has_value() && !looked.value()->empty()) {
            return Result<std::string>(*looked.value());
        }
    }
    if (!authToken.empty()) {
        return Result<std::string>(std::string(authToken));
    }
    return err<std::string>(makeError(
        ErrorCode::Unauthenticated,
        "generation is unavailable: no authenticated account — no hosted account credential "
        "is available, so no request was sent"));
}

Result<GenerativeHttpRequest> HostedGenerativeBackend::buildSubmitRequest(
    const GenerationRequest& request, std::string_view authToken) const {
    Result<std::string> credential = credentialFor(authToken);
    if (credential.isError()) return err<GenerativeHttpRequest>(credential.error());
    return protocol_.submitRequest(request, credential.value());
}

Result<JobId> HostedGenerativeBackend::submit(const GenerationRequest& request,
                                             std::string_view authToken) {
    Result<std::string> credential = credentialFor(authToken);
    if (credential.isError()) return err<JobId>(credential.error());
    return protocol_.submit(request, credential.value());
}

Result<GenerationStatus> HostedGenerativeBackend::poll(const JobId& id,
                                                       std::string_view authToken) {
    Result<std::string> credential = credentialFor(authToken);
    if (credential.isError()) return err<GenerationStatus>(credential.error());
    return protocol_.poll(id, credential.value());
}

Result<MediaAsset> HostedGenerativeBackend::fetchResult(const JobId& id,
                                                       std::string_view authToken) {
    Result<std::string> credential = credentialFor(authToken);
    if (credential.isError()) return err<MediaAsset>(credential.error());
    return protocol_.fetchResult(id, credential.value());
}

Result<void> HostedGenerativeBackend::cancel(const JobId& id, std::string_view authToken) {
    Result<std::string> credential = credentialFor(authToken);
    // A cancel that cannot be authorized cannot have started a job either, so
    // reporting success here would be a lie; report the precondition instead.
    if (credential.isError()) return err<void>(credential.error());
    return protocol_.cancel(id, credential.value());
}

}  // namespace palmier::services
