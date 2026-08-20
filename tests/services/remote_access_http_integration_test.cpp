// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/services/remote_access_http_integration_test.cpp — remote-access and TLS
// integration tests (task 6.6; Requirements 10.6, 15.4).
//
// Every other remote-access test drives the gate, or the gate plus the pure
// `dispatchWithContext` routing function. This file is the only one that puts a
// real socket in front of the whole stage-6 stack and speaks HTTP to it:
//
//     ProjectSession -> buildDefaultToolRegistry -> McpToolExecutor
//                            + McpSessionRegistry -> McpProtocolHandler
//                                                          |
//                            RemoteAccessGate (strictly upstream) + McpServer
//                                                          |
//                        bind <non-loopback address>:<ephemeral>/mcp, accept()
//                                                          |
//                        real HTTP (or TLS) request from a client socket
//
// Two tests:
//
//   * Bearer enforcement over a genuinely non-loopback binding — three real
//     requests, no bearer token, a wrong token, then the configured token,
//     asserting 401, 401, 200 and a BYTE-IDENTICAL project after each rejection.
//     "Byte-identical" is `serializeProject` compared against the snapshot taken
//     before the request, which is the strongest statement available at this layer
//     and is what proves the refused requests never reached the Tool_Surface.
//   * HTTPS succeeds while plaintext on the same port is rejected
//     (Requirement 10.6) — a real TLS handshake against a generated self-signed
//     certificate, then a plaintext request to the same port, which must fail the
//     handshake, be closed without an HTTP response, and be logged as
//     `plaintext_on_tls_port`. On a build without `PALMIER_HAVE_OPENSSL` the case
//     calls `GTEST_SKIP()` with the reason recorded in the CTest output, which is
//     the documented degradation of stage 6.
//
// On the bind address. The gate decides "loopback-only" from the bind address
// itself, and 127.0.0.0/8 — including 127.0.0.2 — is loopback, so a second
// loopback address would make the gate admit unconditionally (Requirement 10.10)
// and could not exercise a 401 at all. These tests therefore bind the container's
// first real non-loopback IPv4 address, discovered at run time through
// `getifaddrs`, and connect to that same address. If a host has no non-loopback
// IPv4 address, the test skips with that reason recorded rather than silently
// asserting nothing.
//
// The HTTP client is modelled on tests/services/mcp_http_integration_test.cpp;
// the only additions are a host parameter (the endpoint is not on loopback here)
// and a receive timeout, so a connection the server closes without answering — the
// plaintext-on-TLS-port case — ends the read instead of blocking.
//
// _Requirements: 10.6, 15.4_

#include "services/McpProtocolHandler.hpp"
#include "services/McpServer.hpp"
#include "services/McpSessionRegistry.hpp"
#include "services/McpToolExecutor.hpp"
#include "services/ProjectSession.hpp"
#include "services/RemoteAccessGate.hpp"
#include "services/ToolRegistry.hpp"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/FrameRate.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/Project.hpp"
#include "core/Resolution.hpp"
#include "core/TimelineEngine.hpp"
#include "core/Track.hpp"
#include "core/Uuid.hpp"
#include "services/Json.hpp"
#include "services/ProjectStore.hpp"
#include "services/TlsTransport.hpp"

#if defined(PALMIER_HAVE_OPENSSL)
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#endif

namespace palmier::services {
namespace {

/// A 48-character printable-ASCII bearer token: inside Requirement 10.2's 32-512
/// bound, and long enough that a "wrong token of the same shape" is a real near
/// miss rather than a length mismatch.
constexpr const char* kToken = "0123456789abcdef0123456789abcdef0123456789abcdef";

// ---------------------------------------------------------------------------
// Host discovery
// ---------------------------------------------------------------------------

/// The first non-loopback IPv4 address of this host, or `std::nullopt` when the
/// host has none. A non-loopback literal is required because the gate derives
/// "loopback-only" — and therefore whether admission control applies at all — from
/// the bind address.
std::optional<std::string> firstNonLoopbackIpv4() {
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0 || list == nullptr) return std::nullopt;

