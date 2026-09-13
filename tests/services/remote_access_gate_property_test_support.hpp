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

#include <unistd.h>  // getpid(), for per-process temp paths

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

// Shared fixtures and helpers for the remote_access_gate_property_test cases, split out so that no single file
// exceeds this repository's 1000-line ceiling.
//
// The support namespace opens INSIDE the original enclosing namespace, because these
// helpers name the types they use unqualified. `inline` and named rather than an
// anonymous namespace: an anonymous namespace in a header gives every translation unit
// its own copy, and a helper only some partitions use would warn as unused.

#ifndef PALMIER_TESTS_SERVICES_REMOTE_ACCESS_GATE_PROPERTY_TEST_SUPPORT_HPP_
#define PALMIER_TESTS_SERVICES_REMOTE_ACCESS_GATE_PROPERTY_TEST_SUPPORT_HPP_

#endif

namespace palmier::services {
namespace testsupport_remoteaccessgatepropertytest {

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
inline bool containsFragmentOf(const std::string& haystack, const std::string& needle,
                               std::size_t window = kFragmentWindow) {
    if (needle.size() < window) return false;
    for (std::size_t i = 0; i + window <= needle.size(); ++i) {
        if (haystack.find(needle.substr(i, window)) != std::string::npos) return true;
    }
    return false;
}

inline std::string joinPrerequisites(const BindDecision& decision) {
    std::string out;
    for (const std::string& entry : decision.unmetPrerequisites) {
        out += entry;
        out += '\n';
    }
    return out;
}

/// Every string a rejection record carries, concatenated — the whole surface a
/// credential could have leaked into (the record type has no other field).
inline std::string flatten(const RejectionRecord& record) {
    return record.timestampUtc + ' ' + record.sourceAddress + ' ' + record.reasonCode;
}

// ---------------------------------------------------------------------------
// Generators
// ---------------------------------------------------------------------------

/// A printable-ASCII string of exactly `length` characters (0x20-0x7E), which is
/// the character class Requirement 10.2 accepts.
inline rc::Gen<std::string> printableAscii(std::size_t length) {
    return rc::gen::container<std::string>(length, rc::gen::inRange<char>(0x20, 0x7F));
}

/// A bearer token of `length` printable characters, with one character replaced by
/// a byte outside the accepted class when `poisoned` is set — so a token is
/// acceptable exactly when its length is in 32..512 and it was not poisoned.
enum class TokenCharset { Printable, Control, NonAscii };

inline std::string generateToken(std::size_t length, TokenCharset charset) {
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
inline rc::Gen<std::string> nonBlankPrintableAscii(std::size_t length) {
    return rc::gen::container<std::string>(length, rc::gen::inRange<char>(0x21, 0x7F));
}

/// A token that is always acceptable AND always presentable: 32-64 printable ASCII
/// characters with no surrounding whitespace.
inline std::string acceptableToken() {
    return *nonBlankPrintableAscii(static_cast<std::size_t>(*rc::gen::inRange<int>(32, 65)));
}

enum class AddressKind { ValidIpv4, ValidIpv6, Hostname, Garbage, Empty };

/// A bind address of the requested kind. Valid literals are drawn from the
/// documentation ranges (198.51.100.0/24, 2001:db8::/32) and are deliberately
/// never loopback: a loopback literal is a *valid* address that still yields a
/// loopback binding, so it is not part of this property's space (the unit tests
/// cover it).
inline std::string generateAddress(AddressKind kind) {
    switch (kind) {
        case AddressKind::ValidIpv4:
            return "198.51.100." + std::to_string(*rc::gen::inRange<int>(1, 255));
        case AddressKind::ValidIpv6:
            return "2001:db8::" + std::to_string(*rc::gen::inRange<int>(1, 9999));
        case AddressKind::Hostname:
            return *rc::gen::element<std::string>("mcp.example.com", "localhost.localdomain",
                                                  "studio.internal", "editor.example.net");
        case AddressKind::Garbage: {
            std::string junk =
                *printableAscii(static_cast<std::size_t>(*rc::gen::inRange<int>(1, 12)));
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

inline void writeText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

#if defined(PALMIER_HAVE_OPENSSL)
/// Write a fresh P-256 private key and a matching self-signed certificate.
inline bool writeSelfSignedPair(const std::filesystem::path& certificate,
                                const std::filesystem::path& privateKey) {
    EVP_PKEY* key = EVP_EC_gen("prime256v1");
    if (key == nullptr) return false;

    X509* cert = X509_new();
    bool wrote = false;
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
                wrote =
                    PEM_write_X509(certFile, cert) == 1 &&
                    PEM_write_PrivateKey(keyFile, key, nullptr, nullptr, 0, nullptr, nullptr) == 1;
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
    bool valid_ = false;
};

inline const TlsFixture& tlsFixture() {
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
    McpServer server_;
    std::size_t invocations_ = 0;
};

inline McpRequestContext contextFrom(std::string source) {
    McpRequestContext context;
    context.sourceAddress = std::move(source);
    return context;
}

}  // namespace testsupport_remoteaccessgatepropertytest
}  // namespace palmier::services

#endif  // PALMIER_TESTS_SERVICES_REMOTE_ACCESS_GATE_PROPERTY_TEST_SUPPORT_HPP_
