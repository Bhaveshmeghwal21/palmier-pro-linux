// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/services/remote_access_gate_property_test.cpp — the universally
// quantified properties of the Remote_Access_Gate (task 6.4; design.md decision
// D4).
//
// Five properties live here, one per admission-gate claim the design makes:
//
//   Property 52 — a non-loopback bind is the conjunction of every prerequisite,
//                 and any unmet prerequisite falls back to loopback while naming
//                 what was missing and leaking no fragment of the token
//                 (Requirements 10.2, 10.3, 10.12).
//   Property 53 — on a non-loopback binding a request reaches the Tool_Surface
//                 if and only if its token matches, its Origin is acceptable and
//                 its source is not blocked (Requirements 10.4, 10.5).
//   Property 54 — every refusal is recorded completely and without the presented
//                 credential (Requirement 10.8).
//   Property 56 — a loopback-only configuration admits everything, so the
//                 current developer experience is preserved (Requirement 10.10).
//   Property 58 — the 401 rate limiter blocks the offending source alone
//                 (Requirement 10.13).
//
// Time is injected, never slept on. `RemoteAccessGate::Clock` is the gate's only
// time source, so Requirement 10.13's 60-second sliding window and its 60-second
// block are driven by advancing a variable: a case whose arrivals straddle the
// window boundary costs nothing and the boundaries are reproducible.
//
// "Reaches the Tool_Surface" is observed, not assumed. Properties 53 and 56 drive
// the gate through `McpServer::dispatchWithContext` — the same pure routing
// function the accept loop calls — with a protocol delegate that counts its own
// invocations. The gate sits strictly upstream of that delegate, so "the
// Tool_Surface invocation counter is unchanged" is measured on the production path
// rather than inferred from the `Admission` value alone.
//
// TLS material is real. When the TLS transport is compiled in, the Property 52
// fixture generates a P-256 key with a matching self-signed certificate, plus a
// second unrelated pair and an unparseable file, so "readable, parseable,
// matching pair" is decided by OpenSSL. On a build without OpenSSL the same
// generated cases assert the documented degradation instead: configuring TLS is
// itself an unmet prerequisite and the gate binds loopback.
//
// _Requirements: 10.2, 10.3, 10.4, 10.5, 10.8, 10.10, 10.12, 10.13_

#include "services/RemoteAccessGate.hpp"

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <unistd.h>   // getpid(), for per-process temp paths

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "services/Json.hpp"
#include "services/McpProtocolHandler.hpp"
#include "services/McpServer.hpp"
#include "services/TlsTransport.hpp"

#if defined(PALMIER_HAVE_OPENSSL)
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#endif

