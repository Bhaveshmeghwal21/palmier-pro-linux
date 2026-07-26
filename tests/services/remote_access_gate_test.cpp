// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/services/remote_access_gate_test.cpp — unit tests for
// services::RemoteAccessGate and its rejection log (tasks 6.2, 6.3;
// Requirements 10.1-10.5, 10.7-10.10, 10.12, 10.13; design.md decision D4).
//
// The gate has two responsibilities and these tests keep them apart the same way
// the code does:
//
//   * the bind-time decision — what `validate()` answers for a configuration, and
//     in particular that a decision to bind beyond loopback is the conjunction of
//     every prerequisite Requirement 10.2 lists, that any unmet prerequisite falls
//     back to loopback while NAMING what was missing, and that neither the error
//     nor the log ever carries the configured token or a substring of it;
//   * the per-request decision — that a loopback-only binding admits everything
//     (the compatibility guarantee of Requirement 10.10), that a non-loopback
//     binding applies the token, origin, session-limit and rate-limit checks in
//     the specified order, and that every refusal is recorded with a timestamp, a
//     source address and a reason code and nothing else.
//
// The clock is injected throughout, so the 60-second sliding window and the
// 60-second block of Requirement 10.13 are exercised by advancing a variable
// rather than by sleeping.
//
// The TLS cases run against real material: when the TLS transport is compiled in
// the fixture generates a P-256 key and a matching self-signed certificate, so
// "loads and forms a matching pair" is checked against OpenSSL rather than
// against a stub. On a build without OpenSSL those cases instead assert the
// documented degradation — configuring TLS is an unmet prerequisite and the gate
// binds loopback — which is exactly the invariant that lets a host with no
// OpenSSL keep configuring, building and testing.

#include "services/RemoteAccessGate.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "services/TlsTransport.hpp"

#if defined(PALMIER_HAVE_OPENSSL)
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#endif

