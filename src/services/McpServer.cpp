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
#include <cstring>
#include <string>
#include <utility>

namespace palmier::services {
namespace {

// Read a full HTTP request from a connected socket: request line + headers up to
// the blank line, then `Content-Length` bytes of body. Returns false on a
// malformed request or a read error. Bounded so a misbehaving client cannot make
// the accept thread read unboundedly.
constexpr std::size_t kMaxHeaderBytes = 64 * 1024;
constexpr std::size_t kMaxBodyBytes   = 8 * 1024 * 1024;

bool recvAll(int fd, char* buf, std::size_t want) {
    std::size_t got = 0;
    while (got < want) {
        const ssize_t n = ::recv(fd, buf + got, want - got, 0);
        if (n == 0) return false;            // peer closed early
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        got += static_cast<std::size_t>(n);
    }
    return true;
}

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

// Read one complete request off `fd`. On success fills `req`; on failure returns
// false (caller closes the connection).
bool readHttpRequest(int fd, HttpRequest& req) {
    std::string buf;
    buf.reserve(1024);
    char chunk[4096];

    // Accumulate until the end-of-headers marker "\r\n\r\n".
    std::size_t headerEnd = std::string::npos;
    while (true) {
        const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
        if (n == 0) return false;
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
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
        if (!recvAll(fd, rest.data(), remaining)) return false;
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

std::string HttpResponse::toWire() const {
    std::string out;
    out.reserve(body.size() + 128);
    out += "HTTP/1.1 ";
    out += std::to_string(status);
    out += ' ';
    out += reason;
    out += "\r\n";
    out += "Content-Type: ";
    out += contentType;
    out += "\r\n";
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
    if (running_.load()) {
        return err(failedPrecondition("MCP server is already running"));
    }
    if (!isLoopbackHost(host)) {
        return err(invalidArgument(
            "MCP server refuses to bind a non-loopback host '" + std::string(host) +
            "'; the endpoint is loopback-only"));
    }

    // Resolve the loopback host into a sockaddr_in. isLoopbackHost has already
    // admitted only loopback forms; map the textual aliases onto 127.0.0.1.
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    std::string h(host);
    if (h == "localhost") h = "127.0.0.1";
    if (::inet_pton(AF_INET, h.c_str(), &addr.sin_addr) != 1) {
        // IPv6 loopback aliases fall back to IPv4 127.0.0.1 for this listener.
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    }

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
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

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
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
    sockaddr_in bound{};
    socklen_t boundLen = sizeof(bound);
    std::uint16_t actualPort = port;
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &boundLen) == 0) {
        actualPort = ntohs(bound.sin_port);
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

        const int conn = ::accept(listenFd_, nullptr, nullptr);
        if (conn < 0) {
            if (errno == EINTR) continue;
            if (errno == EMFILE || errno == ENFILE) continue;  // transient
            break;
        }

        HttpRequest req;
        if (readHttpRequest(conn, req)) {
            const HttpResponse resp = dispatch(req);
            const std::string wire = resp.toWire();
            sendAll(conn, wire.data(), wire.size());
        } else {
            static constexpr char kBadRequest[] =
                "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            sendAll(conn, kBadRequest, sizeof(kBadRequest) - 1);
        }
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
    running_.store(false);
    stopRequested_.store(false);
}

}  // namespace palmier::services
