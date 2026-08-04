// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/ByokGenerativeBackend.cpp — implementation (task 10.5;
// Requirements 12.1, 12.2, 12.4, 12.6). See the header for the policy.

#include "services/ByokGenerativeBackend.hpp"

#include <optional>
#include <utility>

#include "core/Error.hpp"
#include "services/SecretStore.hpp"

namespace palmier::services {
namespace {

[[nodiscard]] HttpGenerativeJobProtocol::Options protocolOptions(
    const ByokGenerativeBackend::Options& options) {
    HttpGenerativeJobProtocol::Options out;
    out.endpoint = options.endpoint;
    out.scheme = GenerativeAuthScheme::ApiKeyHeader;
    out.apiKeyHeaderName = options.apiKeyHeaderName;
    // Naming the provider lets one endpoint route for several. It is a provider
    // NAME, not a key.
    if (!options.provider.empty()) {
        out.extraHeaders.emplace_back("X-Palmier-Provider", options.provider);
    }
    return out;
}

}  // namespace

std::string ByokGenerativeBackend::credentialKey(std::string_view userId,
                                                 std::string_view provider) {
    // Must match ByokCredentialManager::storageKey() for the same user.
    return "palmier/byok/" + std::string(userId) + "/" + std::string(provider);
}

ByokGenerativeBackend::ByokGenerativeBackend(GenerativeHttpTransport& transport,
                                             Options options)
    : options_(std::move(options)), protocol_(transport, protocolOptions(options_)) {}

std::string ByokGenerativeBackend::unmetPrecondition() const {
    if (options_.endpoint.baseUrl.empty()) {
        return "generation is unavailable: no reachable network — no BYOK generative endpoint "
               "is configured in this build";
    }
    if (options_.provider.empty()) {
        return "generation is unavailable: no BYOK credentials — no provider is configured, "
               "so no provider key can be read";
    }
    if (options_.secretStore == nullptr) {
        return "generation is unavailable: no BYOK credentials — no secret store is "
               "configured, so no provider key can be read";
    }
    const Result<std::optional<std::string>> looked =
        options_.secretStore->lookup(credentialKey(options_.userId, options_.provider));
    if (looked.isError() || !looked.value().has_value() || looked.value()->empty()) {
        return "generation is unavailable: no BYOK credentials — no provider key is stored "
               "for provider '" +
               options_.provider + "'";
    }
    return {};
}

Result<std::string> ByokGenerativeBackend::credentialFor() const {
    if (options_.secretStore != nullptr && !options_.provider.empty()) {
        const Result<std::optional<std::string>> looked =
            options_.secretStore->lookup(credentialKey(options_.userId, options_.provider));
        if (looked.isOk() && looked.value().has_value() && !looked.value()->empty()) {
            return Result<std::string>(*looked.value());
        }
    }
    return err<std::string>(makeError(
        ErrorCode::Unauthenticated,
        "generation is unavailable: no BYOK credentials — no provider key is available, so "
        "no request was sent"));
}

Result<GenerativeHttpRequest> ByokGenerativeBackend::buildSubmitRequest(
    const GenerationRequest& request, std::string_view) const {
    Result<std::string> credential = credentialFor();
    if (credential.isError()) return err<GenerativeHttpRequest>(credential.error());
    return protocol_.submitRequest(request, credential.value());
}

Result<JobId> ByokGenerativeBackend::submit(const GenerationRequest& request,
                                           std::string_view) {
    Result<std::string> credential = credentialFor();
    if (credential.isError()) return err<JobId>(credential.error());
    return protocol_.submit(request, credential.value());
}

Result<GenerationStatus> ByokGenerativeBackend::poll(const JobId& id, std::string_view) {
    Result<std::string> credential = credentialFor();
    if (credential.isError()) return err<GenerationStatus>(credential.error());
    return protocol_.poll(id, credential.value());
}

Result<MediaAsset> ByokGenerativeBackend::fetchResult(const JobId& id, std::string_view) {
    Result<std::string> credential = credentialFor();
    if (credential.isError()) return err<MediaAsset>(credential.error());
    return protocol_.fetchResult(id, credential.value());
}

Result<void> ByokGenerativeBackend::cancel(const JobId& id, std::string_view) {
    Result<std::string> credential = credentialFor();
    if (credential.isError()) return err<void>(credential.error());
    return protocol_.cancel(id, credential.value());
}

}  // namespace palmier::services
