// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/McpServer.cpp — implementation of the MCP HTTP transport + lifecycle
// (task 15.2). See McpServer.hpp for the contract and its mapping to design.md
// "Component 2: MCP Server" and Requirements 7.1, 7.2, 7.3, 7.9.
//
// The socket layer is POSIX-only (Linux target) and dependency-light: a single
// blocking listening socket bound to a loopback address, one background accept
// thread, and a self-pipe used to wake the accept `poll()` for a prompt,
// in-budget stop(). Request routing is delegated to the pure `dispatch()` so it
// can be unit-tested without a live server.

#include "services/McpServer.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <utility>

#include "services/RemoteAccessGate.hpp"
#include "services/TlsTransport.hpp"

namespace palmier::services {
namespace {

// ---------------------------------------------------------------------------
// Byte stream abstraction (task 6.3)
// ---------------------------------------------------------------------------
//
// The request loop is identical for a plaintext and a TLS connection; only the
// two byte primitives differ. Expressing that as one tiny interface is what keeps
// "a plaintext request on a TLS port is refused" a property of *where the
// handshake happens* rather than a second copy of the HTTP reader: the handshake
// runs before any stream exists, so a failed handshake cannot produce an
// HttpRequest at all (Requirement 10.6).

class ByteStream {
public:
    virtual ~ByteStream() = default;