    std::optional<std::string> found;
    for (ifaddrs* entry = list; entry != nullptr; entry = entry->ifa_next) {
        if (entry->ifa_addr == nullptr || entry->ifa_addr->sa_family != AF_INET) continue;
        char text[INET_ADDRSTRLEN] = {0};
        const void* addr = &reinterpret_cast<sockaddr_in*>(entry->ifa_addr)->sin_addr;
        if (::inet_ntop(AF_INET, addr, text, sizeof(text)) == nullptr) continue;
        if (RemoteAccessGate::isLoopbackLiteral(text)) continue;
        found = std::string(text);
        break;
    }
    ::freeifaddrs(list);
    return found;
}

// ---------------------------------------------------------------------------
// Minimal blocking HTTP client (modelled on mcp_http_integration_test.cpp)
// ---------------------------------------------------------------------------

struct HttpReply {
    int         status = 0;   ///< Parsed HTTP status code (0 on transport failure).
    std::string body;
    std::string raw;
    bool        ok = false;   ///< True iff any response bytes were received.

    std::vector<std::pair<std::string, std::string>> headers;   ///< Lower-cased names.

    [[nodiscard]] const std::string* header(std::string_view name) const {
        std::string wanted(name);
        for (char& c : wanted) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (const auto& [key, value] : headers) {
            if (key == wanted) return &value;
        }
        return nullptr;
    }
};

/// Build the request message for `/mcp` with the supplied headers and body.
std::string requestMessage(const std::string& host, const std::string& body,
                           const std::vector<std::pair<std::string, std::string>>& extraHeaders) {
    std::string req = "POST /mcp HTTP/1.1\r\nHost: " + host + "\r\n";
    req += "Content-Type: application/json\r\n";
    for (const auto& [name, value] : extraHeaders) req += name + ": " + value + "\r\n";
    req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "Connection: close\r\n\r\n";
    req += body;
    return req;
}

void parseReply(const std::string& resp, HttpReply& reply) {
    if (resp.empty()) return;
    reply.ok = true;
    reply.raw = resp;

    const std::size_t sp = resp.find(' ');
    if (sp != std::string::npos) {
        try {
            reply.status = std::stoi(resp.substr(sp + 1, 3));
        } catch (...) {
            reply.status = 0;
        }
    }

    const std::size_t sep = resp.find("\r\n\r\n");
    if (sep == std::string::npos) return;
    reply.body = resp.substr(sep + 4);

    const std::string headerBlock = resp.substr(0, sep);
    std::size_t       lineStart = headerBlock.find("\r\n");
    while (lineStart != std::string::npos) {
        lineStart += 2;
        const std::size_t lineEnd = headerBlock.find("\r\n", lineStart);
        const std::string line = headerBlock.substr(
            lineStart, lineEnd == std::string::npos ? std::string::npos : lineEnd - lineStart);
        const std::size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string name = line.substr(0, colon);
            for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            std::size_t valueStart = colon + 1;
            while (valueStart < line.size() && line[valueStart] == ' ') ++valueStart;
            reply.headers.emplace_back(name, line.substr(valueStart));
        }
        lineStart = lineEnd;
    }
}

/// Connect to `host:port`, with a 5-second receive timeout so a connection the
/// server closes without answering does not block the test. Returns -1 on failure.
int connectTo(const std::string& host, std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    timeval timeout{};
    timeout.tv_sec = 5;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        return -1;
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

/// One plaintext HTTP POST to `host:port/mcp`.
HttpReply httpPost(const std::string& host, std::uint16_t port, const std::string& body,
                   const std::vector<std::pair<std::string, std::string>>& extraHeaders = {}) {
    HttpReply reply;
    const int fd = connectTo(host, port);
    if (fd < 0) return reply;

    const std::string req = requestMessage(host, body, extraHeaders);
    std::size_t       sent = 0;
    while (sent < req.size()) {
        const ssize_t n = ::send(fd, req.data() + sent, req.size() - sent, 0);
        if (n <= 0) break;
        sent += static_cast<std::size_t>(n);
    }

    std::string resp;
    char        buf[4096];
    while (true) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        resp.append(buf, static_cast<std::size_t>(n));
    }
    ::close(fd);
    parseReply(resp, reply);
    return reply;
}

