// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/GenerativeHttpTransport.cpp — the seam's default implementation and
// the shared wire protocol (task 10.5; Requirements 12.1, 12.2, 12.4, 12.6).
// See the header for why the network is a seam rather than a socket.

#include "services/GenerativeHttpTransport.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <utility>

#include "core/Error.hpp"
#include "core/Uuid.hpp"
#include "services/Json.hpp"

namespace palmier::services {
namespace {

/// ASCII case-insensitive equality, for header names.
[[nodiscard]] bool equalsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto lower = [](char c) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        };
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}

/// True iff `url` is an absolute https URL with a non-empty authority. A
/// plaintext or relative endpoint is a configuration error, not a request to
/// attempt: sending a credential over http:// is exactly the mistake this check
/// exists to make impossible.
[[nodiscard]] bool isHttpsUrl(std::string_view url) {
    constexpr std::string_view kScheme = "https://";
    return url.size() > kScheme.size() && url.substr(0, kScheme.size()) == kScheme;
}

/// The response field a provider may use for the job identifier. Accepting more
/// than one name is deliberate: the wire shape of the out-of-tree hosted service
/// is not this repository's to fix, and a client that tolerates both spellings
/// needs no change when it settles.
[[nodiscard]] std::string jobIdFrom(const Json& body) {
    for (const char* key : {"id", "jobId", "job_id"}) {
        const std::string value = body.stringOr(key);
        if (!value.empty()) return value;
    }
    return {};
}

/// Map a provider status token onto a GenerationPhase. The four phases
/// Requirement 12.3 names — `queued`, `running`, `succeeded`, `failed` — are the
/// canonical spellings; the common synonyms are accepted for the same reason as
/// above.
[[nodiscard]] bool phaseFrom(std::string_view token, GenerationPhase& out) {
    if (token == "queued" || token == "pending" || token == "accepted") {
        out = GenerationPhase::Pending;
        return true;
    }
    if (token == "running" || token == "in_progress" || token == "processing") {
        out = GenerationPhase::Running;
        return true;
    }
    if (token == "succeeded" || token == "success" || token == "completed") {
        out = GenerationPhase::Succeeded;
        return true;
    }
    if (token == "failed" || token == "error" || token == "cancelled") {
        out = GenerationPhase::Failed;
        return true;
    }
    return false;
}

/// A transport that reports the capability as absent without contacting anything.
class UnavailableGenerativeHttpTransport final : public GenerativeHttpTransport {
public:
    [[nodiscard]] Result<GenerativeHttpResponse> send(
        const GenerativeHttpRequest&) override {
        return err<GenerativeHttpResponse>(makeError(
            ErrorCode::Unsupported,
            "no generative HTTPS transport is available in this build; no network "
            "connection was attempted"));
    }
};

/// Parse a response body, mapping a malformed payload onto `Internal` with the
/// operation named. The provider's bytes are not echoed: a body can carry a
/// credential the caller sent back, and a diagnostic must not become a leak.
[[nodiscard]] Result<Json> parseBody(const GenerativeHttpResponse& response,
                                     std::string_view operation) {
    Result<Json> parsed = Json::parse(response.body);
    if (parsed.isError()) {
        return err<Json>(makeError(
            ErrorCode::Internal, "the generative endpoint returned a malformed JSON body for " +
                                     std::string(operation)));
    }
    if (!parsed.value().isObject()) {
        return err<Json>(makeError(ErrorCode::Internal,
                                   "the generative endpoint returned a non-object JSON body for " +
                                       std::string(operation)));
    }
    return parsed;
}

/// The provider's own error text for a non-2xx response, if it supplied one.
[[nodiscard]] std::string providerDetail(const GenerativeHttpResponse& response) {
    const Result<Json> parsed = Json::parse(response.body);
    if (parsed.isError() || !parsed.value().isObject()) return {};
    const std::string direct = parsed.value().stringOr("message");
    if (!direct.empty()) return direct;
    if (const Json* nested = parsed.value().find("error"); nested != nullptr) {
        if (nested->isString()) return nested->asString();
        if (nested->isObject()) return nested->stringOr("message");
    }
    return {};
}