    /// Bytes read, 0 at end-of-stream, negative on error.
    [[nodiscard]] virtual long read(char* buffer, std::size_t length) = 0;
    [[nodiscard]] virtual bool writeAll(const char* buffer, std::size_t length) = 0;
};

// Read a full HTTP request from a connected socket: request line + headers up to
// the blank line, then `Content-Length` bytes of body. Returns false on a
// malformed request or a read error. Bounded so a misbehaving client cannot make
// the accept thread read unboundedly.
constexpr std::size_t kMaxHeaderBytes = 64 * 1024;
constexpr std::size_t kMaxBodyBytes   = 8 * 1024 * 1024;

bool sendAll(int fd, const char* buf, std::size_t len) {
    std::size_t sent = 0;
    while (sent < len) {
        const ssize_t n = ::send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

/// A plaintext connection: the original recv/send pair, unchanged in behaviour.
class PlainStream final : public ByteStream {
public:
    explicit PlainStream(int fd) : fd_(fd) {}

    long read(char* buffer, std::size_t length) override {
        while (true) {
            const ssize_t n = ::recv(fd_, buffer, length, 0);
            if (n < 0 && errno == EINTR) continue;
            return static_cast<long>(n);
        }
    }

    bool writeAll(const char* buffer, std::size_t length) override {
        return sendAll(fd_, buffer, length);
    }

private:
    int fd_;
};

/// A TLS connection: the same two primitives over an established session.
class TlsStream final : public ByteStream {
public:
    explicit TlsStream(TlsConnection& connection) : connection_(&connection) {}

    long read(char* buffer, std::size_t length) override {
        return connection_->read(buffer, length);
    }

    bool writeAll(const char* buffer, std::size_t length) override {
        return connection_->writeAll(buffer, length);
    }

private:
    TlsConnection* connection_;
};

bool recvAll(ByteStream& stream, char* buf, std::size_t want) {
    std::size_t got = 0;
    while (got < want) {
        const long n = stream.read(buf + got, want - got);
        if (n <= 0) return false;   // peer closed early, or a read error
        got += static_cast<std::size_t>(n);
    }
    return true;
}

std::string toLowerCopy(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string trim(std::string_view s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

// Parse a raw HTTP request buffer (headers already accumulated in `header`, body
// separately) into an HttpRequest. Returns false on a malformed request line.
bool parseRequestLine(std::string_view line, HttpRequest& out) {
    const std::size_t sp1 = line.find(' ');
    if (sp1 == std::string_view::npos) return false;
    const std::size_t sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos) return false;
    out.method = std::string(line.substr(0, sp1));
    out.target = std::string(line.substr(sp1 + 1, sp2 - sp1 - 1));
    for (char& c : out.method) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return !out.method.empty() && !out.target.empty();
}

// Read one complete request off `fd`. On success fills `req` (including its
// headers, with lower-case names); on failure returns false (caller closes the
// connection).
bool readHttpRequest(ByteStream& stream, HttpRequest& req) {
    std::string buf;
    buf.reserve(1024);
    char chunk[4096];

    // Accumulate until the end-of-headers marker "\r\n\r\n".
    std::size_t headerEnd = std::string::npos;
    while (true) {
        const long n = stream.read(chunk, sizeof(chunk));
        if (n <= 0) return false;
        buf.append(chunk, static_cast<std::size_t>(n));
        headerEnd = buf.find("\r\n\r\n");
        if (headerEnd != std::string::npos) break;
        if (buf.size() > kMaxHeaderBytes) return false;
    }

    const std::string headerBlock = buf.substr(0, headerEnd);
    std::string already = buf.substr(headerEnd + 4);  // bytes of body already read

    // Split the header block into lines.
    std::size_t lineStart = 0;
    bool first = true;
    std::size_t contentLength = 0;
    while (lineStart <= headerBlock.size()) {
        std::size_t nl = headerBlock.find("\r\n", lineStart);
        std::string_view line =
            std::string_view(headerBlock).substr(lineStart,
                (nl == std::string::npos ? headerBlock.size() : nl) - lineStart);
        if (first) {
            if (!parseRequestLine(line, req)) return false;
            first = false;
        } else if (!line.empty()) {
            const std::size_t colon = line.find(':');
            if (colon != std::string_view::npos) {
                const std::string name = toLowerCopy(trim(line.substr(0, colon)));
                const std::string value = trim(line.substr(colon + 1));
                if (name == "content-length") {
                    try {
                        contentLength = static_cast<std::size_t>(std::stoull(value));
                    } catch (...) {
                        return false;
                    }
                }
                req.headers.emplace_back(name, value);
            }
        }
        if (nl == std::string::npos) break;
        lineStart = nl + 2;
    }

    if (first) return false;  // no request line at all
    if (contentLength > kMaxBodyBytes) return false;

    // Read the remainder of the body if the declared length exceeds what we have.
    if (already.size() < contentLength) {
        const std::size_t remaining = contentLength - already.size();
        std::string rest(remaining, '\0');
        if (!recvAll(stream, rest.data(), remaining)) return false;
        already += rest;
    } else if (already.size() > contentLength) {
        already.resize(contentLength);
    }
    req.body = std::move(already);
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// HttpRequest / HttpResponse
// ---------------------------------------------------------------------------

std::string HttpRequest::path() const {
    const std::size_t q = target.find('?');
    return q == std::string::npos ? target : target.substr(0, q);
}

const std::string* HttpRequest::header(std::string_view name) const {
    const std::string wanted = toLowerCopy(name);
    for (const auto& [key, value] : headers) {
        if (key == wanted) return &value;
    }
    return nullptr;
}

std::string_view httpReasonPhrase(int status) noexcept {
    switch (status) {
        case 200: return "OK";
        case 202: return "Accepted";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Content Too Large";
        // The session-limit refusal of Requirement 10.9 (design.md D4's
        // "429-style limit").
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default:  break;
    }
    return status >= 400 ? "Error" : "OK";
}

std::string HttpResponse::toWire() const {
    std::string out;
    out.reserve(body.size() + 128);
    out += "HTTP/1.1 ";
    out += std::to_string(status);
    out += ' ';
    out += reason;
    out += "\r\n";
    if (!contentType.empty()) {
        out += "Content-Type: ";
        out += contentType;
        out += "\r\n";
    }
    for (const auto& [name, value] : headers) {
        out += name;
        out += ": ";
        out += value;
        out += "\r\n";
    }
    out += "Content-Length: ";
    out += std::to_string(body.size());
    out += "\r\n";
    out += "Connection: close\r\n";
    out += "\r\n";
    out += body;
    return out;
}

// ---------------------------------------------------------------------------
// Handler adapter
// ---------------------------------------------------------------------------

McpRequestHandler handlerFor(IMcpRequestHandler& handler) {
    return [&handler](const Json& request) { return handler.handleRequest(request); };
}

// ---------------------------------------------------------------------------
// McpServer
// ---------------------------------------------------------------------------

McpServer::McpServer(McpRequestHandler handler) : handler_(std::move(handler)) {}

McpServer::~McpServer() { stop(); }

void McpServer::setHandler(McpRequestHandler handler) { handler_ = std::move(handler); }

void McpServer::setProtocolDelegate(McpProtocolDelegate delegate) {
    protocol_ = std::move(delegate);
}

Result<void> McpServer::setTlsMaterial(const std::filesystem::path& certificate,
                                      const std::filesystem::path& privateKey) {
    if (running_.load()) {
        return err(failedPrecondition(
            "TLS material cannot be installed while the MCP server is running"));
    }
    Result<std::unique_ptr<TlsContext>> context = TlsContext::create(certificate, privateKey);
    if (context.isError()) {
        // The gate reports the same classification at bind time, so a listener can
        // never end up serving material the gate rejected (Requirement 10.12).
        return err(std::move(context).error());
    }
    tls_ = std::move(context).value();
    return ok();
}

std::string McpServer::boundHost() const { return running_.load() ? boundHost_ : std::string{}; }

McpRequestContext McpServer::contextFor(const HttpRequest& request, std::string sourceAddress,
                                        bool secureTransport) {
    McpRequestContext context;
    context.sourceAddress = std::move(sourceAddress);
    context.bodyBytes = request.body.size();
    context.secureTransport = secureTransport;

    if (const std::string* session = request.header(kSessionHeader); session != nullptr) {
        context.sessionId = *session;
    }
    if (const std::string* authorization = request.header("authorization");
        authorization != nullptr) {
        context.authorization = *authorization;
    }
    if (const std::string* origin = request.header("origin"); origin != nullptr) {
        context.origin = *origin;
    }
    return context;
}

HttpResponse McpServer::dispatchWithContext(const HttpRequest& request,
                                            const McpRequestContext& context) const {
    if (request.path() != kPath) {
        HttpResponse r;
        r.status = 404;
        r.reason = std::string(httpReasonPhrase(404));
        r.body = Json::object({{"error", Json("not found")}}).dump();
        return r;
    }
    if (request.method != "POST") {
        HttpResponse r;
        r.status = 405;
        r.reason = std::string(httpReasonPhrase(405));
        r.body = Json::object({{"error", Json("method not allowed")}}).dump();
        return r;
    }

    // Requirements 9.1/9.6: a body above the 1 MiB cap is a parse error, and it is
    // refused here so no protocol or execution work is ever started for it.
    if (request.body.size() > kMaxRequestBodyBytes) {
        Json err = Json::object();
        err.set("jsonrpc", Json("2.0"));
        err.set("id", Json(nullptr));
        err.set("error",
                Json::object({{"code", Json(std::int64_t{-32700})},
                              {"message", Json("Parse error: request body of " +
                                               std::to_string(request.body.size()) +
                                               " bytes exceeds the " +
                                               std::to_string(kMaxRequestBodyBytes) +
                                               "-byte limit")}}));
        HttpResponse r;
        r.status = 400;
        r.reason = std::string(httpReasonPhrase(400));
        r.body = err.dump();
        return r;
    }

    // Admission control, strictly upstream of the protocol layer (task 6.3,
    // design.md D4): the gate answers from transport facts alone, so a refused
    // request is never parsed as JSON-RPC, never validated against a ToolSchema and
    // never handed to the executor — it provably cannot reach the Tool_Surface
    // (Requirements 10.4, 10.5, 10.6). On a loopback-only binding the gate admits
    // unconditionally, which is why wiring it changes nothing for the default
    // developer experience (Requirement 10.10).
    if (gate_ != nullptr) {
        const Admission admission = gate_->admit(context);
        if (!admission.allowed) {
            HttpResponse r;
            r.status = admission.httpStatus;
            r.reason = std::string(httpReasonPhrase(admission.httpStatus));
            // The body names the reason CODE only: no credential, and nothing a
            // prober could use to distinguish "wrong token" from "no token".
            r.body = Json::object({{"error",
                                    Json(std::string(rejectionReasonMessage(admission.reason)))},
                                   {"reason",
                                    Json(std::string(rejectionReasonCode(admission.reason)))}})
                         .dump();
            if (admission.httpStatus == 401) {
                r.headers.emplace_back("WWW-Authenticate", "Bearer");
            }
            return r;
        }
    }

    // No JSON-RPC layer wired: keep answering through the original bespoke path so
    // pre-task-5.3 compositions and their tests behave exactly as before.
    if (!protocol_) return dispatch(request);

    const McpReply reply = protocol_(context, request.body);

    HttpResponse r;
    r.status = reply.httpStatus;
    r.reason = std::string(httpReasonPhrase(reply.httpStatus));
    r.body = reply.body;
    // A zero-byte body carries no media type: Requirement 9.10's 202 answer is
    // exactly `Content-Length: 0` with no content.
    if (r.body.empty()) r.contentType.clear();
    if (reply.newSessionId.has_value()) {
        r.headers.emplace_back(std::string(kSessionHeader), *reply.newSessionId);
        // A minted identifier is a newly established session, which is what the
        // gate's concurrent-session ceiling counts (Requirement 10.9).
        if (gate_ != nullptr) gate_->noteSessionCreated();
    }
    return r;
}

bool McpServer::isLoopbackHost(std::string_view host) {
    if (host == "localhost" || host == "ip6-localhost") return true;

    // IPv6 loopback (accept a bracketed form too).
    if (host == "::1" || host == "[::1]") return true;

    // IPv4: parse and check 127.0.0.0/8.
    in_addr v4{};
    std::string h(host);
    if (::inet_pton(AF_INET, h.c_str(), &v4) == 1) {
        const std::uint32_t addr = ntohl(v4.s_addr);
        return (addr >> 24) == 127;  // 127.0.0.0/8
    }

    // IPv6 loopback via inet_pton (covers e.g. "0:0:0:0:0:0:0:1").
    in6_addr v6{};
    if (::inet_pton(AF_INET6, h.c_str(), &v6) == 1) {
        static const unsigned char kLoop[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
        return std::memcmp(v6.s6_addr, kLoop, 16) == 0;
    }
    return false;
}

HttpResponse McpServer::dispatch(const HttpRequest& request) const {
    // Route: only the single MCP path is served.
    if (request.path() != kPath) {
        HttpResponse r;
        r.status = 404;
        r.reason = "Not Found";
        r.body = Json::object({{"error", Json("not found")}}).dump();
        return r;
    }

    // Only POST carries MCP/JSON-RPC messages on this endpoint.
    if (request.method != "POST") {
        HttpResponse r;
        r.status = 405;
        r.reason = "Method Not Allowed";
        r.body = Json::object({{"error", Json("method not allowed")}}).dump();
        return r;
    }

    // Parse the JSON body. A parse failure is a JSON-RPC "parse error" (-32700).
    Result<Json> parsed = Json::parse(request.body);
    if (parsed.isError()) {
        Json err = Json::object();
        err.set("jsonrpc", Json("2.0"));
        err.set("id", Json(nullptr));
        err.set("error", Json::object({{"code", Json(std::int64_t{-32700})},
                                       {"message", Json("Parse error")}}));
        HttpResponse r;
        r.status = 400;
        r.reason = "Bad Request";
        r.body = err.dump();
        return r;
    }

    // No executor wired yet: the endpoint is up but cannot serve tools. This is a
    // transport-level condition; the tool executor (task 15.3) supplies handler_.
    if (!handler_) {
        Json err = Json::object();
        err.set("jsonrpc", Json("2.0"));
        err.set("id", Json(nullptr));
        err.set("error", Json::object({{"code", Json(std::int64_t{-32603})},
                                       {"message", Json("MCP request handler unavailable")}}));
        HttpResponse r;
        r.status = 503;
        r.reason = "Service Unavailable";
        r.body = err.dump();
        return r;
    }

    // Delegate to the executor; its JSON is the response body.
    Json response = handler_(parsed.value());
    HttpResponse r;
    r.status = 200;
    r.reason = "OK";
    r.body = response.dump();
    return r;
}

Result<void> McpServer::start(std::string_view host, std::uint16_t port) {
    // The two-argument form is the loopback-only decision of Requirement 10.1.
    BindDecision decision;
    decision.host = std::string(host);
    decision.port = port;
    decision.loopbackOnly = true;
    decision.tlsEnabled = false;
    return start(decision);
}

Result<void> McpServer::start(const BindDecision& decision) {
    const std::string_view host = decision.host;
    const std::uint16_t    port = decision.port;

    if (running_.load()) {
        return err(failedPrecondition("MCP server is already running"));
    }
    if (decision.tlsEnabled && tls_ == nullptr) {
        // Refused rather than silently downgraded: a caller that asked for TLS must
        // never be served plaintext. Install the material with setTlsMaterial()
        // first; on a build without services::TlsTransport that call is what reports
        // the missing capability.
        return err(unsupported(
            "MCP server cannot serve TLS on '" + std::string(host) +
            "': no TLS material is installed, so this bind was refused rather than "
            "served as plaintext"));
    }
    if (decision.loopbackOnly && !isLoopbackHost(host)) {
        return err(invalidArgument(
            "MCP server refuses to bind a non-loopback host '" + std::string(host) +
            "'; the endpoint is loopback-only"));
    }

    // Resolve the host. Loopback textual aliases map onto 127.0.0.1; anything else
    // must be an IPv4 or IPv6 literal, the two forms Requirement 10.2 permits for a
    // remote bind address.
    std::string h(host);
    if (h == "localhost" || h == "ip6-localhost") h = "127.0.0.1";
    if (h.size() >= 2 && h.front() == '[' && h.back() == ']') h = h.substr(1, h.size() - 2);

    sockaddr_in  addr4{};
    sockaddr_in6 addr6{};
    int          family = AF_INET;
    if (::inet_pton(AF_INET, h.c_str(), &addr4.sin_addr) == 1) {
        addr4.sin_family = AF_INET;
        addr4.sin_port = htons(port);
    } else if (::inet_pton(AF_INET6, h.c_str(), &addr6.sin6_addr) == 1) {
        family = AF_INET6;
        addr6.sin6_family = AF_INET6;
        addr6.sin6_port = htons(port);
    } else {
        return err(invalidArgument(
            "MCP server cannot bind '" + std::string(host) +
            "': the listener accepts an IPv4 or IPv6 literal, or a loopback alias"));
    }

    const int fd = ::socket(family, SOCK_STREAM, 0);
    if (fd < 0) {
        return err(makeError(ErrorCode::Internal,
                             std::string("MCP server socket() failed: ") + std::strerror(errno)));
    }

    // SO_REUSEADDR lets us re-bind promptly across restarts (TIME_WAIT) without
    // masking an actively-listening conflict: a second bind to the same
    // address:port that another socket is already LISTENing on still fails with
    // EADDRINUSE, which is exactly the port-in-use condition of Requirement 7.3.
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    const sockaddr* bindAddr = family == AF_INET ? reinterpret_cast<sockaddr*>(&addr4)
                                                 : reinterpret_cast<sockaddr*>(&addr6);
    const socklen_t bindLen = family == AF_INET ? static_cast<socklen_t>(sizeof(addr4))
                                                : static_cast<socklen_t>(sizeof(addr6));
    if (::bind(fd, bindAddr, bindLen) != 0) {
        const int e = errno;
        ::close(fd);
        if (e == EADDRINUSE || e == EADDRNOTAVAIL) {
            return err(makeError(ErrorCode::FailedPrecondition,
                "MCP endpoint port " + std::to_string(port) +
                " on " + std::string(host) + " is unavailable (address already in use); "
                "the MCP server did not start and the current project is unchanged"));
        }
        return err(makeError(ErrorCode::Io,
            std::string("MCP server bind() failed: ") + std::strerror(e)));
    }

    if (::listen(fd, 16) != 0) {
        const int e = errno;
        ::close(fd);
        return err(makeError(ErrorCode::Io,
            std::string("MCP server listen() failed: ") + std::strerror(e)));
    }

    // Record the actually-bound port (resolves an ephemeral port == 0).
    sockaddr_storage bound{};
    socklen_t boundLen = sizeof(bound);
    std::uint16_t actualPort = port;
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &boundLen) == 0) {
        actualPort = bound.ss_family == AF_INET6
                         ? ntohs(reinterpret_cast<sockaddr_in6*>(&bound)->sin6_port)
                         : ntohs(reinterpret_cast<sockaddr_in*>(&bound)->sin_port);
    }

    // Self-pipe used to interrupt the accept poll() on stop().
    int pipefds[2] = {-1, -1};
    if (::pipe(pipefds) != 0) {
        const int e = errno;
        ::close(fd);
        return err(makeError(ErrorCode::Internal,
            std::string("MCP server pipe() failed: ") + std::strerror(e)));
    }

    listenFd_ = fd;
    wakeReadFd_ = pipefds[0];
    wakeWriteFd_ = pipefds[1];
    boundHost_ = h;
    secureTransport_ = decision.tlsEnabled;
    boundPort_.store(actualPort);
    stopRequested_.store(false);
    running_.store(true);

    acceptThread_ = std::thread([this] { acceptLoop(); });
    return ok();
}

void McpServer::acceptLoop() {
    while (!stopRequested_.load()) {
        pollfd fds[2];
        fds[0].fd = listenFd_;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        fds[1].fd = wakeReadFd_;
        fds[1].events = POLLIN;
        fds[1].revents = 0;

        const int pr = ::poll(fds, 2, -1);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (fds[1].revents & POLLIN) {
            break;  // stop() signalled via the self-pipe.
        }
        if (!(fds[0].revents & POLLIN)) continue;

        sockaddr_storage peer{};
        socklen_t        peerLen = sizeof(peer);
        const int conn = ::accept(listenFd_, reinterpret_cast<sockaddr*>(&peer), &peerLen);
        if (conn < 0) {
            if (errno == EINTR) continue;
            if (errno == EMFILE || errno == ENFILE) continue;  // transient
            break;
        }

        // The peer address is the source address every admission decision and every
        // rejection record is keyed by (Requirements 10.4, 10.8, 10.13).
        char        peerText[INET6_ADDRSTRLEN] = {0};
        std::string sourceAddress;
        const void* peerAddr =
            peer.ss_family == AF_INET6
                ? static_cast<const void*>(&reinterpret_cast<sockaddr_in6*>(&peer)->sin6_addr)
                : static_cast<const void*>(&reinterpret_cast<sockaddr_in*>(&peer)->sin_addr);
        if (::inet_ntop(peer.ss_family, peerAddr, peerText, sizeof(peerText)) != nullptr) {
            sourceAddress = peerText;
        }

        // On a TLS listener the handshake runs FIRST. A plaintext HTTP request fails
        // it, so the connection is closed and the refusal logged without an
        // HttpRequest ever existing — there is no code path from here to the
        // Tool_Surface for such a request (Requirement 10.6).
        std::unique_ptr<TlsConnection> tlsConnection;
        if (secureTransport_) {
            Result<std::unique_ptr<TlsConnection>> accepted = tls_->accept(conn);
            if (accepted.isError()) {
                if (gate_ != nullptr) gate_->noteHandshakeFailure(sourceAddress);
                ::close(conn);
                continue;
            }
            tlsConnection = std::move(accepted).value();
        }

        PlainStream              plain(conn);
        std::optional<TlsStream> secure;
        if (tlsConnection) secure.emplace(*tlsConnection);
        ByteStream& stream = secure.has_value() ? static_cast<ByteStream&>(*secure)
                                                : static_cast<ByteStream&>(plain);

        HttpRequest req;
        if (readHttpRequest(stream, req)) {
            const HttpResponse resp =
                dispatchWithContext(req, contextFor(req, sourceAddress, secureTransport_));
            const std::string wire = resp.toWire();
            (void)stream.writeAll(wire.data(), wire.size());
        } else {
            static constexpr char kBadRequest[] =
                "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            (void)stream.writeAll(kBadRequest, sizeof(kBadRequest) - 1);
        }
        if (tlsConnection) tlsConnection->shutdown();
        ::close(conn);
    }
}

void McpServer::closeListenSocket() {
    if (listenFd_ >= 0) {
        ::close(listenFd_);
        listenFd_ = -1;
    }
}

void McpServer::stop() {
    if (!running_.load()) return;
    stopRequested_.store(true);

    // Wake the accept poll().
    if (wakeWriteFd_ >= 0) {
        const char b = 1;
        ssize_t r = ::write(wakeWriteFd_, &b, 1);
        (void)r;
    }
    if (acceptThread_.joinable()) {
        acceptThread_.join();
    }

    closeListenSocket();
    if (wakeReadFd_ >= 0) { ::close(wakeReadFd_); wakeReadFd_ = -1; }
    if (wakeWriteFd_ >= 0) { ::close(wakeWriteFd_); wakeWriteFd_ = -1; }

    boundPort_.store(0);
    boundHost_.clear();
    secureTransport_ = false;
    running_.store(false);
    stopRequested_.store(false);
}

}  // namespace palmier::services
