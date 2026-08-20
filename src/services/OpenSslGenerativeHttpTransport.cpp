// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/OpenSslGenerativeHttpTransport.cpp — implementation. See the header
// for the contract and for why this is its own translation unit.

#include "services/OpenSslGenerativeHttpTransport.hpp"

#if defined(PALMIER_HAVE_OPENSSL)

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

#include "core/Error.hpp"

#endif  // PALMIER_HAVE_OPENSSL

namespace palmier::services {

#if !defined(PALMIER_HAVE_OPENSSL)

// ---------------------------------------------------------------------------
// No OpenSSL in this build: the factory always returns nullptr, matching the
// header's documented contract. No socket or OpenSSL header is ever named.
// ---------------------------------------------------------------------------

bool openSslGenerativeHttpTransportAvailable() noexcept { return false; }

std::unique_ptr<GenerativeHttpTransport> makeOpenSslGenerativeHttpTransport(
    OpenSslTransportOptions) {
    return nullptr;
}

#else  // PALMIER_HAVE_OPENSSL

namespace {

// ---------------------------------------------------------------------------
// A parsed "https://host[:port]/path" URL. GenerativeHttpTransport.hpp's own
// isHttpsUrl()/urlFor() already guarantee every URL reaching here starts with
// "https://"; this parser still validates defensively (Requirement 11.4) rather
// than trusting that upstream check alone, since a caller could in principle
// construct a GenerativeHttpRequest directly.
// ---------------------------------------------------------------------------

struct ParsedUrl {
    std::string host;
    std::uint16_t port = 443;
    std::string pathAndQuery = "/";
};

[[nodiscard]] std::optional<ParsedUrl> parseHttpsUrl(std::string_view url) {
    constexpr std::string_view kScheme = "https://";
    if (url.size() <= kScheme.size() || url.substr(0, kScheme.size()) != kScheme) {
        return std::nullopt;  // Requirement 11.4: anything but https:// is refused.
    }
    std::string_view rest = url.substr(kScheme.size());
    const std::size_t pathStart = rest.find('/');
    std::string_view authority = pathStart == std::string_view::npos ? rest : rest.substr(0, pathStart);
    if (authority.empty()) {
        return std::nullopt;
    }

    ParsedUrl out;
    const std::size_t colon = authority.rfind(':');
    // A bracketed IPv6 literal's colons are not a port separator; this project's
    // generative endpoints are always hostnames or IPv4 literals in practice, so
    // a bracketed-IPv6 authority is rejected rather than mis-parsed.
    if (colon != std::string_view::npos && authority.find(']') == std::string_view::npos) {
        const std::string_view portText = authority.substr(colon + 1);
        if (portText.empty() || !std::all_of(portText.begin(), portText.end(),
                                             [](char c) { return c >= '0' && c <= '9'; })) {
            return std::nullopt;
        }
        int parsedPort = 0;
        for (char c : portText) {
            parsedPort = parsedPort * 10 + (c - '0');
            if (parsedPort > 65535) return std::nullopt;
        }
        if (parsedPort == 0) return std::nullopt;
        out.port = static_cast<std::uint16_t>(parsedPort);
        out.host = std::string(authority.substr(0, colon));
    } else {
        out.host = std::string(authority);
    }
    if (out.host.empty()) {
        return std::nullopt;
    }

    out.pathAndQuery = pathStart == std::string_view::npos ? std::string("/")
                                                            : std::string(rest.substr(pathStart));
    if (out.pathAndQuery.empty()) {
        out.pathAndQuery = "/";
    }
    return out;
}

/// Format one absolute deadline `std::chrono::milliseconds` from now as a
/// steady_clock time_point, so every blocking step below shares one clock.
[[nodiscard]] std::chrono::steady_clock::time_point deadlineFrom(
    std::chrono::milliseconds timeout) {
    return std::chrono::steady_clock::now() + timeout;
}

[[nodiscard]] std::chrono::milliseconds remaining(std::chrono::steady_clock::time_point deadline) {
    const auto left = deadline - std::chrono::steady_clock::now();
    return left.count() <= 0 ? std::chrono::milliseconds{0}
                             : std::chrono::duration_cast<std::chrono::milliseconds>(left);
}

/// RAII file descriptor. Nothing in this translation unit throws, so this is
/// purely for the early-return-heavy control flow below.
class ScopedFd {
public:
    explicit ScopedFd(int fd = -1) : fd_(fd) {}
    ~ScopedFd() { reset(); }
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
    ScopedFd(ScopedFd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    ScopedFd& operator=(ScopedFd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }
    void reset() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }
    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

private:
    int fd_;
};

/// RAII wrapper over the two OpenSSL client objects this connection needs.
class ScopedSsl {
public:
    ScopedSsl() = default;
    ~ScopedSsl() { reset(); }
    ScopedSsl(const ScopedSsl&) = delete;
    ScopedSsl& operator=(const ScopedSsl&) = delete;