/// True for 2xx.
[[nodiscard]] bool isSuccess(int status) { return status >= 200 && status < 300; }

}  // namespace

// ---------------------------------------------------------------------------
// GenerativeHttpRequest
// ---------------------------------------------------------------------------

std::string GenerativeHttpRequest::header(std::string_view name) const {
    for (const auto& [key, value] : headers) {
        if (equalsIgnoreCase(key, name)) return value;
    }
    return {};
}

bool GenerativeHttpRequest::hasHeader(std::string_view name) const {
    return std::any_of(headers.begin(), headers.end(), [name](const auto& entry) {
        return equalsIgnoreCase(entry.first, name);
    });
}

std::unique_ptr<GenerativeHttpTransport> makeUnavailableGenerativeHttpTransport() {
    return std::make_unique<UnavailableGenerativeHttpTransport>();
}

// ---------------------------------------------------------------------------
// Status mapping
// ---------------------------------------------------------------------------

Error mapGenerativeHttpStatus(int status, std::string_view detail) {
    const std::string suffix = detail.empty() ? std::string{} : (": " + std::string(detail));
    const std::string code = "HTTP " + std::to_string(status);

    if (status == 400 || status == 422) {
        return makeError(ErrorCode::InvalidArgument,
                         "the generative endpoint rejected the request as malformed (" + code +
                             ")" + suffix);
    }
    if (status == 401) {
        return makeError(ErrorCode::Unauthenticated,
                         "the generative endpoint rejected the supplied credential (" + code +
                             ")" + suffix);
    }
    if (status == 403) {
        return makeError(ErrorCode::PermissionDenied,
                         "the generative endpoint refused the request (" + code + ")" + suffix);
    }
    if (status == 404) {
        return makeError(ErrorCode::NotFound,
                         "the generative endpoint reported the job as unknown (" + code + ")" +
                             suffix);
    }
    if (status == 408 || status == 504) {
        return makeError(ErrorCode::Timeout,
                         "the generative endpoint timed out (" + code + ")" + suffix);
    }
    if (status == 409 || status == 429 || status == 503) {
        return makeError(ErrorCode::FailedPrecondition,
                         "the generative endpoint is not currently able to serve the request (" +
                             code + ")" + suffix);
    }
    if (status == 501) {
        return makeError(ErrorCode::Unsupported,
                         "the generative endpoint does not support the request (" + code + ")" +
                             suffix);
    }
    if (status >= 500) {
        return makeError(ErrorCode::Io,
                         "the generative endpoint failed (" + code + ")" + suffix);
    }
    return makeError(ErrorCode::Unknown,
                     "the generative endpoint returned an unexpected status (" + code + ")" +
                         suffix);
}

// ---------------------------------------------------------------------------
// HttpGenerativeJobProtocol
// ---------------------------------------------------------------------------

HttpGenerativeJobProtocol::HttpGenerativeJobProtocol(GenerativeHttpTransport& transport,
                                                     Options options)
    : transport_(transport), options_(std::move(options)) {}

std::vector<std::pair<std::string, std::string>> HttpGenerativeJobProtocol::headersFor(
    std::string_view credential) const {
    std::vector<std::pair<std::string, std::string>> headers;
    headers.reserve(options_.extraHeaders.size() + 4);

    // The credential is injected here and nowhere else, from the value the caller
    // loaded moments ago. There is no compiled-in fallback (Requirement 12.6).
    if (options_.scheme == GenerativeAuthScheme::BearerAuthorization) {
        headers.emplace_back("Authorization", "Bearer " + std::string(credential));
    } else {
        headers.emplace_back(options_.apiKeyHeaderName, std::string(credential));
    }

    headers.emplace_back("Accept", "application/json");
    headers.emplace_back("Content-Type", "application/json");
    headers.emplace_back("User-Agent", "palmier-pro-linux");
    for (const auto& extra : options_.extraHeaders) headers.push_back(extra);
    return headers;
}

