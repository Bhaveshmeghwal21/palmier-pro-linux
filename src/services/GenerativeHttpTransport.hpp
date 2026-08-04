// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/GenerativeHttpTransport.hpp — the injectable network seam the
// generative HTTPS clients speak through, plus the wire protocol they share
// (task 10.5; Requirements 12.1, 12.2, 12.4, 12.6).
//
// Why a seam rather than a socket
// ------------------------------
// `HostedGenerativeBackend` and `ByokGenerativeBackend` are HTTPS clients. Their
// interesting behaviour is NOT the socket: it is
//
//   * request construction — method, URL, headers, JSON body;
//   * credential loading — read from the `SecretStore` at call time, never from a
//     compiled-in literal (Requirement 12.6: this repository contains no
//     hosted-service credential values);
//   * error mapping — an HTTP status or a malformed body becoming exactly one
//     `ErrorCode`.
//
// All three are testable with no endpoint, no TLS and no network at all, provided
// the transport is a declared seam. That is the whole reason this interface
// exists: a test supplies a scripted transport, asserts on the request the client
// built and hands back a canned response. It is also what lets a test prove the
// NEGATIVE claim of Requirement 12.4 — that the offline backend never touches the
// network — because a transport that fails the test if invoked can be installed
// and then observed to stay untouched.
//
// What this file deliberately does NOT contain is a real HTTPS implementation.
// The hosted service itself is out of tree (Requirement 12.6), and this project
// links no HTTP client library; `makeUnavailableGenerativeHttpTransport()` is the
// honest default, reporting `Unsupported` without contacting anything. Wiring a
// real transport is a matter of implementing this one interface — the clients
// above it need no change.

#ifndef PALMIER_SERVICES_GENERATIVEHTTPTRANSPORT_HPP
#define PALMIER_SERVICES_GENERATIVEHTTPTRANSPORT_HPP

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/Result.hpp"
#include "services/GenerativeClient.hpp"

namespace palmier::services {

// ---------------------------------------------------------------------------
// Request / response value types
// ---------------------------------------------------------------------------

/// One HTTPS request, fully built by the client before the transport sees it.
/// `url` is always absolute and always `https://` — `HttpGenerativeJobProtocol`
/// refuses to build anything else, so a misconfigured plaintext endpoint fails at
/// construction time rather than sending a credential in the clear.
struct GenerativeHttpRequest {
    std::string method = "GET";
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;

    /// The value of the first header whose name matches `name` case-insensitively,
    /// or an empty string. Provided for tests and for logging that must redact.
    [[nodiscard]] std::string header(std::string_view name) const;

    /// True iff a header named `name` is present (case-insensitive).
    [[nodiscard]] bool hasHeader(std::string_view name) const;
};

/// One HTTPS response. `body` is the raw payload; the protocol below parses it.
struct GenerativeHttpResponse {
    int status = 0;
    std::string body;
};

/// The seam. An implementation performs one request/response exchange over TLS.
/// A transport-level failure (DNS, connect, handshake, timeout) is reported as an
/// `Error`; any HTTP status — including 4xx and 5xx — is a successful exchange and
/// is classified by the protocol, not here.
class GenerativeHttpTransport {
public:
    virtual ~GenerativeHttpTransport() = default;

    [[nodiscard]] virtual Result<GenerativeHttpResponse> send(
        const GenerativeHttpRequest& request) = 0;
};

/// A transport that contacts nothing and reports `Unsupported`, naming the reason.
/// This is what a build with no HTTPS client library installs, so selecting
/// `hosted` or `byok` on such a build yields a descriptive error per request
/// rather than a link failure or a crash.
[[nodiscard]] std::unique_ptr<GenerativeHttpTransport> makeUnavailableGenerativeHttpTransport();

// ---------------------------------------------------------------------------
// The shared wire protocol
// ---------------------------------------------------------------------------

/// How a loaded credential is presented to the endpoint. The credential VALUE is
/// always supplied at call time; only the scheme is configuration.
enum class GenerativeAuthScheme {
    BearerAuthorization,  ///< `Authorization: Bearer <credential>`.
    ApiKeyHeader,         ///< `<apiKeyHeaderName>: <credential>`.
};

/// The endpoint a client talks to: a base URL and the resource paths beneath it.
/// No field here may ever hold a credential (Requirement 12.6) — a base URL is a
/// location, and the default is an unroutable placeholder rather than a real
/// service, so an unconfigured build cannot reach anything by accident.
struct GenerativeEndpoint {
    /// Absolute `https://` origin, no trailing slash. Empty means unconfigured.
    std::string baseUrl;

    /// Collection path for job submission; `/{jobId}` and `/{jobId}/result` and
    /// `/{jobId}/cancel` are derived from it.
    std::string jobsPath = "/v1/generations";
};

/// The submit -> poll -> fetch -> cancel exchange both HTTPS clients share.
///
/// It owns no credential and no policy: each call is handed the credential the
/// client loaded from the `SecretStore` a moment earlier, and every method has a
/// matching `*Request()` builder so a test can assert on the exact bytes without
/// sending them.
class HttpGenerativeJobProtocol {
public:
    struct Options {
        GenerativeEndpoint endpoint;
        GenerativeAuthScheme scheme = GenerativeAuthScheme::BearerAuthorization;
        std::string apiKeyHeaderName = "X-Api-Key";

        /// Extra headers every request carries (e.g. a provider selector). Never a
        /// credential: these are compiled-in or configured values.
        std::vector<std::pair<std::string, std::string>> extraHeaders;
    };

    /// `transport` must outlive this protocol.
    HttpGenerativeJobProtocol(GenerativeHttpTransport& transport, Options options);

    // --- request builders (pure; no network) --------------------------------

    [[nodiscard]] Result<GenerativeHttpRequest> submitRequest(
        const GenerationRequest& request, std::string_view credential) const;
    [[nodiscard]] Result<GenerativeHttpRequest> pollRequest(const JobId& id,
                                                           std::string_view credential) const;
    [[nodiscard]] Result<GenerativeHttpRequest> resultRequest(
        const JobId& id, std::string_view credential) const;
    [[nodiscard]] Result<GenerativeHttpRequest> cancelRequest(
        const JobId& id, std::string_view credential) const;

    // --- exchanges ----------------------------------------------------------

    [[nodiscard]] Result<JobId> submit(const GenerationRequest& request,
                                      std::string_view credential);
    [[nodiscard]] Result<GenerationStatus> poll(const JobId& id, std::string_view credential);
    [[nodiscard]] Result<MediaAsset> fetchResult(const JobId& id, std::string_view credential);
    [[nodiscard]] Result<void> cancel(const JobId& id, std::string_view credential);

    /// The configured endpoint, for diagnostics. Never contains a credential.
    [[nodiscard]] const GenerativeEndpoint& endpoint() const noexcept {
        return options_.endpoint;
    }

private:
    /// Common header set for one request, credential included at call time.
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> headersFor(
        std::string_view credential) const;

    /// `baseUrl + jobsPath + suffix`, validated to be an absolute https URL.
    [[nodiscard]] Result<std::string> urlFor(std::string_view suffix) const;

    GenerativeHttpTransport& transport_;
    Options options_;
};

/// Map an HTTP status onto the domain's coarse classification. Exposed because
/// the mapping is a documented behaviour of the clients, and a test that pins it
/// down should not have to reach through four layers to reach it.
[[nodiscard]] Error mapGenerativeHttpStatus(int status, std::string_view detail);

}  // namespace palmier::services

#endif  // PALMIER_SERVICES_GENERATIVEHTTPTRANSPORT_HPP