    [[nodiscard]] bool createContext() {
        ctx_ = SSL_CTX_new(TLS_client_method());
        if (ctx_ == nullptr) return false;
        // Requirement 11.1: never accept below TLS 1.2.
        SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
        return true;
    }

    [[nodiscard]] SSL_CTX* ctx() const noexcept { return ctx_; }

    [[nodiscard]] bool createSsl() {
        if (ctx_ == nullptr) return false;
        ssl_ = SSL_new(ctx_);
        return ssl_ != nullptr;
    }

    [[nodiscard]] SSL* ssl() const noexcept { return ssl_; }

    void reset() {
        if (ssl_ != nullptr) {
            SSL_free(ssl_);
            ssl_ = nullptr;
        }
        if (ctx_ != nullptr) {
            SSL_CTX_free(ctx_);
            ctx_ = nullptr;
        }
    }

private:
    SSL_CTX* ctx_ = nullptr;
    SSL* ssl_ = nullptr;
};

/// Wait for `fd` to become readable/writable (per `forWrite`) or for `deadline`
/// to pass, whichever comes first. Returns false on timeout or a poll error.
[[nodiscard]] bool waitReady(int fd, bool forWrite, std::chrono::steady_clock::time_point deadline) {
    const std::chrono::milliseconds left = remaining(deadline);
    if (left.count() <= 0) return false;
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = forWrite ? POLLOUT : POLLIN;
    const int rc = ::poll(&pfd, 1, static_cast<int>(left.count()));
    if (rc <= 0) return false;
    return (pfd.revents & (forWrite ? POLLOUT : POLLIN)) != 0;
}

/// DNS-resolve `host`, connect a non-blocking socket to the first address that
/// completes within `deadline`, then restore blocking mode. `Io` on any
/// resolution/connect failure, `Timeout` if the deadline passes first
/// (Requirement 11.3's distinct-codes obligation).
[[nodiscard]] Result<ScopedFd> connectWithTimeout(const std::string& host, std::uint16_t port,
                                                  std::chrono::milliseconds timeout) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* resolved = nullptr;
    const std::string portText = std::to_string(port);
    const int gaiStatus = ::getaddrinfo(host.c_str(), portText.c_str(), &hints, &resolved);
    if (gaiStatus != 0 || resolved == nullptr) {
        return err<ScopedFd>(makeError(
            ErrorCode::Io, "could not resolve host '" + host + "': " +
                              (gaiStatus != 0 ? ::gai_strerror(gaiStatus) : "no address returned")));
    }

    const std::chrono::steady_clock::time_point deadline = deadlineFrom(timeout);
    Result<ScopedFd> outcome = err<ScopedFd>(
        makeError(ErrorCode::Io, "could not connect to '" + host + "': no address family succeeded"));

    for (addrinfo* candidate = resolved; candidate != nullptr; candidate = candidate->ai_next) {
        ScopedFd fd(::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol));
        if (!fd.valid()) {
            continue;
        }
        const int flags = ::fcntl(fd.get(), F_GETFL, 0);
        if (flags >= 0) {
            ::fcntl(fd.get(), F_SETFL, flags | O_NONBLOCK);
        }

        const int rc = ::connect(fd.get(), candidate->ai_addr, candidate->ai_addrlen);
        if (rc == 0) {
            outcome = Result<ScopedFd>(std::move(fd));
            break;
        }
        if (errno != EINPROGRESS) {
            continue;  // try the next address
        }
        if (!waitReady(fd.get(), /*forWrite=*/true, deadline)) {
            // Distinguish "ran out of time" from "connection actively refused":
            // a refused connection also becomes writable, so check SO_ERROR.
            if (remaining(deadline).count() <= 0) {
                outcome = err<ScopedFd>(
                    makeError(ErrorCode::Timeout, "connecting to '" + host + "' timed out"));
                break;
            }
            continue;
        }
        int soError = 0;
        socklen_t soErrorLen = sizeof(soError);
        if (::getsockopt(fd.get(), SOL_SOCKET, SO_ERROR, &soError, &soErrorLen) == 0 &&
            soError == 0) {
            // Left non-blocking deliberately: handshake() immediately needs the
            // socket non-blocking again for its own WANT_READ/WANT_WRITE poll
            // loop, so restoring blocking mode here would only be undone a few
            // lines later by the caller.
            outcome = Result<ScopedFd>(std::move(fd));
            break;
        }
        // This address refused or failed; try the next one.
    }