namespace palmier::services {
namespace {

using namespace std::chrono_literals;

/// A 40-character printable-ASCII token: comfortably inside the 32..512 bound of
/// Requirement 10.2 and long enough that the leak checks below have real
/// substrings to look for.
constexpr const char* kToken = "0123456789abcdef0123456789abcdef01234567";

RemoteAccessConfig enabledConfig(std::string host = "192.0.2.10") {
    RemoteAccessConfig config;
    config.enabled = true;
    config.bindAddress = std::move(host);
    config.port = 19789;
    config.bearerToken = kToken;
    config.acknowledged = true;
    return config;
}

McpRequestContext requestFrom(std::string source) {
    McpRequestContext context;
    context.sourceAddress = std::move(source);
    return context;
}

McpRequestContext bearerRequest(std::string source, const std::string& token) {
    McpRequestContext context = requestFrom(std::move(source));
    context.authorization = "Bearer " + token;
    return context;
}

/// Does `haystack` contain any substring of `needle` of at least `window`
/// characters? This is the credential-leak check of Requirements 10.3 and 10.8
/// stated directly: not "is the whole token absent" but "is no meaningful piece of
/// it present".
bool containsFragmentOf(const std::string& haystack, const std::string& needle,
                        std::size_t window = 8) {
    // An unset credential has nothing to leak, so there is nothing to look for.
    if (needle.empty()) return false;
    if (needle.size() < window) return haystack.find(needle) != std::string::npos;
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

/// A settable clock so the sliding window and the block expiry are deterministic.
class ManualClock {
public:
    [[nodiscard]] RemoteAccessGate::Clock clock() {
        return [this] { return instant_; };
    }
    void advance(std::chrono::milliseconds by) { instant_ += by; }

private:
    std::chrono::system_clock::time_point instant_ =
        std::chrono::system_clock::time_point(1'700'000'000'000ms);
};

// ---------------------------------------------------------------------------
// TLS material fixtures
// ---------------------------------------------------------------------------

std::filesystem::path uniqueDirectory(const std::string& tag) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                ("palmier-remote-access-" + tag + "-" + std::to_string(stamp));
    std::filesystem::create_directories(dir);
    return dir;
}

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
                                   reinterpret_cast<const unsigned char*>("palmier-test"), -1, -1,
                                   0);
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

// ---------------------------------------------------------------------------
// Bind-time decision (Requirements 10.1-10.3, 10.7, 10.12)
// ---------------------------------------------------------------------------

// Requirement 10.1: no explicit remote-access configuration means loopback
// 127.0.0.1:19789, plaintext, with nothing unmet to report.
TEST(RemoteAccessGateBind, AbsentConfigurationBindsLoopbackOnDefaultPort) {
    RecordingRejectionLog log;
    RemoteAccessGate      gate(RemoteAccessConfig{}, log);

    const BindDecision decision = gate.validate();
    EXPECT_EQ(decision.host, "127.0.0.1");
    EXPECT_EQ(decision.port, 19789);
    EXPECT_TRUE(decision.loopbackOnly);
    EXPECT_FALSE(decision.tlsEnabled);
    EXPECT_TRUE(decision.unmetPrerequisites.empty());
    EXPECT_FALSE(decision.plaintextWarning.has_value());
}

// Requirement 10.2 satisfied in full: the non-loopback bind happens. Requirement
// 10.7: it happens even without TLS, and one warning is offered.
TEST(RemoteAccessGateBind, AllPrerequisitesMetBindsTheConfiguredAddress) {
    RecordingRejectionLog log;
    RemoteAccessGate      gate(enabledConfig(), log);

    const BindDecision decision = gate.validate();
    EXPECT_EQ(decision.host, "192.0.2.10");
    EXPECT_FALSE(decision.loopbackOnly);
    EXPECT_FALSE(decision.tlsEnabled);
    EXPECT_TRUE(decision.unmetPrerequisites.empty());
    ASSERT_TRUE(decision.plaintextWarning.has_value());
    EXPECT_NE(decision.plaintextWarning->find("unencrypted"), std::string::npos);
    // The warning must not carry the credential it is warning about.
    EXPECT_FALSE(containsFragmentOf(*decision.plaintextWarning, kToken));
}

// Requirement 10.2 permits an IPv6 literal, so one must be bindable.
TEST(RemoteAccessGateBind, Ipv6LiteralIsAValidBindAddress) {
    RecordingRejectionLog log;
    RemoteAccessGate      gate(enabledConfig("2001:db8::1"), log);

    const BindDecision decision = gate.validate();
    EXPECT_EQ(decision.host, "2001:db8::1");
    EXPECT_FALSE(decision.loopbackOnly);
    EXPECT_TRUE(decision.unmetPrerequisites.empty());
}

// Requirement 10.7's "exactly one": however many callers ask, the warning is
// handed out once.
TEST(RemoteAccessGateBind, PlaintextWarningIsHandedOutExactlyOnce) {
    RecordingRejectionLog log;
    RemoteAccessGate      gate(enabledConfig(), log);

    EXPECT_FALSE(gate.startupWarningEmitted());
    const auto first = gate.takeStartupWarning();
    ASSERT_TRUE(first.has_value());
    EXPECT_TRUE(gate.startupWarningEmitted());
    EXPECT_FALSE(gate.takeStartupWarning().has_value());
    EXPECT_FALSE(gate.takeStartupWarning().has_value());
}

// Requirement 10.3: each unmet prerequisite is named, the fallback is loopback,
// and the token never appears — not whole, and not in fragments.
TEST(RemoteAccessGateBind, EachUnmetPrerequisiteFallsBackToLoopbackAndIsNamed) {
    RecordingRejectionLog log;

    struct Case {
        const char* label;
        const char* expectedFragment;
        RemoteAccessConfig config;
    };

    RemoteAccessConfig noAddress = enabledConfig();
    noAddress.bindAddress.clear();
    RemoteAccessConfig hostname = enabledConfig("mcp.example.com");
    RemoteAccessConfig noToken = enabledConfig();
    noToken.bearerToken.clear();
    RemoteAccessConfig shortToken = enabledConfig();
    shortToken.bearerToken = std::string(31, 'x');
    RemoteAccessConfig longToken = enabledConfig();
    longToken.bearerToken = std::string(513, 'x');
    RemoteAccessConfig controlToken = enabledConfig();
    controlToken.bearerToken = std::string(20, 'x') + std::string("\n\t") + std::string(20, 'y');
    RemoteAccessConfig notAcknowledged = enabledConfig();
    notAcknowledged.acknowledged = false;

    const Case cases[] = {
        {"no bind address", "bind address", noAddress},
        {"hostname instead of literal", "bind address", hostname},
        {"no bearer token", "bearer token", noToken},
        {"31-character token", "bearer token", shortToken},
        {"513-character token", "bearer token", longToken},
        {"token with control characters", "bearer token", controlToken},
        {"acknowledgement absent", "acknowledgement", notAcknowledged},
    };

    for (const Case& testCase : cases) {
        SCOPED_TRACE(testCase.label);
        RemoteAccessGate   gate(testCase.config, log);
        const BindDecision decision = gate.validate();

        EXPECT_EQ(decision.host, "127.0.0.1");
        EXPECT_TRUE(decision.loopbackOnly);
        EXPECT_FALSE(decision.plaintextWarning.has_value());
        ASSERT_FALSE(decision.unmetPrerequisites.empty());

        const std::string named = joinPrerequisites(decision);
        EXPECT_NE(named.find(testCase.expectedFragment), std::string::npos) << named;
        EXPECT_FALSE(containsFragmentOf(named, testCase.config.bearerToken)) << named;
    }
}

// Requirement 10.3: prerequisites are reported together, not one at a time, so an
// operator fixes one round of configuration rather than three.
TEST(RemoteAccessGateBind, EveryUnmetPrerequisiteIsReportedTogether) {
    RecordingRejectionLog log;
    RemoteAccessConfig    config;
    config.enabled = true;   // nothing else supplied

    RemoteAccessGate   gate(config, log);
    const BindDecision decision = gate.validate();

    EXPECT_TRUE(decision.loopbackOnly);
    EXPECT_EQ(decision.unmetPrerequisites.size(), 3u);
    const std::string named = joinPrerequisites(decision);
    EXPECT_NE(named.find("bind address"), std::string::npos);
    EXPECT_NE(named.find("bearer token"), std::string::npos);
    EXPECT_NE(named.find("acknowledgement"), std::string::npos);
}

// Requirement 10.12, condition 1: material that cannot be read.
TEST(RemoteAccessGateBind, UnreadableTlsMaterialIsAnUnmetPrerequisite) {
    RecordingRejectionLog log;
    const auto            dir = uniqueDirectory("unreadable");
    RemoteAccessConfig    config = enabledConfig();
    config.tlsCertificate = dir / "absent.crt";
    config.tlsPrivateKey = dir / "absent.key";

    RemoteAccessGate   gate(config, log);
    const BindDecision decision = gate.validate();

    EXPECT_TRUE(decision.loopbackOnly);
    EXPECT_FALSE(decision.tlsEnabled);
    ASSERT_EQ(decision.unmetPrerequisites.size(), 1u);
    const std::string named = joinPrerequisites(decision);
    EXPECT_NE(named.find("TLS"), std::string::npos) << named;
    EXPECT_FALSE(containsFragmentOf(named, kToken));
    std::filesystem::remove_all(dir);
}

// Requirement 10.12: only one half of the pair configured is not a usable
// configuration either.
TEST(RemoteAccessGateBind, HalfConfiguredTlsMaterialIsAnUnmetPrerequisite) {
    RecordingRejectionLog log;
    const auto            dir = uniqueDirectory("half");
    RemoteAccessConfig    config = enabledConfig();
    config.tlsCertificate = dir / "only.crt";
    writeText(*config.tlsCertificate, "-----BEGIN CERTIFICATE-----\n");

    RemoteAccessGate   gate(config, log);
    const BindDecision decision = gate.validate();

    EXPECT_TRUE(decision.loopbackOnly);
    EXPECT_FALSE(decision.tlsEnabled);
    ASSERT_EQ(decision.unmetPrerequisites.size(), 1u);
    EXPECT_NE(joinPrerequisites(decision).find("private key"), std::string::npos);
    std::filesystem::remove_all(dir);
}

// Requirement 10.12, conditions 2 and 3, plus the successful case: with the TLS
// transport compiled in the three outcomes are distinguished against real
// material. Without it, configuring TLS is itself the unmet prerequisite — the
// degradation that keeps a host without OpenSSL working.
TEST(RemoteAccessGateBind, TlsMaterialIsCheckedOrReportedUnavailable) {
    RecordingRejectionLog log;
    const auto            dir = uniqueDirectory("tls");

    const std::filesystem::path cert = dir / "server.crt";
    const std::filesystem::path key = dir / "server.key";
    const std::filesystem::path otherKey = dir / "other.key";
    const std::filesystem::path garbageCert = dir / "garbage.crt";
    writeText(garbageCert, "this is not a certificate\n");

#if defined(PALMIER_HAVE_OPENSSL)
    ASSERT_TRUE(tlsTransportAvailable());
    ASSERT_TRUE(writeSelfSignedPair(cert, key));
    const std::filesystem::path otherCert = dir / "other.crt";
    ASSERT_TRUE(writeSelfSignedPair(otherCert, otherKey));

    {   // A readable, parseable, matching pair enables TLS on the non-loopback bind.
        RemoteAccessConfig config = enabledConfig();
        config.tlsCertificate = cert;
        config.tlsPrivateKey = key;
        RemoteAccessGate   gate(config, log);
        const BindDecision decision = gate.validate();
        EXPECT_TRUE(decision.unmetPrerequisites.empty()) << joinPrerequisites(decision);
        EXPECT_FALSE(decision.loopbackOnly);
        EXPECT_TRUE(decision.tlsEnabled);
        // Requirement 10.7 offers the plaintext warning only when TLS is absent.
        EXPECT_FALSE(decision.plaintextWarning.has_value());
    }
    {   // Condition 2: readable but not parseable.
        RemoteAccessConfig config = enabledConfig();
        config.tlsCertificate = garbageCert;
        config.tlsPrivateKey = key;
        RemoteAccessGate   gate(config, log);
        const BindDecision decision = gate.validate();
        EXPECT_TRUE(decision.loopbackOnly);
        EXPECT_FALSE(decision.tlsEnabled);
        EXPECT_NE(joinPrerequisites(decision).find("cannot be parsed"), std::string::npos)
            << joinPrerequisites(decision);
    }
    {   // Condition 3: both parse, but they are not a pair.
        RemoteAccessConfig config = enabledConfig();
        config.tlsCertificate = cert;
        config.tlsPrivateKey = otherKey;
        RemoteAccessGate   gate(config, log);
        const BindDecision decision = gate.validate();
        EXPECT_TRUE(decision.loopbackOnly);
        EXPECT_FALSE(decision.tlsEnabled);
        EXPECT_NE(joinPrerequisites(decision).find("matching pair"), std::string::npos)
            << joinPrerequisites(decision);
    }
#else
    EXPECT_FALSE(tlsTransportAvailable());
    writeText(cert, "-----BEGIN CERTIFICATE-----\n");
    writeText(key, "-----BEGIN PRIVATE KEY-----\n");
    RemoteAccessConfig config = enabledConfig();
    config.tlsCertificate = cert;
    config.tlsPrivateKey = key;
    RemoteAccessGate   gate(config, log);
    const BindDecision decision = gate.validate();
    EXPECT_TRUE(decision.loopbackOnly);
    EXPECT_FALSE(decision.tlsEnabled);
    ASSERT_EQ(decision.unmetPrerequisites.size(), 1u);
    EXPECT_NE(joinPrerequisites(decision).find("no TLS transport"), std::string::npos)
        << joinPrerequisites(decision);
#endif
    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// Per-request admission (Requirements 10.4, 10.5, 10.9, 10.10)
// ---------------------------------------------------------------------------

// Requirement 10.10, the compatibility guarantee: on a loopback-only binding a
// request with neither Authorization nor Origin is admitted, and no 401 or 403 is
// producible at all — not even for a request carrying nonsense credentials.
TEST(RemoteAccessGateAdmit, LoopbackOnlyAdmitsEveryRequest) {
    RecordingRejectionLog log;
    RemoteAccessGate      gate(RemoteAccessConfig{}, log);

    EXPECT_TRUE(gate.admit(requestFrom("127.0.0.1")).allowed);

    McpRequestContext hostile = requestFrom("127.0.0.1");
    hostile.authorization = "Bearer not-the-token";
    hostile.origin = "http://evil.example.com";
    const Admission admission = gate.admit(hostile);
    EXPECT_TRUE(admission.allowed);
    EXPECT_EQ(admission.httpStatus, 200);
    EXPECT_EQ(log.size(), 0u);
}

// Requirement 10.4: absent, malformed, or not byte-identical are each 401; the
// configured token is admitted.
TEST(RemoteAccessGateAdmit, NonLoopbackRequiresTheConfiguredBearerToken) {
    RecordingRejectionLog log;
    ManualClock           clock;
    RemoteAccessGate      gate(enabledConfig(), log, clock.clock());

    const Admission noHeader = gate.admit(requestFrom("198.51.100.5"));
    EXPECT_FALSE(noHeader.allowed);
    EXPECT_EQ(noHeader.httpStatus, 401);
    EXPECT_EQ(noHeader.reason, RejectionReason::NoToken);

    McpRequestContext wrongScheme = requestFrom("198.51.100.6");
    wrongScheme.authorization = std::string("Basic ") + kToken;
    const Admission malformed = gate.admit(wrongScheme);
    EXPECT_FALSE(malformed.allowed);
    EXPECT_EQ(malformed.httpStatus, 401);
    EXPECT_EQ(malformed.reason, RejectionReason::MalformedToken);

    McpRequestContext bare = requestFrom("198.51.100.7");
    bare.authorization = kToken;   // no scheme at all
    EXPECT_EQ(gate.admit(bare).reason, RejectionReason::MalformedToken);

    // A near miss — the correct token with its last character changed — must not be
    // admitted, and the comparison must not depend on how much matched.
    std::string nearMiss = kToken;
    nearMiss.back() = nearMiss.back() == 'f' ? 'e' : 'f';
    const Admission mismatch = gate.admit(bearerRequest("198.51.100.8", nearMiss));
    EXPECT_FALSE(mismatch.allowed);
    EXPECT_EQ(mismatch.httpStatus, 401);
    EXPECT_EQ(mismatch.reason, RejectionReason::TokenMismatch);

    EXPECT_TRUE(gate.admit(bearerRequest("198.51.100.9", kToken)).allowed);
}

// Requirement 10.5: a present Origin must name the bound address, a loopback host
// or an allow-listed host; anything else is 403 and never dispatched.
TEST(RemoteAccessGateAdmit, OriginIsCheckedAgainstTheExtendedAllowList) {
    RecordingRejectionLog log;
    RemoteAccessConfig    config = enabledConfig();
    config.originAllowList = {"https://studio.example.com", "editor.example.net"};
    RemoteAccessGate gate(config, log);

    const auto admitWithOrigin = [&gate](const char* origin) {
        McpRequestContext context = bearerRequest("198.51.100.20", kToken);
        context.origin = origin;
        return gate.admit(context);
    };

    EXPECT_TRUE(gate.admit(bearerRequest("198.51.100.20", kToken)).allowed);   // absent
    EXPECT_TRUE(admitWithOrigin("http://192.0.2.10:19789").allowed);           // bound address
    EXPECT_TRUE(admitWithOrigin("http://127.0.0.1:19789").allowed);            // loopback
    EXPECT_TRUE(admitWithOrigin("http://localhost").allowed);
    EXPECT_TRUE(admitWithOrigin("https://studio.example.com").allowed);        // allow-listed
    EXPECT_TRUE(admitWithOrigin("https://STUDIO.example.com").allowed);        // case-insensitive
    EXPECT_TRUE(admitWithOrigin("https://editor.example.net:8443").allowed);

    const Admission refused = admitWithOrigin("https://evil.example.com");
    EXPECT_FALSE(refused.allowed);
    EXPECT_EQ(refused.httpStatus, 403);
    EXPECT_EQ(refused.reason, RejectionReason::OriginNotAllowed);
}

// Requirement 10.5: an unconfigured allow-list is treated as EMPTY, so the
// accepted set is exactly the bound address plus the loopback hosts.
TEST(RemoteAccessGateAdmit, UnconfiguredAllowListAcceptsOnlyBoundAndLoopbackOrigins) {
    RecordingRejectionLog log;
    RemoteAccessGate      gate(enabledConfig(), log);

    McpRequestContext context = bearerRequest("198.51.100.21", kToken);
    context.origin = "http://anything.example.com";
    EXPECT_EQ(gate.admit(context).reason, RejectionReason::OriginNotAllowed);

    context.origin = "http://192.0.2.10";
    EXPECT_TRUE(gate.admit(context).allowed);
}

// Requirement 10.9: the ceiling refuses only the request that would exceed it;
// requests continuing an established session are unaffected.
TEST(RemoteAccessGateAdmit, SessionCeilingRefusesOnlyTheExcessSessionInitiatingRequest) {
    RecordingRejectionLog log;
    RemoteAccessConfig    config = enabledConfig();
    config.maxSessions = 2;
    RemoteAccessGate gate(config, log);

    EXPECT_EQ(gate.maxSessions(), 2u);
    EXPECT_TRUE(gate.admit(bearerRequest("198.51.100.30", kToken)).allowed);
    gate.noteSessionCreated();
    EXPECT_TRUE(gate.admit(bearerRequest("198.51.100.30", kToken)).allowed);
    gate.noteSessionCreated();
    EXPECT_EQ(gate.activeSessions(), 2u);

    const Admission refused = gate.admit(bearerRequest("198.51.100.30", kToken));
    EXPECT_FALSE(refused.allowed);
    EXPECT_EQ(refused.httpStatus, 429);
    EXPECT_EQ(refused.reason, RejectionReason::SessionLimitReached);

    // An established session keeps working while the ceiling is reached.
    McpRequestContext established = bearerRequest("198.51.100.30", kToken);
    established.sessionId = "abc";
    EXPECT_TRUE(gate.admit(established).allowed);

    // Closing one frees exactly one slot.
    gate.noteSessionClosed();
    EXPECT_TRUE(gate.admit(bearerRequest("198.51.100.30", kToken)).allowed);
}

// ---------------------------------------------------------------------------
// Rejection log (Requirement 10.8)
// ---------------------------------------------------------------------------

// Requirement 10.8: a record carries a UTC millisecond timestamp, the source
// address and the reason — and excludes the presented token and every substring
// of it. The record type has no field a credential could occupy, so the assertion
// below sweeps every string it does have.
TEST(RemoteAccessGateLog, EveryRejectionIsRecordedWithoutTheCredential) {
    RecordingRejectionLog log;
    ManualClock           clock;
    RemoteAccessGate      gate(enabledConfig(), log, clock.clock());

    const std::string presented = "supersecretpresentedtokenvalue0123456789";
    EXPECT_FALSE(gate.admit(bearerRequest("203.0.113.7", presented)).allowed);

    const auto records = log.records();
    ASSERT_EQ(records.size(), 1u);
    const RejectionRecord& record = records.front();

    EXPECT_EQ(record.sourceAddress, "203.0.113.7");
    EXPECT_EQ(record.reason, RejectionReason::TokenMismatch);
    EXPECT_EQ(record.reasonCode, "token_mismatch");
    // `YYYY-MM-DDThh:mm:ss.sssZ` — 24 characters, millisecond precision, UTC.
    EXPECT_EQ(record.timestampUtc.size(), 24u);
    EXPECT_EQ(record.timestampUtc.back(), 'Z');
    EXPECT_EQ(record.timestampUtc[10], 'T');
    EXPECT_EQ(record.timestampUtc[19], '.');
    EXPECT_EQ(record.timestampUtc, formatUtcMilliseconds(record.timestamp));

    for (const std::string& field :
         {record.timestampUtc, record.sourceAddress, record.reasonCode}) {
        EXPECT_FALSE(containsFragmentOf(field, presented)) << field;
        EXPECT_FALSE(containsFragmentOf(field, std::string(kToken))) << field;
    }
}

// A highly repetitive credential is the adversarial case for a substring check, so
// it gets its own assertion.
TEST(RemoteAccessGateLog, RepetitiveCredentialsDoNotAppearInTheLogLine) {
    RecordingRejectionLog log;
    RemoteAccessGate      gate(enabledConfig(), log);

    const std::string repetitive(64, 'a');
    EXPECT_FALSE(gate.admit(bearerRequest("203.0.113.8", repetitive)).allowed);

    const auto records = log.records();
    ASSERT_EQ(records.size(), 1u);
    const std::string line =
        records.front().timestampUtc + ' ' + records.front().sourceAddress + ' ' +
        records.front().reasonCode;
    EXPECT_FALSE(containsFragmentOf(line, repetitive));
}

// Requirement 10.6: a plaintext request on a TLS listener is refused and logged
// without ever being dispatched. The transport fails the handshake before a
// request exists; the gate expresses the same refusal for any caller that reaches
// it, and records it under its own reason code.
TEST(RemoteAccessGateLog, HandshakeFailureIsRecordedAsPlaintextOnTlsPort) {
    RecordingRejectionLog log;
    RemoteAccessGate      gate(enabledConfig(), log);

    gate.noteHandshakeFailure("203.0.113.9");

    const auto records = log.records();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records.front().reason, RejectionReason::PlaintextOnTlsPort);
    EXPECT_EQ(records.front().reasonCode, "plaintext_on_tls_port");
    EXPECT_EQ(records.front().sourceAddress, "203.0.113.9");
}

// ---------------------------------------------------------------------------
// Rate limiting (Requirement 10.13)
// ---------------------------------------------------------------------------

// Requirement 10.13: five 401s from one source inside a 60-second window block
// that source for 60 seconds — including a request that carries the CORRECT token,
// which is what makes the block a block — while every other source keeps being
// served. Every rejection during the block is recorded.
TEST(RemoteAccessGateRateLimit, FiveFailuresBlockOnlyTheOffendingSourceForSixtySeconds) {
    RecordingRejectionLog log;
    ManualClock           clock;
    RemoteAccessGate      gate(enabledConfig(), log, clock.clock());

    for (int attempt = 0; attempt < 4; ++attempt) {
        const Admission admission = gate.admit(bearerRequest("203.0.113.10", "wrong"));
        EXPECT_EQ(admission.reason, RejectionReason::TokenMismatch) << attempt;
        clock.advance(1s);
    }
    // The fifth failure installs the block.
    EXPECT_EQ(gate.admit(bearerRequest("203.0.113.10", "wrong")).reason,
              RejectionReason::TokenMismatch);

    // Even the correct token is refused while the source is blocked.
    const Admission blocked = gate.admit(bearerRequest("203.0.113.10", kToken));
    EXPECT_FALSE(blocked.allowed);
    EXPECT_EQ(blocked.httpStatus, 401);
    EXPECT_EQ(blocked.reason, RejectionReason::SourceBlocked);

    // A different source is untouched.
    EXPECT_TRUE(gate.admit(bearerRequest("203.0.113.11", kToken)).allowed);

    // Still blocked just inside the 60-second block.
    clock.advance(59s);
    EXPECT_EQ(gate.admit(bearerRequest("203.0.113.10", kToken)).reason,
              RejectionReason::SourceBlocked);

    // And served again once it has elapsed.
    clock.advance(2s);
    EXPECT_TRUE(gate.admit(bearerRequest("203.0.113.10", kToken)).allowed);

    // Every refusal above was recorded: five token failures plus the two requests
    // refused while the block was in force.
    EXPECT_EQ(log.size(), 7u);
    for (const RejectionRecord& record : log.records()) {
        EXPECT_EQ(record.sourceAddress, "203.0.113.10");
    }
}

// Requirement 10.13 counts CONSECUTIVE failures, so an admitted request in the
// middle of a run means the next four failures do not block.
TEST(RemoteAccessGateRateLimit, AnAdmittedRequestEndsTheConsecutiveRun) {
    RecordingRejectionLog log;
    ManualClock           clock;
    RemoteAccessGate      gate(enabledConfig(), log, clock.clock());

    for (int attempt = 0; attempt < 4; ++attempt) {
        EXPECT_FALSE(gate.admit(bearerRequest("203.0.113.12", "wrong")).allowed);
    }
    EXPECT_TRUE(gate.admit(bearerRequest("203.0.113.12", kToken)).allowed);

    for (int attempt = 0; attempt < 4; ++attempt) {
        EXPECT_EQ(gate.admit(bearerRequest("203.0.113.12", "wrong")).reason,
                  RejectionReason::TokenMismatch)
            << attempt;
    }
    // Still not blocked: the run was broken, so this is only the fourth failure.
    EXPECT_TRUE(gate.admit(bearerRequest("203.0.113.12", kToken)).allowed);
}

// The window slides: failures older than 60 seconds are no longer evidence.
TEST(RemoteAccessGateRateLimit, FailuresOutsideTheWindowDoNotAccumulate) {
    RecordingRejectionLog log;
    ManualClock           clock;
    RemoteAccessGate      gate(enabledConfig(), log, clock.clock());

    for (int attempt = 0; attempt < 4; ++attempt) {
        EXPECT_FALSE(gate.admit(bearerRequest("203.0.113.13", "wrong")).allowed);
        clock.advance(20s);   // 0 s, 20 s, 40 s, 60 s
    }
    // At 80 s the first two failures have aged out of the 60-second window, so this
    // fifth failure does not reach the threshold.
    clock.advance(20s);
    EXPECT_EQ(gate.admit(bearerRequest("203.0.113.13", "wrong")).reason,
              RejectionReason::TokenMismatch);
    EXPECT_TRUE(gate.admit(bearerRequest("203.0.113.13", kToken)).allowed);
}

// A 403 is not an authentication failure, so it must not feed the 401 limiter.
TEST(RemoteAccessGateRateLimit, OriginRefusalsDoNotCountTowardTheAuthLimiter) {
    RecordingRejectionLog log;
    ManualClock           clock;
    RemoteAccessGate      gate(enabledConfig(), log, clock.clock());

    for (int attempt = 0; attempt < 6; ++attempt) {
        McpRequestContext context = bearerRequest("203.0.113.14", kToken);
        context.origin = "http://evil.example.com";
        EXPECT_EQ(gate.admit(context).reason, RejectionReason::OriginNotAllowed) << attempt;
    }
    EXPECT_TRUE(gate.admit(bearerRequest("203.0.113.14", kToken)).allowed);
}

// ---------------------------------------------------------------------------
// Helper predicates the gate publishes
// ---------------------------------------------------------------------------

TEST(RemoteAccessGateHelpers, IpLiteralAcceptsLiteralsAndRejectsHostnames) {
    EXPECT_TRUE(RemoteAccessGate::isIpLiteral("127.0.0.1"));
    EXPECT_TRUE(RemoteAccessGate::isIpLiteral("192.0.2.10"));
    EXPECT_TRUE(RemoteAccessGate::isIpLiteral("::1"));
    EXPECT_TRUE(RemoteAccessGate::isIpLiteral("[2001:db8::1]"));
    EXPECT_FALSE(RemoteAccessGate::isIpLiteral(""));
    EXPECT_FALSE(RemoteAccessGate::isIpLiteral("localhost"));
    EXPECT_FALSE(RemoteAccessGate::isIpLiteral("mcp.example.com"));
    EXPECT_FALSE(RemoteAccessGate::isIpLiteral("192.0.2"));
    EXPECT_FALSE(RemoteAccessGate::isIpLiteral("999.0.2.10"));
}

TEST(RemoteAccessGateHelpers, TokenBoundsAreExactlyThirtyTwoToFiveHundredTwelve) {
    EXPECT_FALSE(RemoteAccessGate::isAcceptableBearerToken(std::string(31, 'x')));
    EXPECT_TRUE(RemoteAccessGate::isAcceptableBearerToken(std::string(32, 'x')));
    EXPECT_TRUE(RemoteAccessGate::isAcceptableBearerToken(std::string(512, 'x')));
    EXPECT_FALSE(RemoteAccessGate::isAcceptableBearerToken(std::string(513, 'x')));
    EXPECT_FALSE(RemoteAccessGate::isAcceptableBearerToken(std::string(40, '\x01')));
    EXPECT_FALSE(RemoteAccessGate::isAcceptableBearerToken(std::string(40, '\x7f')));
    EXPECT_TRUE(RemoteAccessGate::isAcceptableBearerToken(std::string(40, ' ')));
}

TEST(RemoteAccessGateHelpers, ConstantTimeCompareStillCompares) {
    EXPECT_TRUE(RemoteAccessGate::constantTimeEquals("abc", "abc"));
    EXPECT_TRUE(RemoteAccessGate::constantTimeEquals("", ""));
    EXPECT_FALSE(RemoteAccessGate::constantTimeEquals("abc", "abd"));
    EXPECT_FALSE(RemoteAccessGate::constantTimeEquals("abc", "abcd"));
    EXPECT_FALSE(RemoteAccessGate::constantTimeEquals("a", std::string(257, 'a')));
    EXPECT_FALSE(RemoteAccessGate::constantTimeEquals("", "a"));
}

TEST(RemoteAccessGateHelpers, OriginHostExtractionHandlesTheFormsClientsSend) {
    EXPECT_EQ(RemoteAccessGate::originHost("http://example.com"), "example.com");
    EXPECT_EQ(RemoteAccessGate::originHost("https://Example.COM:8443"), "example.com");
    EXPECT_EQ(RemoteAccessGate::originHost("example.com"), "example.com");
    EXPECT_EQ(RemoteAccessGate::originHost("http://[2001:db8::1]:19789"), "2001:db8::1");
    EXPECT_EQ(RemoteAccessGate::originHost("::1"), "::1");
    EXPECT_EQ(RemoteAccessGate::originHost("http://example.com/path"), "example.com");
    EXPECT_EQ(RemoteAccessGate::originHost("   "), "");
}

}  // namespace
}  // namespace palmier::services
