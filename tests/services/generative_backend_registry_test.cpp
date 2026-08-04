// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/services/generative_backend_registry_test.cpp — the generative backend
// registry and its two HTTPS clients (task 10.5; Requirements 12.1, 12.2, 12.4,
// 12.6, 12.8).
//
// What this suite can and cannot reach, stated plainly because it decides how
// much of task 10.5 is actually verified:
//
//   CAN be exercised offline, and is:
//     * selection — every id, the unknown-id fallback, the credential-less
//       fallback, the startup diagnostic's contents;
//     * the offline stub's rejection: its error code, that it names the unmet
//       precondition, that it is immediate, and — through the socket interposers
//       below — that it opens no socket, resolves no name and sends no datagram;
//     * request construction — method, URL, headers, JSON body, and WHERE the
//       credential ends up, for both clients;
//     * credential loading — read from the SecretStore at request time, not at
//       construction, so a credential filed (or removed) after the backend was
//       built changes the next request;
//     * error mapping — each HTTP status onto exactly one ErrorCode, and a
//       malformed body onto Internal.
//
//   CANNOT be exercised here, and is therefore UNVERIFIED:
//     * any real TLS handshake, certificate validation, DNS, timeout or
//       connection-reset behaviour. This repository links no HTTP client library
//       and the hosted service is out of tree (Requirement 12.6), so there is no
//       endpoint to talk to. What is proven is that everything ABOVE the seam is
//       correct and that the seam is the only route to the network; the transport
//       implementation itself is future work and its absence is reported as an
//       Unsupported error rather than hidden.
//
// The socket interposers are the same technique the offline interpreter suite
// uses: ordinary strong definitions in this executable, so the dynamic linker
// resolves calls from every object in this binary to them, each recording the call
// while armed and then forwarding to glibc.

#include <gtest/gtest.h>

#include <dlfcn.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/Error.hpp"
#include "core/Result.hpp"
#include "services/ByokCredentialManager.hpp"
#include "services/ByokGenerativeBackend.hpp"
#include "services/GenerativeBackendRegistry.hpp"
#include "services/GenerativeHttpTransport.hpp"
#include "services/HostedGenerativeBackend.hpp"
#include "services/Json.hpp"
#include "services/SecretStore.hpp"

namespace {

std::atomic<bool> gNetworkArmed{false};
std::atomic<std::size_t> gSocketCalls{0};

void noteSocketCall() {
    if (gNetworkArmed.load(std::memory_order_relaxed)) {
        gSocketCalls.fetch_add(1, std::memory_order_relaxed);
    }
}

/// Arms the interposers for one operation and reports what happened.
class NetworkWatch {
public:
    NetworkWatch() {
        gSocketCalls.store(0, std::memory_order_relaxed);
        gNetworkArmed.store(true, std::memory_order_relaxed);
    }
    ~NetworkWatch() { gNetworkArmed.store(false, std::memory_order_relaxed); }

    NetworkWatch(const NetworkWatch&) = delete;
    NetworkWatch& operator=(const NetworkWatch&) = delete;

    [[nodiscard]] std::size_t calls() const {
        return gSocketCalls.load(std::memory_order_relaxed);
    }
};

}  // namespace

extern "C" {

int socket(int domain, int type, int protocol) {
    noteSocketCall();
    using Fn = int (*)(int, int, int);
    static Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "socket"));
    return real ? real(domain, type, protocol) : -1;
}

int connect(int fd, const struct sockaddr* address, socklen_t length) {
    noteSocketCall();
    using Fn = int (*)(int, const struct sockaddr*, socklen_t);
    static Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "connect"));
    return real ? real(fd, address, length) : -1;
}

int getaddrinfo(const char* node, const char* service, const struct addrinfo* hints,
                struct addrinfo** result) {
    noteSocketCall();
    using Fn = int (*)(const char*, const char*, const struct addrinfo*, struct addrinfo**);
    static Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "getaddrinfo"));
    return real ? real(node, service, hints, result) : EAI_FAIL;
}

ssize_t sendto(int fd, const void* buffer, size_t length, int flags,
               const struct sockaddr* address, socklen_t addressLength) {
    noteSocketCall();
    using Fn = ssize_t (*)(int, const void*, size_t, int, const struct sockaddr*, socklen_t);
    static Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "sendto"));
    return real ? real(fd, buffer, length, flags, address, addressLength) : -1;
}

}  // extern "C"