    ::freeaddrinfo(resolved);
    return outcome;
}

/// Read exactly the OpenSSL error queue's leading message, for a diagnostic that
/// names *why* a handshake failed without echoing any request/response bytes
/// (which could carry a credential).
[[nodiscard]] std::string opensslErrorSummary() {
    unsigned long code = ERR_get_error();
    if (code == 0) return "no further detail is available";
    char buffer[256];
    ERR_error_string_n(code, buffer, sizeof(buffer));
    ERR_clear_error();
    return std::string(buffer);
}

/// Perform the TLS handshake over `fd`, with hostname verification against
/// `host` when `verify` is set (Requirement 11.1). `PermissionDenied` on any
/// certificate or hostname failure, `Timeout` if the handshake itself does not
/// complete within `deadline`.
[[nodiscard]] Result<void> handshake(ScopedSsl& tls, int fd, const std::string& host, bool verify,
                                    std::chrono::steady_clock::time_point deadline) {
    if (!tls.createContext()) {
        return err<void>(makeError(ErrorCode::Internal, "could not create a TLS client context"));
    }
    if (verify) {
        SSL_CTX_set_verify(tls.ctx(), SSL_VERIFY_PEER, nullptr);
        if (SSL_CTX_set_default_verify_paths(tls.ctx()) != 1) {
            return err<void>(makeError(
                ErrorCode::Internal,
                "could not load the host's default certificate trust store"));
        }
    } else {
        SSL_CTX_set_verify(tls.ctx(), SSL_VERIFY_NONE, nullptr);
    }
    if (!tls.createSsl()) {
        return err<void>(makeError(ErrorCode::Internal, "could not create a TLS session"));
    }

    SSL_set_fd(tls.ssl(), fd);
    // SNI: send the server name, required by most TLS-terminating front ends.
    SSL_set_tlsext_host_name(tls.ssl(), host.c_str());
    if (verify) {
        // Verify the certificate's subject/SAN matches `host`, not merely that
        // the chain is valid for SOME name (Requirement 11.1's "server
        // certificate verified by default" means verified against the endpoint
        // actually being contacted).
        SSL_set1_host(tls.ssl(), host.c_str());
    }

    const int nonBlockFlags = ::fcntl(fd, F_GETFL, 0);
    if (nonBlockFlags >= 0) {
        ::fcntl(fd, F_SETFL, nonBlockFlags | O_NONBLOCK);
    }

    for (;;) {
        const int rc = SSL_connect(tls.ssl());
        if (rc == 1) {
            break;
        }
        const int sslError = SSL_get_error(tls.ssl(), rc);
        if (sslError == SSL_ERROR_WANT_READ) {
            if (!waitReady(fd, /*forWrite=*/false, deadline)) {
                return err<void>(makeError(ErrorCode::Timeout,
                                          "the TLS handshake with '" + host + "' timed out"));
            }
            continue;
        }
        if (sslError == SSL_ERROR_WANT_WRITE) {
            if (!waitReady(fd, /*forWrite=*/true, deadline)) {
                return err<void>(makeError(ErrorCode::Timeout,
                                          "the TLS handshake with '" + host + "' timed out"));
            }
            continue;
        }
        return err<void>(makeError(
            ErrorCode::PermissionDenied,
            "the TLS handshake with '" + host + "' failed: " + opensslErrorSummary()));
    }

    if (verify) {
        const long verifyResult = SSL_get_verify_result(tls.ssl());
        if (verifyResult != X509_V_OK) {
            return err<void>(makeError(
                ErrorCode::PermissionDenied,
                "the server certificate presented by '" + host +
                    "' could not be verified: " + X509_verify_cert_error_string(verifyResult)));
        }
    }

    // The socket is left NON-BLOCKING deliberately: writeAll()/readAll() below
    // both branch on SSL_ERROR_WANT_READ/WANT_WRITE, which OpenSSL only ever
    // returns for a non-blocking fd — restoring blocking mode here would make
    // SSL_write()/SSL_read() block indefinitely instead of returning control so
    // this function's own poll()-based deadline can fire, silently defeating
    // Requirement 11.3's I/O timeout for every request after a successful
    // handshake.
    return ok();
}