namespace palmier::services {
namespace {

using namespace std::chrono_literals;
using TimePoint = std::chrono::system_clock::time_point;

/// The credential-fragment window of Requirements 10.3 and 10.8 as the design
/// states it: no substring of the token of length 8 or more may appear.
constexpr std::size_t kFragmentWindow = 8;

// ---------------------------------------------------------------------------
// A manually advanced clock
// ---------------------------------------------------------------------------

/// The injected time source. Starting well past the epoch keeps every
/// `now - window` subtraction inside the representable range.
class ManualClock {
public:
    [[nodiscard]] RemoteAccessGate::Clock source() {
        return [this] { return now_; };
    }
    [[nodiscard]] TimePoint now() const { return now_; }
    void advance(std::chrono::seconds by) { now_ += by; }

private:
    TimePoint now_ = TimePoint(1'700'000'000'000ms);
};

// ---------------------------------------------------------------------------
// Credential-leak checking
// ---------------------------------------------------------------------------

/// Does `haystack` contain any substring of `needle` of at least `window`
/// characters?
///
/// A needle shorter than the window has no substring of that length, so the answer
/// is false by construction. That matters here and not in the example-based tests:
/// those present one fixed 40-character token, while this file *generates* tokens
/// including degenerate ones — a 1-character token of `" "` would otherwise be
/// "found" in every diagnostic that contains a space, failing a property that the
/// design states over substrings of length 8 or more.
bool containsFragmentOf(const std::string& haystack, const std::string& needle,
                        std::size_t window = kFragmentWindow) {
    if (needle.size() < window) return false;
    for (std::size_t i = 0; i + window <= needle.size(); ++i) {
        if (haystack.find(needle.substr(i, window)) != std::string::npos) return true;
    }
    return false;
}

std::string joinPrerequisites(const BindDecision& decision) {
    std::string out;
    for (const std::string& entry : decision.unmetPrerequisites) {
        out += entry;
        out += '\n';
    }
    return out;
}

/// Every string a rejection record carries, concatenated — the whole surface a
/// credential could have leaked into (the record type has no other field).
std::string flatten(const RejectionRecord& record) {
    return record.timestampUtc + ' ' + record.sourceAddress + ' ' + record.reasonCode;
}

// ---------------------------------------------------------------------------
// Generators
// ---------------------------------------------------------------------------

/// A printable-ASCII string of exactly `length` characters (0x20-0x7E), which is
/// the character class Requirement 10.2 accepts.
rc::Gen<std::string> printableAscii(std::size_t length) {
    return rc::gen::container<std::string>(length, rc::gen::inRange<char>(0x20, 0x7F));
}

/// A bearer token of `length` printable characters, with one character replaced by
/// a byte outside the accepted class when `poisoned` is set — so a token is
/// acceptable exactly when its length is in 32..512 and it was not poisoned.
enum class TokenCharset { Printable, Control, NonAscii };

std::string generateToken(std::size_t length, TokenCharset charset) {
    std::string token = *printableAscii(length);
    if (charset == TokenCharset::Printable || token.empty()) return token;
    const std::size_t index = *rc::gen::inRange<std::size_t>(0, token.size());
    token[index] = charset == TokenCharset::Control
                       ? static_cast<char>(*rc::gen::inRange<int>(1, 0x20))
                       : static_cast<char>(*rc::gen::inRange<int>(0x80, 0x100));
    return token;
}

/// Printable ASCII excluding the space (0x21-0x7E) — the characters an HTTP header
/// value can carry *positionally*. A credential is transported as the second field
/// of `Authorization: Bearer <credential>`, and HTTP strips the optional
/// whitespace around a field value (RFC 9110 OWS), so a token that begins or ends
/// with a space cannot be presented over the wire at all, however acceptable it is
/// at bind time. The properties that need a token a client can actually *send*
/// therefore draw from this class; Property 52, which only ever decides whether a
/// configured token is acceptable, keeps the full printable range including space.
rc::Gen<std::string> nonBlankPrintableAscii(std::size_t length) {
    return rc::gen::container<std::string>(length, rc::gen::inRange<char>(0x21, 0x7F));
}

/// A token that is always acceptable AND always presentable: 32-64 printable ASCII
/// characters with no surrounding whitespace.
std::string acceptableToken() {
    return *nonBlankPrintableAscii(static_cast<std::size_t>(*rc::gen::inRange<int>(32, 65)));
}

enum class AddressKind { ValidIpv4, ValidIpv6, Hostname, Garbage, Empty };

/// A bind address of the requested kind. Valid literals are drawn from the
/// documentation ranges (198.51.100.0/24, 2001:db8::/32) and are deliberately
/// never loopback: a loopback literal is a *valid* address that still yields a
/// loopback binding, so it is not part of this property's space (the unit tests
/// cover it).
std::string generateAddress(AddressKind kind) {
    switch (kind) {
        case AddressKind::ValidIpv4:
            return "198.51.100." + std::to_string(*rc::gen::inRange<int>(1, 255));
        case AddressKind::ValidIpv6:
            return "2001:db8::" + std::to_string(*rc::gen::inRange<int>(1, 9999));
        case AddressKind::Hostname:
            return *rc::gen::element<std::string>("mcp.example.com", "localhost.localdomain",
                                                  "studio.internal", "editor.example.net");
        case AddressKind::Garbage: {
            std::string junk = *printableAscii(
                static_cast<std::size_t>(*rc::gen::inRange<int>(1, 12)));
            RC_PRE(!RemoteAccessGate::isIpLiteral(junk));
            return junk;
        }
        case AddressKind::Empty:
            break;
    }
    return {};
}

// ---------------------------------------------------------------------------
// TLS material fixture
// ---------------------------------------------------------------------------

enum class TlsState { Absent, Unreadable, Unparseable, Mismatched, Valid };

void writeText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

#if defined(PALMIER_HAVE_OPENSSL)
/// Write a fresh P-256 private key and a matching self-signed certificate.
bool writeSelfSignedPair(const std::filesystem::path& certificate,
                         const std::filesystem::path& privateKey) {
    EVP_PKEY* key = EVP_EC_gen("prime256v1");
    if (key == nullptr) return false;

    X509* cert = X509_new();
    bool  wrote = false;
    if (cert != nullptr) {
        ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
        X509_gmtime_adj(X509_getm_notBefore(cert), 0);
        X509_gmtime_adj(X509_getm_notAfter(cert), 60L * 60L * 24L);
        X509_set_pubkey(cert, key);

        X509_NAME* name = X509_get_subject_name(cert);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>("palmier-property-test"),
                                   -1, -1, 0);
        X509_set_issuer_name(cert, name);

        if (X509_sign(cert, key, EVP_sha256()) > 0) {
            FILE* certFile = std::fopen(certificate.string().c_str(), "wb");
            FILE* keyFile = std::fopen(privateKey.string().c_str(), "wb");
            if (certFile != nullptr && keyFile != nullptr) {
                wrote = PEM_write_X509(certFile, cert) == 1 &&
                        PEM_write_PrivateKey(keyFile, key, nullptr, nullptr, 0, nullptr,
                                             nullptr) == 1;
            }
            if (certFile != nullptr) std::fclose(certFile);
            if (keyFile != nullptr) std::fclose(keyFile);
        }
        X509_free(cert);
    }
    EVP_PKEY_free(key);
    return wrote;
}
#endif

/// The five TLS material states, materialized on disk once per process. The
/// directory name carries the process id because `gtest_discover_tests` runs one
/// process per test case and CTest runs those in parallel.
class TlsFixture {
public:
    TlsFixture() {
        directory_ = std::filesystem::temp_directory_path() /
                     ("palmier-remote-access-prop-" + std::to_string(::getpid()));
        std::filesystem::create_directories(directory_);

        writeText(directory_ / "garbage.crt", "this is not a certificate\n");
#if defined(PALMIER_HAVE_OPENSSL)
        valid_ = writeSelfSignedPair(directory_ / "valid.crt", directory_ / "valid.key") &&
                 writeSelfSignedPair(directory_ / "other.crt", directory_ / "other.key");
#else
        // Without the TLS transport nothing can be generated or checked, and the
        // gate reports "no TLS transport" for any configured material — so plain
        // placeholder files are exactly the right fixture.
        writeText(directory_ / "valid.crt", "-----BEGIN CERTIFICATE-----\n");
        writeText(directory_ / "valid.key", "-----BEGIN PRIVATE KEY-----\n");
        writeText(directory_ / "other.key", "-----BEGIN PRIVATE KEY-----\n");
#endif
    }

