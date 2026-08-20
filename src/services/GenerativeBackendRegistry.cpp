// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/GenerativeBackendRegistry.cpp — implementation (task 10.5;
// Requirements 12.1, 12.2, 12.4, 12.8). See the header for the policy.

#include "services/GenerativeBackendRegistry.hpp"

#include <algorithm>
#include <utility>

#include "core/Error.hpp"
#include "services/ByokGenerativeBackend.hpp"
#include "services/HostedGenerativeBackend.hpp"

namespace palmier::services {
namespace {

/// The generic reason, used when the composition root had nothing more specific
/// to say (no backend was configured at all).
constexpr std::string_view kUnconfiguredReason =
    "generation is unavailable: no generative backend is configured — no reachable network, "
    "no authenticated account and no BYOK credentials";

/// The offline stub (Requirements 12.1, 12.4, 12.5).
///
/// It holds no transport, no endpoint and no credential — not as a simplification
/// but as the proof obligation: a class with no way to reach the network cannot
/// reach it, which is what makes "without attempting a network connection"
/// structural rather than merely intended. Every rejection is a string comparison
/// and an allocation, so the 1-second bound of Requirement 12.4 is met by orders
/// of magnitude and cannot be missed under load.
class OfflineGenerativeBackend final : public GenerativeBackend {
public:
    explicit OfflineGenerativeBackend(std::string reason)
        : reason_(reason.empty() ? std::string(kUnconfiguredReason) : std::move(reason)) {}

    [[nodiscard]] std::string_view backendId() const noexcept override {
        return kGenerativeBackendOffline;
    }

    [[nodiscard]] std::string unmetPrecondition() const override { return reason_; }

    [[nodiscard]] Result<JobId> submit(const GenerationRequest&, std::string_view) override {
        return err<JobId>(rejection());
    }
    [[nodiscard]] Result<GenerationStatus> poll(const JobId&, std::string_view) override {
        return err<GenerationStatus>(rejection());
    }
    [[nodiscard]] Result<MediaAsset> fetchResult(const JobId&, std::string_view) override {
        return err<MediaAsset>(rejection());
    }
    [[nodiscard]] Result<void> cancel(const JobId&, std::string_view) override {
        // A job that was never submitted is not running, which is what cancelling
        // asks for. Succeeding here also keeps a timeout path that cancels
        // best-effort from turning one failure into two.
        return ok();
    }

private:
    /// `FailedPrecondition` rather than `Unsupported`: the capability exists in this
    /// build, its precondition is unmet, and Requirement 12.4 requires that
    /// precondition to be named in the message.
    [[nodiscard]] Error rejection() const {
        return makeError(ErrorCode::FailedPrecondition, reason_);
    }

    std::string reason_;
};

/// The one line `startupErrors()` carries when a requested id was not installed
/// (Requirement 12.8). Names the rejected id and the unmet requirement, and
/// nothing else — in particular no credential value, which this function never
/// sees.
[[nodiscard]] std::string fallbackNotice(std::string_view rejected, std::string_view reason) {
    return "generative backend '" + std::string(rejected) + "' was not installed (" +
           std::string(reason) + "); the offline stub backend is in force instead, and every "
                                 "non-generation capability remains available";
}

/// The transport a selected HTTPS client is handed. A null injection means this
/// build has no HTTPS client, which is reported per request rather than at
/// selection time — the id is still selectable, so the configuration is honoured
/// and the diagnostic is the documented one.
[[nodiscard]] GenerativeHttpTransport& transportFor(
    const GenerativeBackendRequest& request,
    std::unique_ptr<GenerativeHttpTransport>& owned) {
    if (request.transport != nullptr) return *request.transport;
    owned = makeUnavailableGenerativeHttpTransport();
    return *owned;
}

/// An HTTPS client that owns its fallback transport when one had to be created.
/// Composition rather than inheritance: the client is final, and the only thing
/// that needs extending is the lifetime of the transport beneath it.
template <typename Client>
class OwningTransportBackend final : public GenerativeBackend {
public:
    OwningTransportBackend(std::unique_ptr<GenerativeHttpTransport> transport,
                           GenerativeHttpTransport& used, typename Client::Options options)
        : transport_(std::move(transport)), client_(used, std::move(options)) {}