/// Write `data` in full over `ssl`, honouring WANT_READ/WANT_WRITE and
/// `deadline`. `Timeout` on a deadline miss, `Io` on any other TLS failure.
[[nodiscard]] Result<void> writeAll(SSL* ssl, int fd, std::string_view data,
                                    std::chrono::steady_clock::time_point deadline) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const int n = SSL_write(ssl, data.data() + sent, static_cast<int>(data.size() - sent));
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
            continue;
        }
        const int sslError = SSL_get_error(ssl, n);
        if (sslError == SSL_ERROR_WANT_READ) {
            if (!waitReady(fd, /*forWrite=*/false, deadline)) {
                return err<void>(makeError(ErrorCode::Timeout, "sending the request timed out"));
            }
            continue;
        }
        if (sslError == SSL_ERROR_WANT_WRITE) {
            if (!waitReady(fd, /*forWrite=*/true, deadline)) {
                return err<void>(makeError(ErrorCode::Timeout, "sending the request timed out"));
            }
            continue;
        }
        return err<void>(
            makeError(ErrorCode::Io, "sending the request failed: " + opensslErrorSummary()));
    }
    return ok();
}

/// Read until the peer closes the connection (this project's generative
/// requests are all `Connection: close`-shaped single exchanges, matching the
/// existing MCP HTTP client tests' own model), honouring `deadline`.
[[nodiscard]] Result<std::string> readAll(SSL* ssl, int fd,
                                          std::chrono::steady_clock::time_point deadline) {
    std::string out;
    char buffer[8192];
    for (;;) {
        const int n = SSL_read(ssl, buffer, sizeof(buffer));
        if (n > 0) {
            out.append(buffer, static_cast<std::size_t>(n));
            continue;
        }
        const int sslError = SSL_get_error(ssl, n);
        if (sslError == SSL_ERROR_ZERO_RETURN) {
            break;  // clean TLS close_notify
        }
        if (sslError == SSL_ERROR_WANT_READ) {
            if (!waitReady(fd, /*forWrite=*/false, deadline)) {
                return err<std::string>(
                    makeError(ErrorCode::Timeout, "reading the response timed out"));
            }
            continue;
        }
        if (sslError == SSL_ERROR_WANT_WRITE) {
            if (!waitReady(fd, /*forWrite=*/true, deadline)) {
                return err<std::string>(
                    makeError(ErrorCode::Timeout, "reading the response timed out"));
            }
            continue;
        }
        if (sslError == SSL_ERROR_SYSCALL && out.empty()) {
            // The peer closed the TCP connection without a TLS close_notify.
            // Some HTTP/1.1 servers do this after a `Connection: close`
            // response body has already been fully delivered, in which case
            // `out` is non-empty and this branch is not taken; here it means no
            // bytes were ever received.
            return err<std::string>(
                makeError(ErrorCode::Io, "the connection was closed before any response bytes "
                                        "were received"));
        }
        break;  // treat any other terminal condition as end-of-stream
    }
    return Result<std::string>(std::move(out));
}