namespace palmier::services {
namespace {

using namespace std::chrono_literals;

/// An endpoint that resolves to nothing. It is a location, not a credential, and
/// `.invalid` is reserved by RFC 2606 so a bug that actually sent a request could
/// not reach a real service.
constexpr const char* kEndpointBase = "https://generative.invalid";

/// Placeholder credential values. Each NAMES ITSELF, which is both a readability
/// aid and the reason task 10.8's repository-hygiene checker classifies them as
/// descriptions of a secret rather than one.
constexpr const char* kStoredHostedCredential = "stored-hosted-account-token-placeholder";
constexpr const char* kStoredProviderKey = "stored-byok-provider-key-placeholder";
constexpr const char* kGateBearer = "gate-supplied-bearer-token-placeholder";

[[nodiscard]] GenerativeEndpoint endpoint() {
    GenerativeEndpoint out;
    out.baseUrl = kEndpointBase;
    return out;
}

/// A transport that records every request and replays scripted responses. This is
/// the whole point of the seam: the clients are driven to completion with no
/// endpoint, no TLS and no socket.
class ScriptedTransport final : public GenerativeHttpTransport {
public:
    [[nodiscard]] Result<GenerativeHttpResponse> send(
        const GenerativeHttpRequest& request) override {
        requests.push_back(request);
        if (transportError.has_value()) {
            return err<GenerativeHttpResponse>(*transportError);
        }
        if (responses.empty()) {
            return err<GenerativeHttpResponse>(
                makeError(ErrorCode::Internal, "the test script ran out of responses"));
        }
        GenerativeHttpResponse next = responses.front();
        responses.erase(responses.begin());
        return Result<GenerativeHttpResponse>(std::move(next));
    }