#if defined(PALMIER_HAVE_OPENSSL)
/// One HTTPS POST to `host:port/mcp`, verifying nothing (the certificate is
/// self-signed by the fixture) so the test asserts the transport, not a PKI.
HttpReply httpsPost(const std::string& host, std::uint16_t port, const std::string& body,
                    const std::vector<std::pair<std::string, std::string>>& extraHeaders = {}) {
    HttpReply reply;
    const int fd = connectTo(host, port);
    if (fd < 0) return reply;

    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (ctx == nullptr) {
        ::close(fd);
        return reply;
    }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);

    SSL* ssl = SSL_new(ctx);
    if (ssl == nullptr) {
        SSL_CTX_free(ctx);
        ::close(fd);
        return reply;
    }
    SSL_set_fd(ssl, fd);
    if (SSL_connect(ssl) == 1) {
        const std::string req = requestMessage(host, body, extraHeaders);
        std::size_t       sent = 0;
        while (sent < req.size()) {
            const int n = SSL_write(ssl, req.data() + sent,
                                    static_cast<int>(req.size() - sent));
            if (n <= 0) break;
            sent += static_cast<std::size_t>(n);
        }

        std::string resp;
        char        buf[4096];
        while (true) {
            const int n = SSL_read(ssl, buf, sizeof(buf));
            if (n <= 0) break;
            resp.append(buf, static_cast<std::size_t>(n));
        }
        parseReply(resp, reply);
    }
    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    ::close(fd);
    return reply;
}

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
                                   reinterpret_cast<const unsigned char*>("palmier-remote-test"),
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

// ---------------------------------------------------------------------------
// Project fixture and the wired remote stack
// ---------------------------------------------------------------------------

Project makeProject(Uuid& trackId, Uuid& assetId) {
    Project project;
    project.id = Uuid::generateV4();
    project.name = "Remote Access HTTP Integration Test";
    project.timelineFps = FrameRate::fps30();
    project.canvas = Resolution::hd1080();

    Track track;
    track.id = Uuid::generateV4();
    track.kind = TrackKind::Video;

    const MediaAssetRef asset(Uuid::generateV4(), "/media/a.mp4");
    project.assets.push_back(asset);
    project.tracks.push_back(track);

    trackId = track.id;
    assetId = asset.assetId;
    return project;
}

/// The whole stage-6 stack behind a real listener: the gate upstream of the
/// JSON-RPC protocol handler, over a real `ProjectSession` and the real default
/// tool surface. `port == 0`, so the listener takes an ephemeral port and parallel
/// CTest processes cannot collide.
class RemoteStack {
public:
    /// `tls` supplies the certificate/key pair to serve HTTPS; leave it empty for a
    /// plaintext non-loopback endpoint.
    RemoteStack(std::string bindAddress,
                std::optional<std::pair<std::filesystem::path, std::filesystem::path>> tls = {}) {
        config_.enabled = true;
        config_.bindAddress = std::move(bindAddress);
        config_.port = 0;   // ephemeral
        config_.bearerToken = kToken;
        config_.acknowledged = true;
        if (tls.has_value()) {
            config_.tlsCertificate = tls->first;
            config_.tlsPrivateKey = tls->second;
        }

        gate_ = std::make_unique<RemoteAccessGate>(config_, log_);

        session_ = std::make_unique<ProjectSession>();
        (void)session_->engine().reset(makeProject(trackId_, assetId_));
        registry_ = std::make_unique<ToolRegistry>(buildDefaultToolRegistry(*session_));
        executor_ = std::make_unique<McpToolExecutor>(*registry_, session_.get());
        protocol_ = std::make_unique<McpProtocolHandler>(*registry_, *executor_, sessions_,
                                                        inlineMainThreadInvoker());

        server_ = std::make_unique<McpServer>();
        server_->setProtocolDelegate(protocolDelegateFor(*protocol_));
        server_->setRemoteAccessGate(gate_.get());

        const BindDecision decision = gate_->validate();
        if (decision.tlsEnabled) {
            tlsMaterial_ = server_->setTlsMaterial(*config_.tlsCertificate,
                                                   *config_.tlsPrivateKey);
        }
        const Result<void> started = server_->start(decision);
        startError_ = started.isError() ? started.error().message() : std::string();
        started_ = started.isOk();
        if (started_) port_ = server_->boundPort();
    }