/// Split a raw HTTP/1.1 response into status code + body, tolerating either a
/// `Content-Length`-correct body or a connection-close-terminated one (both are
/// already fully read into `raw` by readAll() above).
[[nodiscard]] Result<GenerativeHttpResponse> parseHttpResponse(const std::string& raw) {
    const std::size_t statusLineEnd = raw.find("\r\n");
    if (statusLineEnd == std::string::npos) {
        return err<GenerativeHttpResponse>(
            makeError(ErrorCode::Io, "the response did not contain a valid HTTP status line"));
    }
    const std::string statusLine = raw.substr(0, statusLineEnd);
    const std::size_t firstSpace = statusLine.find(' ');
    if (firstSpace == std::string::npos) {
        return err<GenerativeHttpResponse>(
            makeError(ErrorCode::Io, "the response's status line was malformed"));
    }
    const std::size_t secondSpace = statusLine.find(' ', firstSpace + 1);
    const std::string statusText = statusLine.substr(
        firstSpace + 1, secondSpace == std::string::npos ? std::string::npos
                                                          : secondSpace - firstSpace - 1);
    int status = 0;
    for (char c : statusText) {
        if (c < '0' || c > '9') {
            return err<GenerativeHttpResponse>(
                makeError(ErrorCode::Io, "the response's status code was not numeric"));
        }
        status = status * 10 + (c - '0');
    }

    const std::size_t headerEnd = raw.find("\r\n\r\n");
    GenerativeHttpResponse out;
    out.status = status;
    out.body = headerEnd == std::string::npos ? std::string{} : raw.substr(headerEnd + 4);

    // A chunked transfer-encoding body would need de-chunking; this project's
    // generative endpoints exchange small JSON bodies, and Content-Length or a
    // connection-close-terminated body (both handled by readAll() above) cover
    // that shape. A genuinely chunked response is passed through undecoded
    // rather than silently corrupted, which parseBody()'s downstream JSON
    // parse will then reject with a clear "malformed JSON body" error.
    return Result<GenerativeHttpResponse>(std::move(out));
}

/// The real transport.
class OpenSslGenerativeHttpTransport final : public GenerativeHttpTransport {
public:
    explicit OpenSslGenerativeHttpTransport(OpenSslTransportOptions options)
        : options_(options) {}

    [[nodiscard]] Result<GenerativeHttpResponse> send(
        const GenerativeHttpRequest& request) override {
        // Requirement 11.4: refused before a single byte is sent.
        const std::optional<ParsedUrl> parsed = parseHttpsUrl(request.url);
        if (!parsed.has_value()) {
            return err<GenerativeHttpResponse>(makeError(
                ErrorCode::InvalidArgument,
                "refusing to send a request to a non-https:// URL; no network connection was "
                "attempted"));
        }

        Result<ScopedFd> connected =
            connectWithTimeout(parsed->host, parsed->port, options_.connectTimeout);
        if (connected.isError()) {
            return err<GenerativeHttpResponse>(connected.error());
        }
        ScopedFd fd = std::move(connected).value();

        const std::chrono::steady_clock::time_point ioDeadline = deadlineFrom(options_.ioTimeout);

        ScopedSsl tls;
        Result<void> shaken =
            handshake(tls, fd.get(), parsed->host, options_.verifyServerCertificate, ioDeadline);
        if (shaken.isError()) {
            return err<GenerativeHttpResponse>(shaken.error());
        }

        const std::string requestText = buildRequestText(request, *parsed);
        Result<void> wrote = writeAll(tls.ssl(), fd.get(), requestText, ioDeadline);
        if (wrote.isError()) {
            return err<GenerativeHttpResponse>(wrote.error());
        }

        Result<std::string> raw = readAll(tls.ssl(), fd.get(), ioDeadline);
        if (raw.isError()) {
            return err<GenerativeHttpResponse>(raw.error());
        }

        SSL_shutdown(tls.ssl());
        return parseHttpResponse(raw.value());
    }

private:
    [[nodiscard]] static std::string buildRequestText(const GenerativeHttpRequest& request,
                                                       const ParsedUrl& url) {
        std::string out;
        out += request.method;
        out += ' ';
        out += url.pathAndQuery;
        out += " HTTP/1.1\r\n";
        out += "Host: " + url.host + "\r\n";
        out += "Connection: close\r\n";
        bool hasContentLength = false;
        for (const auto& [name, value] : request.headers) {
            out += name + ": " + value + "\r\n";
            if (name.size() == std::string_view("content-length").size()) {
                std::string lowered = name;
                std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                hasContentLength = hasContentLength || lowered == "content-length";
            }
        }
        if (!hasContentLength) {
            out += "Content-Length: " + std::to_string(request.body.size()) + "\r\n";
        }
        out += "\r\n";
        out += request.body;
        return out;
    }

    OpenSslTransportOptions options_;
};

}  // namespace

bool openSslGenerativeHttpTransportAvailable() noexcept { return true; }

std::unique_ptr<GenerativeHttpTransport> makeOpenSslGenerativeHttpTransport(
    OpenSslTransportOptions options) {
    return std::make_unique<OpenSslGenerativeHttpTransport>(options);
}

#endif  // PALMIER_HAVE_OPENSSL

}  // namespace palmier::services
