// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/RemoteAccessGate.cpp — implementation of the Remote_Access_Gate
// (task 6.2; Requirements 10.1-10.5, 10.7-10.10, 10.12, 10.13; design.md D4).
//
// Two responsibilities, kept apart on purpose (see the header): a bind-time
// decision computed once, and a per-request decision computed for every request.
//
// One rule shapes every line below: the credential a client presented is never
// carried anywhere except the constant-time comparison. It is not a parameter of
// `reject()`, it is not a field of `RejectionRecord`, and no message built here
// interpolates the configured token either. That is how Requirement 10.8's "and
// SHALL exclude the presented token value and every substring of it" becomes a
// property of the code's shape rather than something a reviewer has to re-check
// on every change.

#include "services/RemoteAccessGate.hpp"

#include <arpa/inet.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <utility>

#include "services/TlsTransport.hpp"

namespace palmier::services {
namespace {

/// The `Bearer` credential scheme, matched case-insensitively as RFC 7235 requires.
constexpr std::string_view kBearerScheme = "bearer";

std::string toLower(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string_view trimView(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return text.substr(begin, end - begin);
}

/// Split an `Authorization` header value into its scheme and credential. Returns
/// false when the value is not `<scheme> <credential>` at all.
bool splitCredential(std::string_view header, std::string_view& scheme,
                     std::string_view& credential) {
    const std::string_view value = trimView(header);
    const std::size_t space = value.find(' ');
    if (space == std::string_view::npos) return false;
    scheme = value.substr(0, space);
    credential = trimView(value.substr(space + 1));
    return !scheme.empty() && !credential.empty();
}

}  // namespace

// ---------------------------------------------------------------------------
// Reason codes and the rejection log
// ---------------------------------------------------------------------------

std::string_view rejectionReasonCode(RejectionReason reason) noexcept {
    switch (reason) {
        case RejectionReason::NoToken:             return "no_token";
        case RejectionReason::MalformedToken:      return "malformed_token";
        case RejectionReason::TokenMismatch:       return "token_mismatch";
        case RejectionReason::OriginNotAllowed:    return "origin_not_allowed";
        case RejectionReason::SessionLimitReached: return "session_limit_reached";
        case RejectionReason::SourceBlocked:       return "source_blocked";
        case RejectionReason::PlaintextOnTlsPort:  return "plaintext_on_tls_port";
    }
    return "unknown";
}

std::string_view rejectionReasonMessage(RejectionReason reason) noexcept {
    switch (reason) {
        case RejectionReason::NoToken:
            return "the request carried no Authorization header";
        case RejectionReason::MalformedToken:
            return "the Authorization header is not a Bearer credential";
        case RejectionReason::TokenMismatch:
            return "the presented bearer token was not accepted";
        case RejectionReason::OriginNotAllowed:
            return "the Origin header names a host outside the allow-list";
        case RejectionReason::SessionLimitReached:
            return "the concurrent session limit was reached";
        case RejectionReason::SourceBlocked:
            return "this source address is temporarily blocked after repeated "
                   "authentication failures";
        case RejectionReason::PlaintextOnTlsPort:
            return "a plaintext request arrived on a TLS port";
    }
    return "the request was refused";
}

std::string formatUtcMilliseconds(std::chrono::system_clock::time_point when) {
    using namespace std::chrono;
    const auto sinceEpoch = when.time_since_epoch();
    const auto secondsPart = duration_cast<seconds>(sinceEpoch);
    auto millisPart = duration_cast<milliseconds>(sinceEpoch - secondsPart);
    // A pre-epoch instant yields a negative remainder; normalise so the rendered
    // millisecond field is always in [0, 1000).
    auto wholeSeconds = static_cast<std::time_t>(secondsPart.count());
    if (millisPart.count() < 0) {
        millisPart += milliseconds(1000);
        wholeSeconds -= 1;
    }

    std::tm utc{};
    ::gmtime_r(&wholeSeconds, &utc);

    std::array<char, 32> buffer{};
    // Millisecond precision, explicitly UTC (Requirement 10.8).
    std::snprintf(buffer.data(), buffer.size(), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                  utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min,
                  utc.tm_sec, static_cast<int>(millisPart.count()));
    return std::string(buffer.data());
}

void RecordingRejectionLog::record(const RejectionRecord& record) {
    const std::lock_guard<std::mutex> lock(mutex_);
    records_.push_back(record);
}

std::vector<RejectionRecord> RecordingRejectionLog::records() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return records_;
}

std::size_t RecordingRejectionLog::size() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return records_.size();
}