    ~RemoteStack() {
        if (server_) server_->stop();
    }

    [[nodiscard]] bool                 started() const { return started_; }
    [[nodiscard]] const std::string&   startError() const { return startError_; }
    [[nodiscard]] std::uint16_t        port() const { return port_; }
    [[nodiscard]] const std::string&   host() const { return config_.bindAddress; }
    [[nodiscard]] BindDecision         decision() const { return gate_->validate(); }
    [[nodiscard]] RecordingRejectionLog& log() { return log_; }
    [[nodiscard]] const Result<void>&  tlsMaterial() const { return tlsMaterial_; }

    /// The current project serialized — the byte-identical baseline the rejection
    /// cases are compared against.
    [[nodiscard]] std::string projectBytes() const {
        return serializeProject(session_->engine().snapshot());
    }
    [[nodiscard]] std::size_t undoDepth() const { return session_->engine().undoDepth(); }

private:
    RemoteAccessConfig                  config_;
    RecordingRejectionLog               log_;
    std::unique_ptr<RemoteAccessGate>   gate_;
    Uuid                                trackId_;
    Uuid                                assetId_;
    std::unique_ptr<ProjectSession>     session_;
    std::unique_ptr<ToolRegistry>       registry_;
    std::unique_ptr<McpToolExecutor>    executor_;
    McpSessionRegistry                  sessions_;
    std::unique_ptr<McpProtocolHandler> protocol_;
    std::unique_ptr<McpServer>          server_;
    Result<void>                        tlsMaterial_{};   ///< Default-constructed = ok.
    bool                                started_ = false;
    std::string                         startError_;
    std::uint16_t                       port_ = 0;
};

/// A JSON-RPC `initialize` request body — the first request any MCP client sends,
/// and therefore the one whose admission matters most.
std::string initializeBody(std::int64_t id) {
    Json params = Json::object();
    params.set("protocolVersion", Json("2025-06-18"));
    params.set("clientInfo", Json::object({{"name", Json("remote-access-integration-test")},
                                           {"version", Json("1.0")}}));
    Json request = Json::object();
    request.set("jsonrpc", Json("2.0"));
    request.set("id", Json(id));
    request.set("method", Json("initialize"));
    request.set("params", std::move(params));
    return request.dump();
}

std::vector<std::pair<std::string, std::string>> bearer(const std::string& token) {
    return {{"Authorization", "Bearer " + token}};
}