Result<std::string> HttpGenerativeJobProtocol::urlFor(std::string_view suffix) const {
    if (options_.endpoint.baseUrl.empty()) {
        return err<std::string>(failedPrecondition(
            "no generative endpoint is configured; no network connection was attempted"));
    }
    if (!isHttpsUrl(options_.endpoint.baseUrl)) {
        return err<std::string>(invalidArgument(
            "the configured generative endpoint is not an https:// URL, so no request was "
            "sent"));
    }
    return Result<std::string>(options_.endpoint.baseUrl + options_.endpoint.jobsPath +
                               std::string(suffix));
}

Result<GenerativeHttpRequest> HttpGenerativeJobProtocol::submitRequest(
    const GenerationRequest& request, std::string_view credential) const {
    Result<std::string> url = urlFor("");
    if (url.isError()) return err<GenerativeHttpRequest>(url.error());

    Json body = Json::object();
    body.set("model", request.model);
    body.set("mediaType", std::string(toStringView(request.mediaType)));
    body.set("prompt", request.prompt);
    Json params = Json::object();
    for (const auto& [key, value] : request.params) params.set(key, value);
    body.set("params", std::move(params));

    GenerativeHttpRequest out;
    out.method = "POST";
    out.url = std::move(url).value();
    out.headers = headersFor(credential);
    out.body = body.dump();
    return Result<GenerativeHttpRequest>(std::move(out));
}

Result<GenerativeHttpRequest> HttpGenerativeJobProtocol::pollRequest(
    const JobId& id, std::string_view credential) const {
    if (id.empty()) {
        return err<GenerativeHttpRequest>(invalidArgument(
            "a generation job identifier is required; no request was sent"));
    }
    Result<std::string> url = urlFor("/" + id.value);
    if (url.isError()) return err<GenerativeHttpRequest>(url.error());

    GenerativeHttpRequest out;
    out.method = "GET";
    out.url = std::move(url).value();
    out.headers = headersFor(credential);
    return Result<GenerativeHttpRequest>(std::move(out));
}

Result<GenerativeHttpRequest> HttpGenerativeJobProtocol::resultRequest(
    const JobId& id, std::string_view credential) const {
    if (id.empty()) {
        return err<GenerativeHttpRequest>(invalidArgument(
            "a generation job identifier is required; no request was sent"));
    }
    Result<std::string> url = urlFor("/" + id.value + "/result");
    if (url.isError()) return err<GenerativeHttpRequest>(url.error());

    GenerativeHttpRequest out;
    out.method = "GET";
    out.url = std::move(url).value();
    out.headers = headersFor(credential);
    return Result<GenerativeHttpRequest>(std::move(out));
}

Result<GenerativeHttpRequest> HttpGenerativeJobProtocol::cancelRequest(
    const JobId& id, std::string_view credential) const {
    if (id.empty()) {
        return err<GenerativeHttpRequest>(invalidArgument(
            "a generation job identifier is required; no request was sent"));
    }
    Result<std::string> url = urlFor("/" + id.value + "/cancel");
    if (url.isError()) return err<GenerativeHttpRequest>(url.error());

    GenerativeHttpRequest out;
    out.method = "POST";
    out.url = std::move(url).value();
    out.headers = headersFor(credential);
    out.body = Json::object().dump();
    return Result<GenerativeHttpRequest>(std::move(out));
}

Result<JobId> HttpGenerativeJobProtocol::submit(const GenerationRequest& request,
                                                std::string_view credential) {
    Result<GenerativeHttpRequest> built = submitRequest(request, credential);
    if (built.isError()) return err<JobId>(built.error());

    Result<GenerativeHttpResponse> sent = transport_.send(built.value());
    if (sent.isError()) return err<JobId>(sent.error());

    const GenerativeHttpResponse& response = sent.value();
    if (!isSuccess(response.status)) {
        return err<JobId>(mapGenerativeHttpStatus(response.status, providerDetail(response)));
    }

    Result<Json> body = parseBody(response, "the generation submission");
    if (body.isError()) return err<JobId>(body.error());

    const std::string id = jobIdFrom(body.value());
    if (id.empty()) {
        return err<JobId>(makeError(
            ErrorCode::Internal,
            "the generative endpoint accepted the request but returned no job identifier"));
    }
    return Result<JobId>(JobId{id});
}