void RecordingRejectionLog::clear() {
    const std::lock_guard<std::mutex> lock(mutex_);
    records_.clear();
}

StreamRejectionLog::StreamRejectionLog(std::ostream& out) : out_(&out) {}

StreamRejectionLog::StreamRejectionLog() : out_(&std::clog) {}

void StreamRejectionLog::record(const RejectionRecord& record) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (out_ == nullptr) return;
    // Three fields, nothing else: timestamp, source, reason code.
    (*out_) << record.timestampUtc << " palmier.mcp.remote-access rejected source="
            << (record.sourceAddress.empty() ? std::string("unknown") : record.sourceAddress)
            << " reason=" << record.reasonCode << '\n';
    out_->flush();
}

// ---------------------------------------------------------------------------
// RemoteAccessGate — construction and the bind-time decision
// ---------------------------------------------------------------------------

RemoteAccessGate::RemoteAccessGate(RemoteAccessConfig config, RejectionLog& log)
    : RemoteAccessGate(std::move(config), log, Clock{}) {}

RemoteAccessGate::RemoteAccessGate(RemoteAccessConfig config, RejectionLog& log, Clock clock)
    : config_(std::move(config)),
      log_(&log),
      clock_(std::move(clock)),
      maxSessions_(std::clamp<std::size_t>(
          config_.maxSessions <= 0 ? 1u : static_cast<std::size_t>(config_.maxSessions), 1u, 32u)) {
    // The bind decision is a startup fact, so it is computed once. That also keeps
    // the TLS material read out of `admit()`, which runs per request.
    decision_ = computeDecision();
}

bool RemoteAccessGate::isIpLiteral(std::string_view text) noexcept {
    if (text.empty()) return false;
    std::string host(text);
    // Accept the bracketed IPv6 form operators are used to writing.
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
        host = host.substr(1, host.size() - 2);
    }
    in_addr v4{};
    if (::inet_pton(AF_INET, host.c_str(), &v4) == 1) return true;
    in6_addr v6{};
    return ::inet_pton(AF_INET6, host.c_str(), &v6) == 1;
}

bool RemoteAccessGate::isLoopbackLiteral(std::string_view text) noexcept {
    if (text == "localhost" || text == "ip6-localhost") return true;
    std::string host(text);
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
        host = host.substr(1, host.size() - 2);
    }
    in_addr v4{};
    if (::inet_pton(AF_INET, host.c_str(), &v4) == 1) {
        return (ntohl(v4.s_addr) >> 24) == 127;   // 127.0.0.0/8
    }
    in6_addr v6{};
    if (::inet_pton(AF_INET6, host.c_str(), &v6) == 1) {
        static constexpr unsigned char kLoop[16] = {0, 0, 0, 0, 0, 0, 0, 0,
                                                    0, 0, 0, 0, 0, 0, 0, 1};
        for (int i = 0; i < 16; ++i) {
            if (v6.s6_addr[i] != kLoop[i]) return false;
        }
        return true;
    }
    return false;
}

bool RemoteAccessGate::isAcceptableBearerToken(std::string_view token) noexcept {
    if (token.size() < kMinTokenLength || token.size() > kMaxTokenLength) return false;
    for (const char c : token) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20u || byte > 0x7Eu) return false;   // printable ASCII only
    }
    return true;
}

