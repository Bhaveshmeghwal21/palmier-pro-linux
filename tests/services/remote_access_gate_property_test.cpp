// SPDX-License-Identifier: GPL-3.0-or-later
//
// The remote_access_gate_property_test cases that stayed in this file after it was split.
//
// Suites here: RemoteAccessGateProperties.
// Compiled into the same target as its siblings, so every case keeps the name CTest
// already knew it by.

#include "remote_access_gate_property_test_support.hpp"

namespace palmier::services {
namespace {

using namespace palmier::services::
    testsupport_remoteaccessgatepropertytest;  // NOLINT(google-build-using-namespace)

// ===========================================================================
// Feature: end-to-end-editor-integration, Property 52: A non-loopback bind
// requires all prerequisites, and failure falls back to loopback without leaking
// the token — for any remote-access configuration, the endpoint binds a
// non-loopback address if and only if remote access is enabled and all of a
// syntactically valid IPv4/IPv6 literal, a bearer token of 32-512 printable ASCII
// characters, an acknowledgement flag set to true, and (when TLS material is
// configured) a readable, parseable, matching certificate/key pair are present;
// otherwise it binds `127.0.0.1:19789`, the startup error names every unmet
// prerequisite, and no substring of the configured token of length 8 or more
// appears anywhere in the emitted error or log.
//
// Requirement 10.2 lists the three prerequisites; Requirement 10.3 states the
// fallback ("SHALL bind `127.0.0.1` on port 19789 instead") and the no-token rule;
// Requirement 10.12 adds the three TLS material failures to the same fallback.
//
// On the fallback port. Requirements 10.3 and 10.12 name port 19789 explicitly, so
// a refused non-loopback bind lands on 127.0.0.1:19789 regardless of the port that
// was configured — the operator's port choice travelled with the address they were
// not allowed to bind. A configuration that never enabled remote access is a
// different antecedent: Requirement 10.1 describes the default endpoint and
// Requirement 16.3 makes the port configurable, so the configured port is honoured
// there. Both branches are asserted below. (The implementation originally carried
// the configured port through the fallback too; that disagreed with the requirement
// text and was corrected in `RemoteAccessGate::computeDecision`.)
//
// **Validates: Requirements 10.2, 10.3, 10.12**
// ===========================================================================
RC_GTEST_PROP(RemoteAccessGateProperties, NonLoopbackBindRequiresEveryPrerequisite, ()) {
    const bool enabled = *rc::gen::element(true, false);
    const auto addressKind = *rc::gen::element(AddressKind::ValidIpv4, AddressKind::ValidIpv6,
                                               AddressKind::Hostname, AddressKind::Garbage,
                                               AddressKind::Empty);
    const auto tokenLength = static_cast<std::size_t>(
        *rc::gen::element(0, 1, 31, 32, 33, 100, 511, 512, 513, 700));
    const auto charset = *rc::gen::element(TokenCharset::Printable, TokenCharset::Control,
                                           TokenCharset::NonAscii);
    const bool acknowledged = *rc::gen::element(true, false);
    const auto tlsState = *rc::gen::element(TlsState::Absent, TlsState::Unreadable,
                                            TlsState::Unparseable, TlsState::Mismatched,
                                            TlsState::Valid);
    const auto port = static_cast<std::uint16_t>(*rc::gen::element(19789, 20001, 45123));

    RemoteAccessConfig config;
    config.enabled = enabled;
    config.bindAddress = generateAddress(addressKind);
    config.bearerToken = generateToken(tokenLength, charset);
    config.acknowledged = acknowledged;
    config.port = port;
    tlsFixture().apply(tlsState, config);

    // The model: each prerequisite decided independently of the gate.
    const bool addressOk = RemoteAccessGate::isIpLiteral(config.bindAddress);
    const bool tokenOk = tokenLength >= RemoteAccessGate::kMinTokenLength &&
                         tokenLength <= RemoteAccessGate::kMaxTokenLength &&
                         charset == TokenCharset::Printable;
    const bool tlsConfigured = tlsState != TlsState::Absent;
    const bool tlsOk = !tlsConfigured ||
                       (tlsState == TlsState::Valid && tlsFixture().usablePairAvailable());
    const bool expectNonLoopback = enabled && addressOk && tokenOk && acknowledged && tlsOk;

    RecordingRejectionLog log;
    ManualClock           clock;
    RemoteAccessGate      gate(config, log, clock.source());
    const BindDecision    decision = gate.validate();

    // `validate()` is pure: a bind decision is a startup fact.
    RC_ASSERT(decision.host == gate.validate().host);
    RC_ASSERT(decision.port == gate.validate().port);

    if (expectNonLoopback) {
        RC_ASSERT(decision.host == config.bindAddress);
        RC_ASSERT(decision.port == port);
        RC_ASSERT(!decision.loopbackOnly);
        RC_ASSERT(decision.unmetPrerequisites.empty());
        // Requirement 10.6/10.12: TLS is served only when material loaded and
        // matched; Requirement 10.7 warns exactly when it did not.
        RC_ASSERT(decision.tlsEnabled == (tlsConfigured && tlsOk));
        RC_ASSERT(decision.plaintextWarning.has_value() == !decision.tlsEnabled);
    } else {
        RC_ASSERT(decision.host == "127.0.0.1");
        RC_ASSERT(decision.loopbackOnly);
        RC_ASSERT(!decision.tlsEnabled);
        RC_ASSERT(!decision.plaintextWarning.has_value());
        if (enabled) {
            // Requirement 10.3: the fallback endpoint is 127.0.0.1:19789.
            RC_ASSERT(decision.port == 19789);
            // Every unmet prerequisite is named, and only the unmet ones.
            const std::string named = joinPrerequisites(decision);
            RC_ASSERT(!decision.unmetPrerequisites.empty());
            RC_ASSERT((named.find("bind address") != std::string::npos) == !addressOk);
            RC_ASSERT((named.find("bearer token") != std::string::npos) == !tokenOk);
            RC_ASSERT((named.find("acknowledgement") != std::string::npos) == !acknowledged);
            RC_ASSERT((named.find("TLS") != std::string::npos) == !tlsOk);
            // Requirement 10.3: not the token, and not a fragment of it either.
            RC_ASSERT(!containsFragmentOf(named, config.bearerToken));
        } else {
            // Requirement 10.1 / 16.3: the default endpoint on the configured port.
            RC_ASSERT(decision.port == port);
            RC_ASSERT(decision.unmetPrerequisites.empty());
        }
    }

    // A bind decision rejects nothing, so nothing has been logged yet — and there
    // is therefore no log line a token could have leaked into.
    RC_ASSERT(log.size() == 0u);
}
// ===========================================================================
// Feature: end-to-end-editor-integration, Property 53: Non-loopback admission is
// exactly the conjunction of its checks — for any request arriving on a
// non-loopback binding, it is dispatched to the Tool_Surface if and only if its
// `Authorization` header is byte-identical to the configured bearer token, its
// `Origin` header is absent or names a host in the allow-list extended with the
// bound address and the loopback hosts, and its source address is not blocked;
// otherwise the response carries HTTP 401 for a token failure and HTTP 403 for an
// origin failure, is produced within 500 milliseconds, and the Tool_Surface
// invocation counter is unchanged.
//
// Requirement 10.4 gives the 401 and the 500-millisecond bound and forbids
// dispatch; Requirement 10.5 gives the 403 and the extended allow-list, with an
// unconfigured allow-list treated as empty.
//
// The counter is real: the request travels `McpServer::dispatchWithContext`, where
// the gate is wired strictly upstream of the protocol delegate, so a denied
// request provably never reaches the surface.
//
// **Validates: Requirements 10.4, 10.5**
// ===========================================================================
RC_GTEST_PROP(RemoteAccessGateProperties, NonLoopbackAdmissionIsTheConjunctionOfItsChecks, ()) {
    enum class AuthKind { Absent, Empty, WrongScheme, NearMiss, Bare, Correct };
    enum class OriginKind { Absent, Empty, BoundAddress, Loopback, AllowListed, NotAllowed };

    const auto authKind = *rc::gen::element(AuthKind::Absent, AuthKind::Empty,
                                            AuthKind::WrongScheme, AuthKind::NearMiss,
                                            AuthKind::Bare, AuthKind::Correct);
    const auto originKind = *rc::gen::element(OriginKind::Absent, OriginKind::Empty,
                                              OriginKind::BoundAddress, OriginKind::Loopback,
                                              OriginKind::AllowListed, OriginKind::NotAllowed);
    const bool sourceBlocked = *rc::gen::element(true, false);

    // 0-5 allow-list hosts (Requirement 10.5 treats an unconfigured list as empty).
    static const std::vector<std::string> pool = {
        "https://studio.example.com", "editor.example.net", "https://desk.example.org:8443",
        "console.example.com",        "https://lab.example.net"};
    const auto allowCount = static_cast<std::size_t>(*rc::gen::inRange<int>(0, 6));

    RemoteAccessConfig config;
    config.enabled = true;
    config.bindAddress = "198.51.100." + std::to_string(*rc::gen::inRange<int>(1, 255));
    config.port = 19789;
    config.bearerToken = acceptableToken();
    config.acknowledged = true;
    config.originAllowList.assign(pool.begin(),
                                  pool.begin() + static_cast<std::ptrdiff_t>(allowCount));

    RecordingRejectionLog log;
    ManualClock           clock;
    RemoteAccessGate      gate(config, log, clock.source());
    RC_ASSERT(!gate.validate().loopbackOnly);

    const std::string source = "203.0.113." + std::to_string(*rc::gen::inRange<int>(1, 255));

    // Requirement 10.13's block is one of the three conjuncts, so half the cases
    // install it first — five failures from this source inside the window.
    if (sourceBlocked) {
        for (std::size_t attempt = 0; attempt < RemoteAccessGate::kAuthFailureThreshold;
             ++attempt) {
            McpRequestContext failing = contextFrom(source);
            failing.authorization = "Bearer definitely-not-the-configured-token";
            RC_ASSERT(!gate.admit(failing).allowed);
            clock.advance(1s);
        }
        log.clear();
    }

    // The Authorization header under test.
    std::optional<std::string> authorization;
    switch (authKind) {
        case AuthKind::Absent:
            break;
        case AuthKind::Empty:
            authorization = "   ";
            break;
        case AuthKind::WrongScheme:
            authorization = "Basic " + config.bearerToken;
            break;
        case AuthKind::NearMiss: {
            std::string near = config.bearerToken;
            const std::size_t index = *rc::gen::inRange<std::size_t>(0, near.size());
            near[index] = near[index] == 'x' ? 'y' : 'x';
            RC_PRE(near != config.bearerToken);
            authorization = "Bearer " + near;
            break;
        }
        case AuthKind::Bare:
            authorization = config.bearerToken;   // no scheme at all
            break;
        case AuthKind::Correct:
            authorization = "Bearer " + config.bearerToken;
            break;
    }

    // The Origin header under test.
    std::optional<std::string> origin;
    bool originAcceptable = true;
    switch (originKind) {
        case OriginKind::Absent:
            break;
        case OriginKind::Empty:
            origin = "  ";   // present but blank: treated as absent
            break;
        case OriginKind::BoundAddress:
            origin = "http://" + config.bindAddress + ":19789";
            break;
        case OriginKind::Loopback:
            origin = *rc::gen::element<std::string>("http://127.0.0.1:19789", "http://localhost",
                                                    "http://[::1]:19789");
            break;
        case OriginKind::AllowListed:
            if (allowCount == 0) {
                // Nothing to draw from, so this is the not-allowed case instead.
                origin = "https://unlisted.example.com";
                originAcceptable = false;
            } else {
                origin = config.originAllowList[
                    static_cast<std::size_t>(*rc::gen::inRange<std::size_t>(0, allowCount))];
            }
            break;
        case OriginKind::NotAllowed:
            origin = "https://evil.example.com";
            originAcceptable = false;
            break;
    }

    const bool tokenAcceptable = authKind == AuthKind::Correct;

    McpRequestContext context = contextFrom(source);
    context.authorization = authorization;
    context.origin = origin;
    // An established session, so Requirement 10.9's ceiling is not a conjunct here.
    context.sessionId = std::string(64, 'a');

    CountingTransport transport(gate);
    const auto        started = std::chrono::steady_clock::now();
    const HttpResponse response = transport.post(context);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    // Requirement 10.4's 500-millisecond bound, measured on real time.
    RC_ASSERT(elapsed < 500ms);

    const bool expectedAllowed = tokenAcceptable && originAcceptable && !sourceBlocked;
    if (expectedAllowed) {
        RC_ASSERT(response.status == 200);
        RC_ASSERT(transport.invocations() == 1u);
        RC_ASSERT(log.size() == 0u);
    } else {
        // Not dispatched: the Tool_Surface invocation counter is unchanged.
        RC_ASSERT(transport.invocations() == 0u);
        RC_ASSERT(log.size() == 1u);

        if (sourceBlocked) {
            RC_ASSERT(response.status == RemoteAccessGate::kStatusUnauthorized);
            RC_ASSERT(log.records().front().reason == RejectionReason::SourceBlocked);
        } else if (!tokenAcceptable) {
            RC_ASSERT(response.status == RemoteAccessGate::kStatusUnauthorized);
            const RejectionReason reason = log.records().front().reason;
            const bool noCredential = authKind == AuthKind::Absent || authKind == AuthKind::Empty;
            RC_ASSERT(reason == (noCredential ? RejectionReason::NoToken
                                              : authKind == AuthKind::NearMiss
                                                    ? RejectionReason::TokenMismatch
                                                    : RejectionReason::MalformedToken));
        } else {
            RC_ASSERT(response.status == RemoteAccessGate::kStatusForbidden);
            RC_ASSERT(log.records().front().reason == RejectionReason::OriginNotAllowed);
        }
        // The refusal body names the reason code and nothing the client presented.
        RC_ASSERT(!containsFragmentOf(response.body, config.bearerToken));
    }
}
// ===========================================================================
// Feature: end-to-end-editor-integration, Property 54: Every rejection is logged
// completely and without the credential — for any presented token of at least 8
// characters and any rejection reason, the log record written within 1 second of
// the rejection contains a UTC timestamp with millisecond precision, the source
// address and the rejection reason, and contains no substring of the presented
// token of length 8 or more.
//
// Requirement 10.8: "THE Remote_Access_Gate SHALL record, within 1 second of the
// rejection, the UTC timestamp with millisecond precision, the source address and
// the rejection reason in the application log, and SHALL exclude the presented
// token value and every substring of it from that record."
//
// The generated space is presented-token shape (random, highly repetitive, the
// configured token as a prefix) x every rejection reason the gate can produce. The
// assertions sweep every string the record carries and also the line the
// application-log sink writes, so both the in-memory record and the emitted text
// are covered.
//
// **Validates: Requirements 10.8**
// ===========================================================================
RC_GTEST_PROP(RemoteAccessGateProperties, EveryRejectionIsLoggedWithoutTheCredential, ()) {
    enum class TokenShape { Random, Repetitive, ConfiguredPrefix };
    const auto shape = *rc::gen::element(TokenShape::Random, TokenShape::Repetitive,
                                         TokenShape::ConfiguredPrefix);
    const auto reason = *rc::gen::element(
        RejectionReason::NoToken, RejectionReason::MalformedToken, RejectionReason::TokenMismatch,
        RejectionReason::OriginNotAllowed, RejectionReason::SessionLimitReached,
        RejectionReason::SourceBlocked, RejectionReason::PlaintextOnTlsPort);

    RemoteAccessConfig config;
    config.enabled = true;
    config.bindAddress = "198.51.100.7";
    config.bearerToken = acceptableToken();
    config.acknowledged = true;
    config.maxSessions = 1;

    // The credential the client presents (at least 8 characters, so the fragment
    // check has something to look for).
    std::string presented;
    switch (shape) {
        case TokenShape::Random:
            presented = *nonBlankPrintableAscii(
                static_cast<std::size_t>(*rc::gen::inRange<int>(8, 80)));
            break;
        case TokenShape::Repetitive:
            presented = std::string(static_cast<std::size_t>(*rc::gen::inRange<int>(8, 80)),
                                    *rc::gen::inRange<char>(0x21, 0x7F));
            break;
        case TokenShape::ConfiguredPrefix:
            presented = config.bearerToken.substr(
                0, static_cast<std::size_t>(*rc::gen::inRange<int>(8, 32)));
            break;
    }
    RC_PRE(presented != config.bearerToken);

    RecordingRejectionLog log;
    ManualClock           clock;
    RemoteAccessGate      gate(config, log, clock.source());
    const std::string     source = "203.0.113." + std::to_string(*rc::gen::inRange<int>(1, 255));

    // Drive the generated reason on the production path.
    const auto correctCredential = [&] { return "Bearer " + config.bearerToken; };
    McpRequestContext context = contextFrom(source);
    context.sessionId = std::string(64, 'b');
    switch (reason) {
        case RejectionReason::NoToken:
            break;   // no Authorization header at all
        case RejectionReason::MalformedToken:
            context.authorization = "Basic " + presented;
            break;
        case RejectionReason::TokenMismatch:
            context.authorization = "Bearer " + presented;
            break;
        case RejectionReason::OriginNotAllowed:
            context.authorization = correctCredential();
            context.origin = "https://evil.example.com";
            break;
        case RejectionReason::SessionLimitReached:
            context.authorization = correctCredential();
            context.sessionId.reset();   // session-initiating
            gate.noteSessionCreated();   // ceiling of 1 already reached
            break;
        case RejectionReason::SourceBlocked: {
            for (std::size_t attempt = 0; attempt < RemoteAccessGate::kAuthFailureThreshold;
                 ++attempt) {
                McpRequestContext failing = contextFrom(source);
                failing.authorization = "Bearer " + presented;
                (void)gate.admit(failing);
                clock.advance(1s);
            }
            log.clear();
            context.authorization = correctCredential();
            break;
        }
        case RejectionReason::PlaintextOnTlsPort:
            break;   // driven through noteHandshakeFailure below
    }

    const TimePoint at = clock.now();
    if (reason == RejectionReason::PlaintextOnTlsPort) {
        gate.noteHandshakeFailure(source);
    } else {
        const Admission admission = gate.admit(context);
        RC_ASSERT(!admission.allowed);
        RC_ASSERT(admission.reason == reason);
    }

    // Exactly one record, and it is complete.
    const std::vector<RejectionRecord> records = log.records();
    RC_ASSERT(records.size() == 1u);
    const RejectionRecord& record = records.front();

    RC_ASSERT(record.sourceAddress == source);
    RC_ASSERT(record.reason == reason);
    RC_ASSERT(record.reasonCode == std::string(rejectionReasonCode(reason)));
    // `YYYY-MM-DDThh:mm:ss.sssZ`: 24 characters, UTC, millisecond precision.
    RC_ASSERT(record.timestampUtc.size() == 24u);
    RC_ASSERT(record.timestampUtc.back() == 'Z');
    RC_ASSERT(record.timestampUtc[10] == 'T');
    RC_ASSERT(record.timestampUtc[19] == '.');
    RC_ASSERT(record.timestampUtc == formatUtcMilliseconds(record.timestamp));
    // Written within 1 second of the rejection, measured on the same clock the
    // gate reads (so the bound is checked rather than approximated by wall time).
    RC_ASSERT(record.timestamp >= at);
    RC_ASSERT(record.timestamp - at < 1s);

    // No fragment of the presented credential, and none of the configured token,
    // anywhere in the record or in the line the application log emits.
    RC_ASSERT(!containsFragmentOf(flatten(record), presented));
    RC_ASSERT(!containsFragmentOf(flatten(record), config.bearerToken));

    std::ostringstream    emitted;
    StreamRejectionLog    sink(emitted);
    sink.record(record);
    RC_ASSERT(!containsFragmentOf(emitted.str(), presented));
    RC_ASSERT(!containsFragmentOf(emitted.str(), config.bearerToken));
    RC_ASSERT(emitted.str().find(record.reasonCode) != std::string::npos);
    RC_ASSERT(emitted.str().find(record.timestampUtc) != std::string::npos);
}
// ===========================================================================
// Feature: end-to-end-editor-integration, Property 56: Loopback-only
// configurations preserve the current developer experience — for all request
// sequences on a loopback-only configuration, requests carrying neither an
// `Authorization` header nor an `Origin` header are accepted, and no response
// carries HTTP 401 or HTTP 403.
//
// Requirement 10.10: "FOR ALL request sequences on a loopback-only configuration,
// THE Remote_Access_Gate SHALL accept requests that carry neither an
// `Authorization` header nor an `Origin` header, without issuing HTTP status 401
// or 403, and preserve the current default developer experience (compatibility
// property)."
//
// The generated space is the three ways a binding can end up loopback-only —
// remote access never configured (Requirement 10.1), enabled with a loopback
// literal, and enabled but falling back after an unmet prerequisite (Requirement
// 10.3) — crossed with sequences of 1-50 requests over the four MCP methods. Each
// request goes through the transport, so "no response carries 401 or 403" is
// asserted on the HTTP status the client would actually see.
//
// **Validates: Requirements 10.10**
// ===========================================================================
RC_GTEST_PROP(RemoteAccessGateProperties, LoopbackOnlyConfigurationsAdmitEveryRequest, ()) {
    enum class Kind { NeverConfigured, EnabledOnLoopback, EnabledButUnmet };
    const auto kind = *rc::gen::element(Kind::NeverConfigured, Kind::EnabledOnLoopback,
                                        Kind::EnabledButUnmet);
    const auto requestCount = static_cast<std::size_t>(*rc::gen::inRange<int>(1, 51));

    RemoteAccessConfig config;
    switch (kind) {
        case Kind::NeverConfigured:
            break;   // the Requirement 10.1 default
        case Kind::EnabledOnLoopback:
            config.enabled = true;
            config.bindAddress = *rc::gen::element<std::string>("127.0.0.1", "127.0.0.2", "::1");
            config.bearerToken = acceptableToken();
            config.acknowledged = true;
            break;
        case Kind::EnabledButUnmet:
            config.enabled = true;
            config.bindAddress = "198.51.100.9";
            // One prerequisite deliberately missing, so the gate falls back.
            config.bearerToken = *rc::gen::element(true, false) ? std::string() : acceptableToken();
            config.acknowledged = config.bearerToken.empty();
            break;
    }

    RecordingRejectionLog log;
    ManualClock           clock;
    RemoteAccessGate      gate(config, log, clock.source());
    const BindDecision    decision = gate.validate();
    RC_ASSERT(decision.loopbackOnly);
    RC_ASSERT(RemoteAccessGate::isLoopbackLiteral(decision.host));

    CountingTransport transport(gate);
    static const std::vector<std::string> methods = {"initialize", "notifications/initialized",
                                                     "tools/list", "tools/call"};

    for (std::size_t i = 0; i < requestCount; ++i) {
        const std::string method =
            methods[static_cast<std::size_t>(*rc::gen::inRange<std::size_t>(0, methods.size()))];
        Json body = Json::object();
        body.set("jsonrpc", Json("2.0"));
        body.set("id", Json(static_cast<std::int64_t>(i + 1)));
        body.set("method", Json(method));

        // Neither Authorization nor Origin — exactly the request shape a local
        // developer's client sends today.
        McpRequestContext context = contextFrom("127.0.0.1");
        RC_ASSERT(!context.authorization.has_value());
        RC_ASSERT(!context.origin.has_value());

        const Admission admission = gate.admit(context);
        RC_ASSERT(admission.allowed);
        RC_ASSERT(admission.httpStatus == 200);

        const HttpResponse response = transport.post(context, body.dump());
        RC_ASSERT(response.status != RemoteAccessGate::kStatusUnauthorized);
        RC_ASSERT(response.status != RemoteAccessGate::kStatusForbidden);
        RC_ASSERT(response.status == 200);
        // Advancing time cannot make a loopback-only binding start refusing.
        clock.advance(std::chrono::seconds(*rc::gen::inRange<int>(0, 120)));
    }

    // Every request reached the Tool_Surface, and nothing was ever rejected.
    RC_ASSERT(transport.invocations() == requestCount);
    RC_ASSERT(log.size() == 0u);
}
// ===========================================================================
// Feature: end-to-end-editor-integration, Property 58: The 401 rate limiter
// blocks the offending source only — for any arrival pattern of authentication
// failures across two source addresses, a source is blocked for 60 seconds
// starting at its fifth failure within any 60-second window and not before; every
// rejection during the block is logged; and requests from every other source
// address continue to be served.
//
// Requirement 10.13: "IF 5 consecutive requests from the same source address are
// rejected with HTTP status 401 within any 60-second window, THEN THE
// Remote_Access_Gate SHALL reject all further requests from that source address
// with HTTP status 401 for the next 60 seconds, SHALL record each such rejection
// per criterion 8, and SHALL continue serving requests from all other source
// addresses."
//
// The property runs an independent model of the sliding window per source and
// asserts the gate agrees on every single arrival — outcome, HTTP status, reason
// code and log growth — so "and not before" is checked as strictly as "for the
// next 60 seconds". Inter-arrival gaps straddle both the 60-second window and the
// 60-second block, and the two sources interleave freely, which is what makes the
// isolation claim testable.
//
// **Validates: Requirements 10.13**
// ===========================================================================
RC_GTEST_PROP(RemoteAccessGateProperties, RateLimiterBlocksTheOffendingSourceOnly, ()) {
    RemoteAccessConfig config;
    config.enabled = true;
    config.bindAddress = "198.51.100.11";
    config.bearerToken = acceptableToken();
    config.acknowledged = true;

    RecordingRejectionLog log;
    ManualClock           clock;
    RemoteAccessGate      gate(config, log, clock.source());
    RC_ASSERT(!gate.validate().loopbackOnly);

    /// The model of one source's sliding window, mirroring the gate's rules: a
    /// live block refuses everything (without extending itself), an expired block
    /// is cleared along with the failures that installed it, a failure inside the
    /// 60-second window accumulates, and an admitted request ends the run.
    struct SourceModel {
        std::vector<TimePoint> failures;
        std::optional<TimePoint> blockedUntil;
    };
    SourceModel model[2];
    const std::string addresses[2] = {"203.0.113.40", "203.0.113.41"};

    const auto steps = static_cast<std::size_t>(*rc::gen::inRange<int>(1, 40));
    std::size_t expectedLogSize = 0;

    for (std::size_t step = 0; step < steps; ++step) {
        const auto which = static_cast<std::size_t>(*rc::gen::inRange<int>(0, 2));
        const bool presentCorrectToken = *rc::gen::element(true, false);
        // Gaps that straddle both 60-second boundaries.
        const auto gap = std::chrono::seconds(
            *rc::gen::element(0, 1, 5, 30, 59, 60, 61, 90));
        clock.advance(gap);
        const TimePoint now = clock.now();

        SourceModel& m = model[which];

        // The expected outcome, derived from the model before the call.
        bool            expectAllowed = false;
        RejectionReason expectReason = RejectionReason::TokenMismatch;
        if (m.blockedUntil.has_value() && now < *m.blockedUntil) {
            expectReason = RejectionReason::SourceBlocked;
        } else {
            if (m.blockedUntil.has_value()) {
                m.blockedUntil.reset();
                m.failures.clear();
            }
            if (!presentCorrectToken) {
                const TimePoint windowStart = now - RemoteAccessGate::kAuthFailureWindow;
                m.failures.erase(std::remove_if(m.failures.begin(), m.failures.end(),
                                                [windowStart](const TimePoint& at) {
                                                    return at < windowStart;
                                                }),
                                 m.failures.end());
                m.failures.push_back(now);
                if (m.failures.size() >= RemoteAccessGate::kAuthFailureThreshold) {
                    m.blockedUntil = now + RemoteAccessGate::kSourceBlockDuration;
                }
                expectReason = RejectionReason::TokenMismatch;
            } else {
                m.failures.clear();
                expectAllowed = true;
            }
        }

        McpRequestContext context = contextFrom(addresses[which]);
        context.authorization =
            "Bearer " + (presentCorrectToken ? config.bearerToken
                                             : std::string("wrong-token-presented-by-the-client"));
        context.sessionId = std::string(64, 'c');

        const Admission admission = gate.admit(context);
        RC_ASSERT(admission.allowed == expectAllowed);
        if (!expectAllowed) {
            // Requirement 10.13: every refusal under the limiter is a 401 and is
            // recorded (criterion 8).
            RC_ASSERT(admission.httpStatus == RemoteAccessGate::kStatusUnauthorized);
            RC_ASSERT(admission.reason == expectReason);
            ++expectedLogSize;
            RC_ASSERT(log.size() == expectedLogSize);
            RC_ASSERT(log.records().back().sourceAddress == addresses[which]);
            RC_ASSERT(log.records().back().reasonCode ==
                      std::string(rejectionReasonCode(expectReason)));
        } else {
            RC_ASSERT(admission.httpStatus == 200);
            RC_ASSERT(log.size() == expectedLogSize);
        }

        // Isolation: whichever source is blocked, the OTHER one is served on the
        // very same instant when it presents the configured token — unless it is
        // itself inside a block it earned.
        const std::size_t other = 1 - which;
        const bool otherBlocked = model[other].blockedUntil.has_value() &&
                                  now < *model[other].blockedUntil;
        if (!otherBlocked) {
            McpRequestContext probe = contextFrom(addresses[other]);
            probe.authorization = "Bearer " + config.bearerToken;
            probe.sessionId = std::string(64, 'd');
            RC_ASSERT(gate.admit(probe).allowed);
            model[other].failures.clear();
            RC_ASSERT(log.size() == expectedLogSize);
        }
    }
}

}  // namespace
}  // namespace palmier::services