    [[nodiscard]] std::string_view backendId() const noexcept override {
        return client_.backendId();
    }
    [[nodiscard]] std::string unmetPrecondition() const override {
        return client_.unmetPrecondition();
    }
    [[nodiscard]] Result<JobId> submit(const GenerationRequest& request,
                                       std::string_view authToken) override {
        return client_.submit(request, authToken);
    }
    [[nodiscard]] Result<GenerationStatus> poll(const JobId& id,
                                                std::string_view authToken) override {
        return client_.poll(id, authToken);
    }
    [[nodiscard]] Result<MediaAsset> fetchResult(const JobId& id,
                                                 std::string_view authToken) override {
        return client_.fetchResult(id, authToken);
    }
    [[nodiscard]] Result<void> cancel(const JobId& id, std::string_view authToken) override {
        return client_.cancel(id, authToken);
    }

private:
    std::unique_ptr<GenerativeHttpTransport> transport_;  ///< May be null (injected case).
    Client client_;
};

}  // namespace

const std::vector<std::string_view>& generativeBackendIds() {
    static const std::vector<std::string_view> ids = {
        kGenerativeBackendOffline, kGenerativeBackendHosted, kGenerativeBackendByok};
    return ids;
}

bool isGenerativeBackendId(std::string_view id) {
    const std::vector<std::string_view>& ids = generativeBackendIds();
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

std::unique_ptr<GenerativeBackend> makeOfflineGenerativeBackend(std::string reason) {
    return std::make_unique<OfflineGenerativeBackend>(std::move(reason));
}

GenerativeBackendSelection selectGenerativeBackend(const GenerativeBackendRequest& request) {
    GenerativeBackendSelection selection;

    const std::string requested =
        request.id.empty() ? std::string(defaultGenerativeBackendId()) : request.id;

    // --- An id no backend answers to: install offline, say which id was rejected.
    if (!isGenerativeBackendId(requested)) {
        selection.id = std::string(kGenerativeBackendOffline);
        selection.startupError = fallbackNotice(
            requested, "it names no backend in the registry, which offers 'offline', 'hosted' "
                       "and 'byok'");
        selection.backend = makeOfflineGenerativeBackend(
            "generation is unavailable: no generative backend is configured — the configured "
            "identifier '" +
            requested + "' names no backend in the registry");
        return selection;
    }

    // --- offline: always available, no credentials, no transport. -------------
    if (requested == kGenerativeBackendOffline) {
        selection.id = std::string(kGenerativeBackendOffline);
        selection.backend = makeOfflineGenerativeBackend({});
        return selection;
    }

    // --- hosted / byok: credentials decide (Requirement 12.8). ---------------
    const bool authorized = request.credentials && request.credentials(requested);
    if (!authorized) {
        const bool hosted = requested == kGenerativeBackendHosted;
        selection.id = std::string(kGenerativeBackendOffline);
        selection.startupError = fallbackNotice(
            requested, hosted ? "it requires an authenticated hosted account and none is present"
                              : "it requires BYOK provider credentials and none are present");
        selection.backend = makeOfflineGenerativeBackend(
            hosted ? "generation is unavailable: no authenticated account — the configured "
                     "'hosted' backend requires an authenticated hosted account and none is "
                     "present"
                   : "generation is unavailable: no BYOK credentials — the configured 'byok' "
                     "backend requires provider credentials and none are present");
        return selection;
    }

    // --- The requested HTTPS client is installed. ----------------------------
    std::unique_ptr<GenerativeHttpTransport> owned;
    GenerativeHttpTransport& transport = transportFor(request, owned);

    selection.id = requested;
    if (requested == kGenerativeBackendHosted) {
        HostedGenerativeBackend::Options options;
        options.endpoint = request.endpoint;
        options.secretStore = request.secretStore;
        options.userId = request.userId;
        selection.backend = std::make_unique<OwningTransportBackend<HostedGenerativeBackend>>(
            std::move(owned), transport, std::move(options));
    } else {
        ByokGenerativeBackend::Options options;
        options.endpoint = request.endpoint;
        options.secretStore = request.secretStore;
        options.provider = request.byokProvider;
        options.userId = request.userId;
        selection.backend = std::make_unique<OwningTransportBackend<ByokGenerativeBackend>>(
            std::move(owned), transport, std::move(options));
    }
    return selection;
}

}  // namespace palmier::services