    ~TlsFixture() {
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }

    TlsFixture(const TlsFixture&) = delete;
    TlsFixture& operator=(const TlsFixture&) = delete;

    /// True iff a *usable* pair could be produced, i.e. this build has the TLS
    /// transport and the fixture generated real material.
    [[nodiscard]] bool usablePairAvailable() const { return valid_ && tlsTransportAvailable(); }

    /// Apply `state` to `config`. Returns false when the state cannot be
    /// represented on this build (never happens: an unrepresentable "valid" pair
    /// simply remains an unmet prerequisite, which is the documented behaviour).
    void apply(TlsState state, RemoteAccessConfig& config) const {
        switch (state) {
            case TlsState::Absent:
                return;
            case TlsState::Unreadable:
                config.tlsCertificate = directory_ / "absent.crt";
                config.tlsPrivateKey = directory_ / "absent.key";
                return;
            case TlsState::Unparseable:
                config.tlsCertificate = directory_ / "garbage.crt";
                config.tlsPrivateKey = directory_ / "valid.key";
                return;
            case TlsState::Mismatched:
                config.tlsCertificate = directory_ / "valid.crt";
                config.tlsPrivateKey = directory_ / "other.key";
                return;
            case TlsState::Valid:
                config.tlsCertificate = directory_ / "valid.crt";
                config.tlsPrivateKey = directory_ / "valid.key";
                return;
        }
    }

private:
    std::filesystem::path directory_;
    bool                  valid_ = false;
};

const TlsFixture& tlsFixture() {
    static const TlsFixture fixture;
    return fixture;
}

// ---------------------------------------------------------------------------
// The transport the admission properties observe
// ---------------------------------------------------------------------------

/// `McpServer` with the gate wired upstream of a protocol delegate that counts
/// its invocations. `dispatchWithContext()` is pure and socket-free, so this
/// observes the real routing decision — gate first, Tool_Surface second — without
/// a listener.
class CountingTransport {
public:
    explicit CountingTransport(RemoteAccessGate& gate) {
        server_.setProtocolDelegate([this](const McpRequestContext&, std::string_view) {
            ++invocations_;
            McpReply reply;
            reply.httpStatus = 200;
            reply.body = R"({"jsonrpc":"2.0","id":1,"result":{}})";
            return reply;
        });
        server_.setRemoteAccessGate(&gate);
    }

    [[nodiscard]] HttpResponse post(const McpRequestContext& context,
                                    const std::string& body = R"({"jsonrpc":"2.0","id":1,)"
                                                              R"("method":"tools/list"})") {
        HttpRequest request;
        request.method = "POST";
        request.target = std::string(McpServer::kPath);
        request.body = body;
        return server_.dispatchWithContext(request, context);
    }

    [[nodiscard]] std::size_t invocations() const { return invocations_; }

private:
    McpServer   server_;
    std::size_t invocations_ = 0;
};

McpRequestContext contextFrom(std::string source) {
    McpRequestContext context;
    context.sourceAddress = std::move(source);
    return context;
}

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