BindDecision RemoteAccessGate::computeDecision() const {
    // Requirement 10.1: no explicit remote-access configuration means loopback,
    // and nothing else can make that decision.
    if (!config_.enabled) return BindDecision::loopback(config_.port);

    std::vector<std::string> unmet;

    // Requirement 10.2, prerequisite 1: a syntactically valid IP literal.
    if (config_.bindAddress.empty()) {
        unmet.emplace_back("bind address: not configured");
    } else if (!isIpLiteral(config_.bindAddress)) {
        unmet.emplace_back("bind address: '" + config_.bindAddress +
                           "' is not a syntactically valid IPv4 or IPv6 literal");
    }

    // Requirement 10.2, prerequisite 2: a bearer token of 32 to 512 printable
    // ASCII characters. The message states the bound that was missed and the
    // length that was supplied, and never the value.
    if (config_.bearerToken.empty()) {
        unmet.emplace_back("bearer token: not configured");
    } else if (!isAcceptableBearerToken(config_.bearerToken)) {
        unmet.emplace_back("bearer token: rejected, a token of " +
                           std::to_string(kMinTokenLength) + " to " +
                           std::to_string(kMaxTokenLength) +
                           " printable ASCII characters is required (the configured "
                           "token has " + std::to_string(config_.bearerToken.size()) +
                           " characters)");
    }

    // Requirement 10.2, prerequisite 3: the explicit acknowledgement.
    if (!config_.acknowledged) {
        unmet.emplace_back("acknowledgement flag: not set to true");
    }

    // Requirement 10.12 plus design.md D4: when TLS material is configured it must
    // load and match, and on a build without the TLS transport, configuring TLS is
    // itself an unmet prerequisite.
    const bool wantsTls = config_.tlsCertificate.has_value() || config_.tlsPrivateKey.has_value();
    bool tlsUsable = false;
    if (wantsTls) {
        if (!config_.tlsCertificate.has_value()) {
            unmet.emplace_back("TLS certificate: not configured, but a private key is");
        } else if (!config_.tlsPrivateKey.has_value()) {
            unmet.emplace_back("TLS private key: not configured, but a certificate is");
        } else {
            const Result<void> material =
                checkTlsMaterial(*config_.tlsCertificate, *config_.tlsPrivateKey);
            if (material.isError()) {
                unmet.emplace_back("TLS material: " + material.error().message());
            } else {
                tlsUsable = true;
            }
        }
    }

    if (!unmet.empty()) {
        // Fail closed: still bind loopback, and carry the named prerequisites for
        // the startup error (Requirements 10.3, 10.12).
        BindDecision decision = BindDecision::loopback(config_.port);
        decision.unmetPrerequisites = std::move(unmet);
        return decision;
    }

    BindDecision decision;
    decision.host = config_.bindAddress;
    decision.port = config_.port;
    // An operator who enables remote access but names a loopback literal gets a
    // loopback binding, and per-request admission follows the binding rather than
    // the switch (Requirements 10.4, 10.5 are scoped to a non-loopback binding).
    decision.loopbackOnly = isLoopbackLiteral(config_.bindAddress);
    decision.tlsEnabled = tlsUsable;

    if (!tlsUsable && !decision.loopbackOnly) {
        // Requirement 10.7: the bind still happens, and one warning is offered.
        decision.plaintextWarning =
            "Remote MCP access is enabled on " + decision.host + ":" +
            std::to_string(decision.port) +
            " without TLS, so all traffic including bearer tokens is transmitted "
            "unencrypted. Configure a TLS certificate and private key, or reach the "
            "endpoint through an SSH tunnel instead.";
    }
    return decision;
}

BindDecision RemoteAccessGate::validate() const { return decision_; }

std::optional<std::string> RemoteAccessGate::takeStartupWarning() {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (warningTaken_ || !decision_.plaintextWarning.has_value()) return std::nullopt;
    warningTaken_ = true;
    return decision_.plaintextWarning;
}

bool RemoteAccessGate::startupWarningEmitted() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return warningTaken_;
}

// ---------------------------------------------------------------------------
// Per-request admission
// ---------------------------------------------------------------------------