/// The per-process temp directory for TLS material. `getpid()` is part of the name
/// because `gtest_discover_tests` runs one process per case and CTest runs those
/// in parallel.
std::filesystem::path uniqueTlsDirectory() {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("palmier-remote-access-tls-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    return dir;
}

// ===========================================================================
// Requirement 10.4 over a real non-loopback socket, and Requirement 15.4's
// "byte-identical project" after every rejection.
// ===========================================================================

TEST(RemoteAccessHttpIntegration, MissingAndWrongBearerTokensAreRefusedAndTheProjectIsUntouched) {
    const std::optional<std::string> address = firstNonLoopbackIpv4();
    if (!address.has_value()) {
        GTEST_SKIP() << "skipped: this host has no non-loopback IPv4 address to bind, so a "
                        "non-loopback endpoint cannot be exercised (a second loopback address "
                        "such as 127.0.0.2 would be treated as loopback-only by the gate and "
                        "would admit every request unconditionally, per Requirement 10.10)";
    }

    RemoteStack stack(*address);
    ASSERT_TRUE(stack.started()) << stack.startError();
    ASSERT_FALSE(stack.decision().loopbackOnly);
    ASSERT_EQ(stack.decision().host, *address);
    ASSERT_GT(stack.port(), 0);
    // Requirement 10.7: a non-loopback plaintext bind still happens, and offers
    // exactly one warning.
    ASSERT_TRUE(stack.decision().plaintextWarning.has_value());

    const std::string baseline = stack.projectBytes();
    const std::size_t undoBaseline = stack.undoDepth();

    // --- 1. no bearer token at all -> 401 ----------------------------------
    const HttpReply anonymous = httpPost(*address, stack.port(), initializeBody(1));
    ASSERT_TRUE(anonymous.ok) << "no HTTP response was received";
    EXPECT_EQ(anonymous.status, 401);
    EXPECT_NE(anonymous.body.find("no_token"), std::string::npos) << anonymous.body;
    // A 401 invites the client to authenticate.
    EXPECT_NE(anonymous.header("WWW-Authenticate"), nullptr) << anonymous.raw;
    // The refused request never reached the Tool_Surface.
    EXPECT_EQ(stack.projectBytes(), baseline);
    EXPECT_EQ(stack.undoDepth(), undoBaseline);
    EXPECT_EQ(anonymous.header("Mcp-Session-Id"), nullptr);

    // --- 2. a wrong bearer token -> 401 ------------------------------------
    std::string wrong = kToken;
    wrong.back() = wrong.back() == 'f' ? 'e' : 'f';
    const HttpReply mismatched =
        httpPost(*address, stack.port(), initializeBody(2), bearer(wrong));
    ASSERT_TRUE(mismatched.ok);
    EXPECT_EQ(mismatched.status, 401);
    EXPECT_NE(mismatched.body.find("token_mismatch"), std::string::npos) << mismatched.body;
    EXPECT_EQ(stack.projectBytes(), baseline);
    EXPECT_EQ(stack.undoDepth(), undoBaseline);
    // Requirement 10.8: both refusals are recorded, keyed by the source address,
    // with a reason code and no credential.
    ASSERT_EQ(stack.log().size(), 2u);
    for (const RejectionRecord& record : stack.log().records()) {
        EXPECT_FALSE(record.sourceAddress.empty());
        EXPECT_EQ(record.timestampUtc.size(), 24u);
        EXPECT_EQ(record.timestampUtc.find(kToken), std::string::npos);
        EXPECT_EQ(record.reasonCode.find(kToken), std::string::npos);
    }

    // --- 3. the configured bearer token -> 200 -----------------------------
    const HttpReply admitted =
        httpPost(*address, stack.port(), initializeBody(3), bearer(kToken));
    ASSERT_TRUE(admitted.ok);
    EXPECT_EQ(admitted.status, 200);
    Result<Json> parsed = Json::parse(admitted.body);
    ASSERT_TRUE(parsed.isOk()) << admitted.body;
    const Json body = parsed.value();
    EXPECT_EQ(body.stringOr("jsonrpc"), "2.0");
    ASSERT_TRUE(body.contains("result")) << admitted.body;
    EXPECT_EQ(body.find("result")->stringOr("protocolVersion"), "2025-06-18");
    // Requirement 9.11: an admitted `initialize` mints a session identifier.
    const std::string* session = admitted.header("Mcp-Session-Id");
    ASSERT_NE(session, nullptr) << admitted.raw;
    EXPECT_TRUE(McpSessionRegistry::isWellFormedId(*session)) << *session;
    // Admission logged nothing further, and the two rejections stand alone.
    EXPECT_EQ(stack.log().size(), 2u);
}

// ===========================================================================
// Requirement 10.6: HTTPS is served on the configured address, and a plaintext
// request on that port is refused without ever being dispatched.
// ===========================================================================

TEST(RemoteAccessHttpIntegration, HttpsIsServedAndPlaintextOnTheTlsPortIsRejected) {
#if !defined(PALMIER_HAVE_OPENSSL)
    GTEST_SKIP() << "skipped: PALMIER_HAVE_OPENSSL is not defined in this build, so no TLS "
                    "transport exists to serve HTTPS; the gate treats configured TLS material "
                    "as an unmet prerequisite and binds loopback instead (Requirement 10.12)";
#else
    ASSERT_TRUE(tlsTransportAvailable());

    const std::optional<std::string> address = firstNonLoopbackIpv4();
    if (!address.has_value()) {
        GTEST_SKIP() << "skipped: this host has no non-loopback IPv4 address to bind, so a "
                        "TLS-served non-loopback endpoint cannot be exercised";
    }

    const std::filesystem::path dir = uniqueTlsDirectory();
    const std::filesystem::path cert = dir / "server.crt";
    const std::filesystem::path key = dir / "server.key";
    ASSERT_TRUE(writeSelfSignedPair(cert, key));

    {
        RemoteStack stack(*address, std::make_pair(cert, key));
        ASSERT_TRUE(stack.tlsMaterial().isOk()) << stack.tlsMaterial().error().message();
        ASSERT_TRUE(stack.started()) << stack.startError();
        ASSERT_TRUE(stack.decision().tlsEnabled);
        ASSERT_FALSE(stack.decision().loopbackOnly);
        // Requirement 10.7: with TLS in place there is no unencrypted-traffic
        // warning to emit.
        EXPECT_FALSE(stack.decision().plaintextWarning.has_value());

        const std::string baseline = stack.projectBytes();

        // --- HTTPS with the configured token succeeds ----------------------
        const HttpReply secure =
            httpsPost(*address, stack.port(), initializeBody(1), bearer(kToken));
        ASSERT_TRUE(secure.ok) << "the TLS handshake or the HTTPS exchange failed";
        EXPECT_EQ(secure.status, 200);
        Result<Json> parsed = Json::parse(secure.body);
        ASSERT_TRUE(parsed.isOk()) << secure.body;
        EXPECT_TRUE(parsed.value().contains("result")) << secure.body;
        EXPECT_NE(secure.header("Mcp-Session-Id"), nullptr) << secure.raw;
        EXPECT_EQ(stack.log().size(), 0u);

        // --- plaintext on the same port is rejected ------------------------
        // The handshake fails, the connection is closed, and no HTTP response is
        // produced at all — so the request cannot have been dispatched.
        const HttpReply plaintext =
            httpPost(*address, stack.port(), initializeBody(2), bearer(kToken));
        // The peer may see TLS alert bytes on the wire before the close, so the
        // claim is not "no bytes" but "no HTTP response": nothing was served, so
        // nothing was dispatched.
        EXPECT_EQ(plaintext.status, 0) << plaintext.raw;
        EXPECT_EQ(plaintext.raw.find("HTTP/"), std::string::npos) << plaintext.raw;
        EXPECT_TRUE(plaintext.body.empty()) << plaintext.body;
        EXPECT_EQ(stack.projectBytes(), baseline);

        // Requirement 10.8: the refusal is logged even though no HttpRequest ever
        // existed, under its own reason code.
        ASSERT_EQ(stack.log().size(), 1u);
        const std::vector<RejectionRecord> records = stack.log().records();
        const RejectionRecord&             record = records.front();
        EXPECT_EQ(record.reason, RejectionReason::PlaintextOnTlsPort);
        EXPECT_EQ(record.reasonCode, "plaintext_on_tls_port");
        EXPECT_FALSE(record.sourceAddress.empty());
        EXPECT_EQ(record.timestampUtc.size(), 24u);

        // A second HTTPS request still works: the failed handshake took down only
        // its own connection.
        const HttpReply again =
            httpsPost(*address, stack.port(), initializeBody(3), bearer(kToken));
        ASSERT_TRUE(again.ok);
        EXPECT_EQ(again.status, 200);
    }

    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
#endif
}

}  // namespace
}  // namespace palmier::services