Result<GenerationStatus> HttpGenerativeJobProtocol::poll(const JobId& id,
                                                         std::string_view credential) {
    Result<GenerativeHttpRequest> built = pollRequest(id, credential);
    if (built.isError()) return err<GenerationStatus>(built.error());

    Result<GenerativeHttpResponse> sent = transport_.send(built.value());
    if (sent.isError()) return err<GenerationStatus>(sent.error());

    const GenerativeHttpResponse& response = sent.value();
    if (!isSuccess(response.status)) {
        return err<GenerationStatus>(
            mapGenerativeHttpStatus(response.status, providerDetail(response)));
    }

    Result<Json> body = parseBody(response, "a job status poll");
    if (body.isError()) return err<GenerationStatus>(body.error());

    GenerationStatus status;
    const std::string token = body.value().stringOr("status");
    if (!phaseFrom(token, status.phase)) {
        return err<GenerationStatus>(makeError(
            ErrorCode::Internal,
            "the generative endpoint reported an unrecognised job status for job " + id.value));
    }
    status.progressPercent =
        static_cast<int>(std::clamp<std::int64_t>(body.value().intOr("progress", 0), 0, 100));
    if (status.phase == GenerationPhase::Failed) {
        status.failureReason = body.value().stringOr("reason");
        if (status.failureReason.empty()) {
            status.failureReason = "the generative endpoint reported the job as failed without "
                                   "a reason";
        }
    }
    return Result<GenerationStatus>(std::move(status));
}

Result<MediaAsset> HttpGenerativeJobProtocol::fetchResult(const JobId& id,
                                                          std::string_view credential) {
    Result<GenerativeHttpRequest> built = resultRequest(id, credential);
    if (built.isError()) return err<MediaAsset>(built.error());

    Result<GenerativeHttpResponse> sent = transport_.send(built.value());
    if (sent.isError()) return err<MediaAsset>(sent.error());

    const GenerativeHttpResponse& response = sent.value();
    if (!isSuccess(response.status)) {
        return err<MediaAsset>(mapGenerativeHttpStatus(response.status, providerDetail(response)));
    }

    Result<Json> body = parseBody(response, "a job result fetch");
    if (body.isError()) return err<MediaAsset>(body.error());

    const std::string sourcePath = body.value().stringOr(
        "sourcePath", body.value().stringOr("url"));
    if (sourcePath.empty()) {
        return err<MediaAsset>(makeError(
            ErrorCode::Internal,
            "the generative endpoint reported job " + id.value +
                " as succeeded but returned no media locator"));
    }

    const std::string assetId = body.value().stringOr("assetId");
    const std::optional<Uuid> parsedId = Uuid::parse(assetId);

    MediaAsset asset{MediaAssetRef{parsedId.value_or(Uuid::generateV4()), sourcePath},
                     GenerationMediaType::Video};
    const std::string kind = body.value().stringOr("mediaType", "video");
    asset.mediaType = (kind == "image") ? GenerationMediaType::Image : GenerationMediaType::Video;
    return Result<MediaAsset>(std::move(asset));
}

Result<void> HttpGenerativeJobProtocol::cancel(const JobId& id, std::string_view credential) {
    Result<GenerativeHttpRequest> built = cancelRequest(id, credential);
    if (built.isError()) return err<void>(built.error());

    Result<GenerativeHttpResponse> sent = transport_.send(built.value());
    if (sent.isError()) return err<void>(sent.error());

    const GenerativeHttpResponse& response = sent.value();
    // Cancelling a job the endpoint has already forgotten is a success: the job is
    // not running, which is what the caller asked for.
    if (!isSuccess(response.status) && response.status != 404 && response.status != 409) {
        return err<void>(mapGenerativeHttpStatus(response.status, providerDetail(response)));
    }
    return ok();
}

}  // namespace palmier::services