bool RemoteAccessGate::constantTimeEquals(std::string_view lhs, std::string_view rhs) noexcept {
    // Walk the longer of the two and fold every difference — including the length
    // difference — into one accumulator, so no early exit can leak how much of a
    // candidate matched.
    const std::size_t length = std::max(lhs.size(), rhs.size());
    std::size_t difference = lhs.size() ^ rhs.size();
    for (std::size_t i = 0; i < length; ++i) {
        const auto a = i < lhs.size() ? static_cast<unsigned char>(lhs[i]) : 0u;
        const auto b = i < rhs.size() ? static_cast<unsigned char>(rhs[i]) : 0u;
        difference |= static_cast<std::size_t>(a ^ b);
    }
    return difference == 0;
}

std::string RemoteAccessGate::originHost(std::string_view origin) {
    std::string_view value = trimView(origin);
    if (value.empty()) return {};

    // Strip a scheme, if any.
    const std::size_t schemeEnd = value.find("://");
    if (schemeEnd != std::string_view::npos) value = value.substr(schemeEnd + 3);

    // Drop any path/query the header should not carry but might.
    const std::size_t slash = value.find('/');
    if (slash != std::string_view::npos) value = value.substr(0, slash);

    if (!value.empty() && value.front() == '[') {
        // Bracketed IPv6 literal, optionally followed by ":port".
        const std::size_t close = value.find(']');
        if (close != std::string_view::npos) return toLower(value.substr(1, close - 1));
        return toLower(value.substr(1));
    }

    // A single colon separates host and port; more than one means a bare IPv6
    // literal, which carries no port.
    const std::size_t firstColon = value.find(':');
    if (firstColon != std::string_view::npos &&
        value.find(':', firstColon + 1) == std::string_view::npos) {
        value = value.substr(0, firstColon);
    }
    return toLower(value);
}

bool RemoteAccessGate::originAllowed(std::string_view origin) const {
    const std::string host = originHost(origin);
    if (host.empty()) return false;

    // Requirement 10.5: an unconfigured allow-list is treated as empty, so the
    // accepted set is exactly the bound address plus the loopback hosts.
    if (isLoopbackLiteral(host)) return true;
    if (host == toLower(decision_.host)) return true;

    for (const std::string& allowed : config_.originAllowList) {
        // Allow-list entries may be written as bare hosts or as full origins.
        if (host == originHost(allowed)) return true;
    }
    return false;
}

Admission RemoteAccessGate::reject(const std::string& sourceAddress, int httpStatus,
                                   RejectionReason reason) {
    RejectionRecord record;
    record.timestamp = now();
    record.timestampUtc = formatUtcMilliseconds(record.timestamp);
    record.sourceAddress = sourceAddress;
    record.reason = reason;
    record.reasonCode = std::string(rejectionReasonCode(reason));
    if (log_ != nullptr) log_->record(record);
    return Admission::deny(httpStatus, reason);
}

std::chrono::system_clock::time_point RemoteAccessGate::now() const {
    return clock_ ? clock_() : std::chrono::system_clock::now();
}

bool RemoteAccessGate::isBlockedLocked(const std::string& sourceAddress,
                                       std::chrono::system_clock::time_point instant) {
    const auto it = sources_.find(sourceAddress);
    if (it == sources_.end()) return false;
    if (!it->second.blockedUntil.has_value()) return false;
    if (instant < *it->second.blockedUntil) return true;
    // The block has run its course: clear it and the failures that installed it, so
    // the next five failures start a fresh window.
    it->second.blockedUntil.reset();
    it->second.authFailures.clear();
    return false;
}

void RemoteAccessGate::noteAuthFailureLocked(const std::string& sourceAddress,
                                             std::chrono::system_clock::time_point instant) {
    SourceState& state = sources_[sourceAddress];
    // Requirement 10.13 counts failures inside a 60-second sliding window, so
    // anything older than the window is no longer evidence.
    const auto windowStart = instant - kAuthFailureWindow;
    auto& failures = state.authFailures;
    failures.erase(std::remove_if(failures.begin(), failures.end(),
                                  [windowStart](const auto& at) { return at < windowStart; }),
                   failures.end());
    failures.push_back(instant);

    if (failures.size() >= kAuthFailureThreshold) {
        // Block this source, and only this source, for the next 60 seconds.
        state.blockedUntil = instant + kSourceBlockDuration;
    }
}