    std::vector<GenerativeHttpRequest> requests;
    std::vector<GenerativeHttpResponse> responses;
    std::optional<Error> transportError;
};

/// A transport that fails the test if it is ever asked to send anything. Installed
/// wherever a "this must not reach the network" claim is being checked.
class ForbiddenTransport final : public GenerativeHttpTransport {
public:
    [[nodiscard]] Result<GenerativeHttpResponse> send(
        const GenerativeHttpRequest& request) override {
        ADD_FAILURE() << "the backend attempted a network request to " << request.url;
        return err<GenerativeHttpResponse>(makeError(ErrorCode::Internal, "forbidden"));
    }
};

[[nodiscard]] GenerationRequest sampleRequest() {
    GenerationRequest request;
    request.model = "sota-video-1";
    request.mediaType = GenerationMediaType::Video;
    request.prompt = "a slow pan across a harbour at dawn";
    request.params.emplace("resolution", "1920x1080");
    return request;
}

// ===========================================================================
// The id set (Requirement 12.2)
// ===========================================================================

TEST(GenerativeBackendRegistry, OffersExactlyTheThreeRequiredIdsWithOfflineDefault) {
    // Requirement 12.2 names three backends; `offline` is the default and comes
    // first because it is also the fallback.
    ASSERT_EQ(generativeBackendIds().size(), 3u);
    EXPECT_EQ(generativeBackendIds()[0], "offline");
    EXPECT_EQ(generativeBackendIds()[1], "hosted");
    EXPECT_EQ(generativeBackendIds()[2], "byok");
    EXPECT_EQ(defaultGenerativeBackendId(), "offline");

    for (const std::string_view id : generativeBackendIds()) {
        EXPECT_TRUE(isGenerativeBackendId(id)) << id;
    }
    EXPECT_FALSE(isGenerativeBackendId("hosted-v2"));
    EXPECT_FALSE(isGenerativeBackendId(""));
    EXPECT_FALSE(isGenerativeBackendId("OFFLINE")) << "ids are matched exactly, not casefolded";
}

// ===========================================================================
// Selection and fallback (Requirements 12.1, 12.8)
// ===========================================================================

TEST(GenerativeBackendRegistry, AnEmptyIdSelectsTheOfflineStubWithNoDiagnostic) {
    const GenerativeBackendSelection selection = selectGenerativeBackend({});
    EXPECT_EQ(selection.id, "offline");
    ASSERT_NE(selection.backend, nullptr);
    EXPECT_EQ(selection.backend->backendId(), "offline");
    EXPECT_TRUE(selection.startupError.empty());
    EXPECT_FALSE(selection.fellBack());
}

TEST(GenerativeBackendRegistry, AnUnknownIdInstallsOfflineAndNamesTheRejectedId) {
    GenerativeBackendRequest request;
    request.id = "no-such-backend";

    const GenerativeBackendSelection selection = selectGenerativeBackend(request);

    // Requirement 12.8: the offline stub is installed, the rejected id is named,
    // and the unmet requirement is stated. Never fatal.
    EXPECT_EQ(selection.id, "offline");
    ASSERT_NE(selection.backend, nullptr);
    EXPECT_TRUE(selection.fellBack());
    EXPECT_NE(selection.startupError.find("no-such-backend"), std::string::npos);
    EXPECT_NE(selection.startupError.find("names no backend in the registry"),
              std::string::npos);

    // The stub's own rejection also names the id that was asked for, so a user who
    // sees only the tool error still learns what went wrong.
    EXPECT_NE(selection.backend->unmetPrecondition().find("no-such-backend"),
              std::string::npos);
}

TEST(GenerativeBackendRegistry, CredentiallessHostedAndByokFallBackNamingTheRequirement) {
    for (const std::string_view id : {kGenerativeBackendHosted, kGenerativeBackendByok}) {
        GenerativeBackendRequest request;
        request.id = std::string(id);
        request.endpoint = endpoint();
        // No `credentials` probe at all — the Offline_Mode default.

        const GenerativeBackendSelection selection = selectGenerativeBackend(request);
        EXPECT_EQ(selection.id, "offline") << id;
        ASSERT_NE(selection.backend, nullptr);
        EXPECT_TRUE(selection.fellBack()) << id;
        EXPECT_NE(selection.startupError.find(id), std::string::npos) << id;

        const std::string unmet = selection.backend->unmetPrecondition();
        if (id == kGenerativeBackendHosted) {
            EXPECT_NE(unmet.find("no authenticated account"), std::string::npos) << unmet;
        } else {
            EXPECT_NE(unmet.find("no BYOK credentials"), std::string::npos) << unmet;
        }
    }
}

TEST(GenerativeBackendRegistry, AnAuthorizedIdIsInstalledAsAsked) {
    InMemorySecretStore store;
    ScriptedTransport transport;

    for (const std::string_view id : {kGenerativeBackendHosted, kGenerativeBackendByok}) {
        GenerativeBackendRequest request;
        request.id = std::string(id);
        request.endpoint = endpoint();
        request.secretStore = &store;
        request.transport = &transport;
        request.byokProvider = "example-provider";
        request.credentials = [](std::string_view) { return true; };

        const GenerativeBackendSelection selection = selectGenerativeBackend(request);
        EXPECT_EQ(selection.id, id);
        ASSERT_NE(selection.backend, nullptr);
        EXPECT_EQ(selection.backend->backendId(), id);
        EXPECT_TRUE(selection.startupError.empty()) << selection.startupError;
    }
}

TEST(GenerativeBackendRegistry, ASelectedClientWithNoInjectedTransportStillInstalls) {
    // Requirement 12.2's "without recompilation" has a corollary: a build with no
    // HTTPS transport must still SELECT the configured id. The capability is then
    // reported per request rather than at startup.
    InMemorySecretStore store;
    ASSERT_TRUE(store.store(HostedGenerativeBackend::credentialKey("default"),
                            kStoredHostedCredential)
                    .isOk());

    GenerativeBackendRequest request;
    request.id = "hosted";
    request.endpoint = endpoint();
    request.secretStore = &store;
    request.transport = nullptr;  // no HTTPS client in this build
    request.credentials = [](std::string_view) { return true; };

    GenerativeBackendSelection selection = selectGenerativeBackend(request);
    ASSERT_EQ(selection.id, "hosted");
    ASSERT_NE(selection.backend, nullptr);
    EXPECT_TRUE(selection.backend->unmetPrecondition().empty());

    const Result<JobId> submitted = selection.backend->submit(sampleRequest(), "");
    ASSERT_TRUE(submitted.isError());
    EXPECT_EQ(submitted.error().code(), ErrorCode::Unsupported);
    EXPECT_NE(submitted.error().message().find("no network connection was attempted"),
              std::string::npos);
}

// ===========================================================================
// The offline stub (Requirements 12.4, 12.5)
// ===========================================================================

TEST(OfflineGenerativeBackend, RejectsImmediatelyNamingTheUnmetPrecondition) {
    const std::unique_ptr<GenerativeBackend> backend = makeOfflineGenerativeBackend({});
    ASSERT_NE(backend, nullptr);

    const std::string unmet = backend->unmetPrecondition();
    // Requirement 12.4 enumerates the three preconditions the message must be able
    // to name; the generic stub is installed precisely when none of them is met.
    EXPECT_NE(unmet.find("generation is unavailable"), std::string::npos) << unmet;
    EXPECT_NE(unmet.find("no reachable network"), std::string::npos) << unmet;
    EXPECT_NE(unmet.find("no authenticated account"), std::string::npos) << unmet;
    EXPECT_NE(unmet.find("no BYOK credentials"), std::string::npos) << unmet;

    const auto started = std::chrono::steady_clock::now();
    const Result<JobId> submitted = backend->submit(sampleRequest(), kGateBearer);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    ASSERT_TRUE(submitted.isError());
    EXPECT_EQ(submitted.error().code(), ErrorCode::FailedPrecondition);
    EXPECT_EQ(submitted.error().message(), unmet);
    EXPECT_LT(elapsed, 1s) << "Requirement 12.4 bounds the rejection at one second";

    // Poll and fetch reject the same way; cancel succeeds, because a job that was
    // never submitted is not running.
    EXPECT_TRUE(backend->poll(JobId{"job-1"}, kGateBearer).isError());
    EXPECT_TRUE(backend->fetchResult(JobId{"job-1"}, kGateBearer).isError());
    EXPECT_TRUE(backend->cancel(JobId{"job-1"}, kGateBearer).isOk());
}

TEST(OfflineGenerativeBackend, OpensNoSocketAndResolvesNoName) {
    const std::unique_ptr<GenerativeBackend> backend = makeOfflineGenerativeBackend({});

    // Requirement 12.4: "without attempting a network connection". The interposers
    // observe the C library entry points directly, so this is a proof about the
    // process rather than an assertion about a mock.
    NetworkWatch watch;
    EXPECT_TRUE(backend->submit(sampleRequest(), kGateBearer).isError());
    EXPECT_TRUE(backend->poll(JobId{"job-1"}, kGateBearer).isError());
    EXPECT_TRUE(backend->fetchResult(JobId{"job-1"}, kGateBearer).isError());
    EXPECT_TRUE(backend->cancel(JobId{"job-1"}, kGateBearer).isOk());
    EXPECT_EQ(watch.calls(), 0u);
}

TEST(OfflineGenerativeBackend, TheInterposersActuallyObserveARealSocketCall) {
    // Guards the test above: if the interposers were not linked in, the zero count
    // would be vacuous. Arming and then opening a socket must be seen.
    NetworkWatch watch;
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_GE(fd, 0) << "the interposer must forward to the real socket()";
    if (fd >= 0) ::close(fd);
    EXPECT_GE(watch.calls(), 1u);
}

TEST(OfflineGenerativeBackend, ReportsTheSpecificReasonItWasInstalledWith) {
    const std::unique_ptr<GenerativeBackend> backend =
        makeOfflineGenerativeBackend("generation is unavailable: no BYOK credentials — none "
                                     "are present");
    EXPECT_NE(backend->unmetPrecondition().find("no BYOK credentials"), std::string::npos);
    const Result<JobId> submitted = backend->submit(sampleRequest(), "");
    ASSERT_TRUE(submitted.isError());
    EXPECT_NE(submitted.error().message().find("no BYOK credentials"), std::string::npos);
}

// ===========================================================================
// Request construction and credential loading (Requirements 12.1, 12.6)
// ===========================================================================

TEST(HostedGenerativeBackend, BuildsAnHttpsPostCarryingTheStoredCredentialAsABearer) {
    InMemorySecretStore store;
    ASSERT_TRUE(store.store(HostedGenerativeBackend::credentialKey("default"),
                            kStoredHostedCredential)
                    .isOk());

    ScriptedTransport transport;
    HostedGenerativeBackend::Options options;
    options.endpoint = endpoint();
    options.secretStore = &store;
    HostedGenerativeBackend backend{transport, options};

    EXPECT_TRUE(backend.unmetPrecondition().empty());

    const Result<GenerativeHttpRequest> built =
        backend.buildSubmitRequest(sampleRequest(), "");
    ASSERT_TRUE(built.isOk()) << built.error().toString();

    EXPECT_EQ(built.value().method, "POST");
    EXPECT_EQ(built.value().url, std::string(kEndpointBase) + "/v1/generations");
    EXPECT_EQ(built.value().header("Authorization"),
              std::string("Bearer ") + kStoredHostedCredential);
    EXPECT_EQ(built.value().header("Content-Type"), "application/json");
    EXPECT_TRUE(built.value().hasHeader("accept")) << "header lookup is case-insensitive";

    const Result<Json> body = Json::parse(built.value().body);
    ASSERT_TRUE(body.isOk()) << body.error().toString();
    EXPECT_EQ(body.value().stringOr("model"), "sota-video-1");
    EXPECT_EQ(body.value().stringOr("mediaType"), "video");
    EXPECT_EQ(body.value().stringOr("prompt"), "a slow pan across a harbour at dawn");
    const Json* params = body.value().find("params");
    ASSERT_NE(params, nullptr);
    EXPECT_EQ(params->stringOr("resolution"), "1920x1080");

    // Building a request sends nothing.
    EXPECT_TRUE(transport.requests.empty());
}

TEST(HostedGenerativeBackend, ReadsTheCredentialAtRequestTimeNotAtConstruction) {
    InMemorySecretStore store;
    ScriptedTransport transport;
    HostedGenerativeBackend::Options options;
    options.endpoint = endpoint();
    options.secretStore = &store;
    HostedGenerativeBackend backend{transport, options};

    // Constructed against an empty store: no credential, so the precondition is
    // unmet and no request can be built.
    EXPECT_FALSE(backend.unmetPrecondition().empty());
    EXPECT_TRUE(backend.buildSubmitRequest(sampleRequest(), "").isError());

    // Filing one afterwards changes the very next request — which is the whole
    // reason the value is not captured in the constructor.
    ASSERT_TRUE(store.store(HostedGenerativeBackend::credentialKey("default"),
                            kStoredHostedCredential)
                    .isOk());
    EXPECT_TRUE(backend.unmetPrecondition().empty());
    const Result<GenerativeHttpRequest> built =
        backend.buildSubmitRequest(sampleRequest(), "");
    ASSERT_TRUE(built.isOk());
    EXPECT_EQ(built.value().header("Authorization"),
              std::string("Bearer ") + kStoredHostedCredential);

    // And removing it takes the capability away again, without a rebuild.
    ASSERT_TRUE(store.remove(HostedGenerativeBackend::credentialKey("default")).isOk());
    EXPECT_FALSE(backend.unmetPrecondition().empty());
}

TEST(HostedGenerativeBackend, FallsBackToTheGateBearerWhenTheStoreHoldsNothing) {
    InMemorySecretStore store;
    ScriptedTransport transport;
    HostedGenerativeBackend::Options options;
    options.endpoint = endpoint();
    options.secretStore = &store;
    HostedGenerativeBackend backend{transport, options};

    const Result<GenerativeHttpRequest> built =
        backend.buildSubmitRequest(sampleRequest(), kGateBearer);
    ASSERT_TRUE(built.isOk()) << built.error().toString();
    EXPECT_EQ(built.value().header("Authorization"), std::string("Bearer ") + kGateBearer);
}

TEST(HostedGenerativeBackend, AnUnconfiguredEndpointSendsNothingAndSaysWhy) {
    InMemorySecretStore store;
    ASSERT_TRUE(store.store(HostedGenerativeBackend::credentialKey("default"),
                            kStoredHostedCredential)
                    .isOk());

    ForbiddenTransport transport;
    HostedGenerativeBackend::Options options;
    options.secretStore = &store;  // endpoint left empty
    HostedGenerativeBackend backend{transport, options};

    EXPECT_NE(backend.unmetPrecondition().find("no reachable network"), std::string::npos);

    NetworkWatch watch;
    const Result<JobId> submitted = backend.submit(sampleRequest(), "");
    ASSERT_TRUE(submitted.isError());
    EXPECT_EQ(submitted.error().code(), ErrorCode::FailedPrecondition);
    EXPECT_EQ(watch.calls(), 0u);
}

TEST(HostedGenerativeBackend, APlaintextEndpointIsRefusedRatherThanSentTo) {
    // A credential must never leave over http://. The client refuses to build the
    // request at all, so there is nothing to send.
    InMemorySecretStore store;
    ASSERT_TRUE(store.store(HostedGenerativeBackend::credentialKey("default"),
                            kStoredHostedCredential)
                    .isOk());

    ForbiddenTransport transport;
    HostedGenerativeBackend::Options options;
    options.endpoint.baseUrl = "http://generative.invalid";
    options.secretStore = &store;
    HostedGenerativeBackend backend{transport, options};

    const Result<JobId> submitted = backend.submit(sampleRequest(), "");
    ASSERT_TRUE(submitted.isError());
    EXPECT_EQ(submitted.error().code(), ErrorCode::InvalidArgument);
    EXPECT_NE(submitted.error().message().find("https"), std::string::npos);
}

TEST(ByokGenerativeBackend, SendsTheProviderKeyInItsOwnHeaderAndNamesTheProvider) {
    InMemorySecretStore store;
    ASSERT_TRUE(
        store.store(ByokGenerativeBackend::credentialKey("default", "example-provider"),
                    kStoredProviderKey)
            .isOk());

    ScriptedTransport transport;
    ByokGenerativeBackend::Options options;
    options.endpoint = endpoint();
    options.secretStore = &store;
    options.provider = "example-provider";
    ByokGenerativeBackend backend{transport, options};

    EXPECT_TRUE(backend.unmetPrecondition().empty());

    const Result<GenerativeHttpRequest> built =
        backend.buildSubmitRequest(sampleRequest(), "");
    ASSERT_TRUE(built.isOk()) << built.error().toString();
    EXPECT_EQ(built.value().header("X-Api-Key"), kStoredProviderKey);
    EXPECT_EQ(built.value().header("X-Palmier-Provider"), "example-provider");
    EXPECT_TRUE(built.value().header("Authorization").empty())
        << "a provider key is not a bearer";
}

TEST(ByokGenerativeBackend, NeverSendsTheGateBearerToAThirdPartyProvider) {
    // A hosted session token is not a provider key. With no stored key the request
    // is refused rather than authorized with whatever the gate happened to supply.
    InMemorySecretStore store;
    ForbiddenTransport transport;
    ByokGenerativeBackend::Options options;
    options.endpoint = endpoint();
    options.secretStore = &store;
    options.provider = "example-provider";
    ByokGenerativeBackend backend{transport, options};

    const Result<JobId> submitted = backend.submit(sampleRequest(), kGateBearer);
    ASSERT_TRUE(submitted.isError());
    EXPECT_EQ(submitted.error().code(), ErrorCode::Unauthenticated);
    EXPECT_NE(submitted.error().message().find("no BYOK credentials"), std::string::npos);
}

TEST(ByokGenerativeBackend, ItsSecretKeyDerivationMatchesTheCredentialManager) {
    // The BYOK client re-derives the key rather than depending on the manager (so
    // it stays linkable in the lean service-layer test binaries). That is only safe
    // if the two derivations agree, so pin them together here rather than trusting
    // the comment that says they do.
    class RejectingValidator final : public ByokProviderValidator {
    public:
        [[nodiscard]] Result<void> validate(const ByokCredential&) override {
            return err<void>(makeError(ErrorCode::Unsupported, "not used"));
        }
    };
    RejectingValidator validator;
    InMemorySecretStore store;
    const ByokCredentialManager manager{validator, store, "default"};

    EXPECT_EQ(ByokGenerativeBackend::credentialKey("default", "example-provider"),
              manager.storageKey("example-provider"));
    EXPECT_EQ(ByokGenerativeBackend::credentialKey("someone-else", "example-provider"),
              "palmier/byok/someone-else/example-provider");
}

// ===========================================================================
// The exchange and its error mapping
// ===========================================================================

TEST(HostedGenerativeBackend, DrivesASubmitPollFetchLifecycleOverTheSeam) {
    InMemorySecretStore store;
    ASSERT_TRUE(store.store(HostedGenerativeBackend::credentialKey("default"),
                            kStoredHostedCredential)
                    .isOk());

    ScriptedTransport transport;
    transport.responses.push_back({201, R"({"id":"job-42"})"});
    transport.responses.push_back({200, R"({"status":"running","progress":40})"});
    transport.responses.push_back({200, R"({"status":"succeeded","progress":100})"});
    transport.responses.push_back(
        {200,
         R"({"assetId":"6f9619ff-8b86-d011-b42d-00cf4fc964ff",)"
         R"("sourcePath":"/var/tmp/generated.mp4","mediaType":"video"})"});

    HostedGenerativeBackend::Options options;
    options.endpoint = endpoint();
    options.secretStore = &store;
    HostedGenerativeBackend backend{transport, options};

    const Result<JobId> submitted = backend.submit(sampleRequest(), "");
    ASSERT_TRUE(submitted.isOk()) << submitted.error().toString();
    EXPECT_EQ(submitted.value().value, "job-42");

    const Result<GenerationStatus> running = backend.poll(submitted.value(), "");
    ASSERT_TRUE(running.isOk()) << running.error().toString();
    EXPECT_EQ(running.value().phase, GenerationPhase::Running);
    EXPECT_EQ(running.value().progressPercent, 40);
    EXPECT_FALSE(running.value().isTerminal());

    const Result<GenerationStatus> done = backend.poll(submitted.value(), "");
    ASSERT_TRUE(done.isOk());
    EXPECT_EQ(done.value().phase, GenerationPhase::Succeeded);
    EXPECT_TRUE(done.value().isTerminal());

    const Result<MediaAsset> asset = backend.fetchResult(submitted.value(), "");
    ASSERT_TRUE(asset.isOk()) << asset.error().toString();
    EXPECT_EQ(asset.value().ref.sourcePath, "/var/tmp/generated.mp4");
    EXPECT_EQ(asset.value().ref.assetId.toString(), "6f9619ff-8b86-d011-b42d-00cf4fc964ff");
    EXPECT_EQ(asset.value().mediaType, GenerationMediaType::Video);

    // Each exchange addressed the job resource derived from the submitted id, and
    // every request carried the credential.
    ASSERT_EQ(transport.requests.size(), 4u);
    EXPECT_EQ(transport.requests[1].url, std::string(kEndpointBase) + "/v1/generations/job-42");
    EXPECT_EQ(transport.requests[3].url,
              std::string(kEndpointBase) + "/v1/generations/job-42/result");
    for (const GenerativeHttpRequest& request : transport.requests) {
        EXPECT_EQ(request.header("Authorization"),
                  std::string("Bearer ") + kStoredHostedCredential);
    }
}

TEST(HostedGenerativeBackend, ReportsAProviderSideFailureWithItsReason) {
    InMemorySecretStore store;
    ASSERT_TRUE(store.store(HostedGenerativeBackend::credentialKey("default"),
                            kStoredHostedCredential)
                    .isOk());

    ScriptedTransport transport;
    transport.responses.push_back({200, R"({"status":"failed","reason":"the model refused"})"});

    HostedGenerativeBackend::Options options;
    options.endpoint = endpoint();
    options.secretStore = &store;
    HostedGenerativeBackend backend{transport, options};

    const Result<GenerationStatus> status = backend.poll(JobId{"job-42"}, "");
    ASSERT_TRUE(status.isOk()) << status.error().toString();
    EXPECT_EQ(status.value().phase, GenerationPhase::Failed);
    EXPECT_EQ(status.value().failureReason, "the model refused");
}

TEST(GenerativeHttpProtocol, MapsEachHttpStatusOntoExactlyOneErrorCode) {
    const struct {
        int status;
        ErrorCode code;
    } cases[] = {
        {400, ErrorCode::InvalidArgument}, {422, ErrorCode::InvalidArgument},
        {401, ErrorCode::Unauthenticated}, {403, ErrorCode::PermissionDenied},
        {404, ErrorCode::NotFound},        {408, ErrorCode::Timeout},
        {504, ErrorCode::Timeout},         {429, ErrorCode::FailedPrecondition},
        {503, ErrorCode::FailedPrecondition}, {501, ErrorCode::Unsupported},
        {500, ErrorCode::Io},              {502, ErrorCode::Io},
    };
    for (const auto& testCase : cases) {
        const Error mapped = mapGenerativeHttpStatus(testCase.status, "detail");
        EXPECT_EQ(mapped.code(), testCase.code) << "status " << testCase.status;
        EXPECT_NE(mapped.message().find(std::to_string(testCase.status)), std::string::npos);
        EXPECT_NE(mapped.message().find("detail"), std::string::npos);
    }
}

TEST(HostedGenerativeBackend, MapsAnErrorStatusAndSurfacesTheProvidersMessage) {
    InMemorySecretStore store;
    ASSERT_TRUE(store.store(HostedGenerativeBackend::credentialKey("default"),
                            kStoredHostedCredential)
                    .isOk());

    ScriptedTransport transport;
    transport.responses.push_back({401, R"({"error":{"message":"the session has expired"}})"});

    HostedGenerativeBackend::Options options;
    options.endpoint = endpoint();
    options.secretStore = &store;
    HostedGenerativeBackend backend{transport, options};

    const Result<JobId> submitted = backend.submit(sampleRequest(), "");
    ASSERT_TRUE(submitted.isError());
    EXPECT_EQ(submitted.error().code(), ErrorCode::Unauthenticated);
    EXPECT_NE(submitted.error().message().find("the session has expired"), std::string::npos);
}

TEST(HostedGenerativeBackend, MapsAMalformedOrIncompleteBodyOntoInternal) {
    InMemorySecretStore store;
    ASSERT_TRUE(store.store(HostedGenerativeBackend::credentialKey("default"),
                            kStoredHostedCredential)
                    .isOk());

    ScriptedTransport transport;
    transport.responses.push_back({200, "not json at all"});
    transport.responses.push_back({200, R"({"accepted":true})"});   // no job id
    transport.responses.push_back({200, R"({"status":"levitating"})"});

    HostedGenerativeBackend::Options options;
    options.endpoint = endpoint();
    options.secretStore = &store;
    HostedGenerativeBackend backend{transport, options};

    const Result<JobId> malformed = backend.submit(sampleRequest(), "");
    ASSERT_TRUE(malformed.isError());
    EXPECT_EQ(malformed.error().code(), ErrorCode::Internal);

    const Result<JobId> idless = backend.submit(sampleRequest(), "");
    ASSERT_TRUE(idless.isError());
    EXPECT_EQ(idless.error().code(), ErrorCode::Internal);
    EXPECT_NE(idless.error().message().find("no job identifier"), std::string::npos);

    const Result<GenerationStatus> unknownPhase = backend.poll(JobId{"job-42"}, "");
    ASSERT_TRUE(unknownPhase.isError());
    EXPECT_EQ(unknownPhase.error().code(), ErrorCode::Internal);
}

TEST(HostedGenerativeBackend, ForwardsATransportFailureUnchanged) {
    InMemorySecretStore store;
    ASSERT_TRUE(store.store(HostedGenerativeBackend::credentialKey("default"),
                            kStoredHostedCredential)
                    .isOk());

    ScriptedTransport transport;
    transport.transportError =
        makeError(ErrorCode::Io, "the TLS handshake failed before any bytes were exchanged");

    HostedGenerativeBackend::Options options;
    options.endpoint = endpoint();
    options.secretStore = &store;
    HostedGenerativeBackend backend{transport, options};

    const Result<JobId> submitted = backend.submit(sampleRequest(), "");
    ASSERT_TRUE(submitted.isError());
    EXPECT_EQ(submitted.error().code(), ErrorCode::Io);
    EXPECT_NE(submitted.error().message().find("TLS handshake"), std::string::npos);
}

TEST(HostedGenerativeBackend, CancellingAJobTheEndpointForgotSucceeds) {
    InMemorySecretStore store;
    ASSERT_TRUE(store.store(HostedGenerativeBackend::credentialKey("default"),
                            kStoredHostedCredential)
                    .isOk());

    ScriptedTransport transport;
    transport.responses.push_back({404, "{}"});
    transport.responses.push_back({204, ""});

    HostedGenerativeBackend::Options options;
    options.endpoint = endpoint();
    options.secretStore = &store;
    HostedGenerativeBackend backend{transport, options};

    EXPECT_TRUE(backend.cancel(JobId{"job-42"}, "").isOk());
    EXPECT_TRUE(backend.cancel(JobId{"job-43"}, "").isOk());
    ASSERT_EQ(transport.requests.size(), 2u);
    EXPECT_EQ(transport.requests[0].method, "POST");
    EXPECT_EQ(transport.requests[0].url,
              std::string(kEndpointBase) + "/v1/generations/job-42/cancel");
}

TEST(GenerativeHttpProtocol, AnEmptyJobIdIsRefusedBeforeAnythingIsSent) {
    ForbiddenTransport transport;
    HttpGenerativeJobProtocol protocol{transport, {.endpoint = endpoint()}};

    NetworkWatch watch;
    EXPECT_EQ(protocol.poll(JobId{}, kGateBearer).error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(protocol.fetchResult(JobId{}, kGateBearer).error().code(),
              ErrorCode::InvalidArgument);
    EXPECT_EQ(protocol.cancel(JobId{}, kGateBearer).error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(watch.calls(), 0u);
}

TEST(GenerativeHttpProtocol, TheUnavailableTransportContactsNothing) {
    const std::unique_ptr<GenerativeHttpTransport> transport =
        makeUnavailableGenerativeHttpTransport();
    ASSERT_NE(transport, nullptr);

    GenerativeHttpRequest request;
    request.url = std::string(kEndpointBase) + "/v1/generations";

    NetworkWatch watch;
    const Result<GenerativeHttpResponse> sent = transport->send(request);
    ASSERT_TRUE(sent.isError());
    EXPECT_EQ(sent.error().code(), ErrorCode::Unsupported);
    EXPECT_EQ(watch.calls(), 0u);
}

}  // namespace
}  // namespace palmier::services