Admission RemoteAccessGate::admit(const McpRequestContext& context) {
    // Requirement 10.10, the compatibility property: on a loopback-only binding
    // every request is admitted, so a request carrying neither Authorization nor
    // Origin is served exactly as it is today and no 401 or 403 is ever produced.
    if (decision_.loopbackOnly) return Admission::allow();

    const std::string& source = context.sourceAddress;
    const auto instant = now();

    // A TLS listener must never serve a plaintext request (Requirement 10.6). The
    // transport normally fails the handshake before a request exists at all; this
    // check is the same refusal expressed for any caller that reaches the gate with
    // an insecure request on a TLS binding.
    if (decision_.tlsEnabled && !context.secureTransport) {
        return reject(source, kStatusBadRequest, RejectionReason::PlaintextOnTlsPort);
    }

    bool blocked = false;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        blocked = isBlockedLocked(source, instant);
    }
    if (blocked) {
        // Requirement 10.13: while blocked, every further request from this source
        // is refused with 401 and recorded — including one carrying the correct
        // token — and every other source address continues to be served.
        return reject(source, kStatusUnauthorized, RejectionReason::SourceBlocked);
    }

    // Requirement 10.4: absent, malformed, or not byte-identical -> 401, and the
    // request is not dispatched.
    const auto authFailure = [&](RejectionReason reason) {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            noteAuthFailureLocked(source, instant);
        }
        return reject(source, kStatusUnauthorized, reason);
    };

    if (!context.authorization.has_value() || trimView(*context.authorization).empty()) {
        return authFailure(RejectionReason::NoToken);
    }
    std::string_view scheme;
    std::string_view credential;
    if (!splitCredential(*context.authorization, scheme, credential) ||
        toLower(scheme) != kBearerScheme) {
        return authFailure(RejectionReason::MalformedToken);
    }
    if (!constantTimeEquals(credential, config_.bearerToken)) {
        return authFailure(RejectionReason::TokenMismatch);
    }

    // Requirement 10.5: a present Origin must name an accepted host.
    if (context.origin.has_value() && !trimView(*context.origin).empty()) {
        if (!originAllowed(*context.origin)) {
            return reject(source, kStatusForbidden, RejectionReason::OriginNotAllowed);
        }
    }

    // Requirement 10.9: the concurrent-session ceiling applies to a request that
    // would establish a session. A request presenting a session identifier is
    // continuing an established session, so it is not gated by the ceiling and
    // every established session stays usable.
    const bool sessionInitiating = !context.sessionId.has_value();
    bool sessionLimitReached = false;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        sessionLimitReached = sessionInitiating && activeSessions_ >= maxSessions_;
        if (!sessionLimitReached) {
            // A fully admitted request ends the "consecutive failures" run for this
            // source, which is what makes Requirement 10.13's counter consecutive.
            const auto it = sources_.find(source);
            if (it != sources_.end() && !it->second.blockedUntil.has_value()) {
                it->second.authFailures.clear();
            }
        }
    }
    if (sessionLimitReached) {
        return reject(source, kStatusSessionLimit, RejectionReason::SessionLimitReached);
    }
    return Admission::allow();
}

void RemoteAccessGate::noteHandshakeFailure(const std::string& sourceAddress) {
    (void)reject(sourceAddress, kStatusBadRequest, RejectionReason::PlaintextOnTlsPort);
}

void RemoteAccessGate::noteSessionCreated() {
    const std::lock_guard<std::mutex> lock(mutex_);
    ++activeSessions_;
}

void RemoteAccessGate::noteSessionClosed() {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (activeSessions_ > 0) --activeSessions_;
}

std::size_t RemoteAccessGate::activeSessions() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return activeSessions_;
}

}  // namespace palmier::services
