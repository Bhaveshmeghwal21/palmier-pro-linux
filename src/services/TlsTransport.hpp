// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/TlsTransport.hpp — the optional TLS layer for the MCP endpoint
// (task 6.3; Requirements 10.6, 10.12; design.md decision D4, "TLS").
//
// TLS is served by OpenSSL 3.x (Apache-2.0, compatible with GPLv3) behind the
// option `PALMIER_ENABLE_OPENSSL` (default ON) with *optional* detection setting
// the guard `PALMIER_HAVE_OPENSSL`, mirroring the project's existing
// `PALMIER_HAVE_*` style. That optionality is the point, and it is a rule of the
// migration plan rather than a convenience: no stage may make configuration fail
// on a host that configured successfully at the previous stage. So when the guard
// is absent every entry point here still exists and still compiles — it simply
// reports the capability as unavailable, which makes "TLS is configured" an unmet
// prerequisite in `RemoteAccessGate::validate()` and falls the endpoint back to a
// loopback bind (Requirement 10.12's spirit applied to a build without OpenSSL).
//
// Two things live here, and nothing else:
//
//   * `checkTlsMaterial()` — the bind-time question the gate asks: do the
//     configured certificate and private key both load, and do they form a
//     matching pair? The three failure conditions Requirement 10.12 enumerates
//     are distinguished by error code AND named in the message: `Io` for
//     "cannot be read", `InvalidArgument` for "cannot be parsed",
//     `FailedPrecondition` for "do not form a matching pair".
//   * `TlsContext` / `TlsConnection` — the server-side handshake and the
//     encrypted byte stream the transport reads and writes. A plaintext request
//     arriving on a TLS listener fails the handshake here, so the connection is
//     closed and logged without ever producing an `HttpRequest`, which is exactly
//     what Requirement 10.6 demands ("reject any plaintext HTTP request on that
//     port without dispatching it to the Tool_Surface").
//
// The header is deliberately free of any OpenSSL type: handles are opaque, so
// including this costs a translation unit nothing and the transport does not
// acquire an OpenSSL include path.

#ifndef PALMIER_SERVICES_TLSTRANSPORT_HPP
#define PALMIER_SERVICES_TLSTRANSPORT_HPP

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>

#include "core/Result.hpp"

namespace palmier::services {

/// True iff this build has the OpenSSL-backed TLS transport compiled in, i.e.
/// `PALMIER_ENABLE_OPENSSL` was ON *and* OpenSSL 3.x was found at configure time.
/// When false, `checkTlsMaterial()` and `TlsContext::create()` return an
/// `Unsupported` error and the gate treats configured TLS material as an unmet
/// prerequisite.
[[nodiscard]] bool tlsTransportAvailable() noexcept;

/// Load `certificate` and `privateKey` and verify they form a matching pair.
///
/// Returns, distinguishing exactly the three conditions Requirement 10.12 names:
///   * `Io`                 — a file cannot be read (absent, a directory,
///                            unreadable permissions);
///   * `InvalidArgument`    — a file can be read but cannot be parsed as PEM;
///   * `FailedPrecondition` — both parse but do not form a matching pair;
///   * `Unsupported`        — this build has no TLS transport.
/// The message names the path and the condition; it never contains key material.
[[nodiscard]] Result<void> checkTlsMaterial(const std::filesystem::path& certificate,
                                            const std::filesystem::path& privateKey);

class TlsConnection;

/// A server-side TLS configuration: one loaded certificate/key pair, shared by
/// every accepted connection.
class TlsContext {
public:
    /// Load the material and build the server context. Fails with the same error
    /// classification as `checkTlsMaterial()`.
    [[nodiscard]] static Result<std::unique_ptr<TlsContext>> create(
        const std::filesystem::path& certificate, const std::filesystem::path& privateKey);

    ~TlsContext();

    TlsContext(const TlsContext&) = delete;
    TlsContext& operator=(const TlsContext&) = delete;

    /// Perform the server side of a TLS handshake on the already-connected socket
    /// `fd`. On success the returned connection owns the TLS session (but not the
    /// socket, which the caller still closes). On failure — which is what a
    /// plaintext HTTP request on this port produces — returns an error and leaves
    /// `fd` untouched apart from whatever bytes the handshake consumed, so the
    /// caller closes and logs without ever parsing a request (Requirement 10.6).
    [[nodiscard]] Result<std::unique_ptr<TlsConnection>> accept(int fd);

private:
    explicit TlsContext(void* nativeContext) : native_(nativeContext) {}

    void* native_ = nullptr;   ///< `SSL_CTX*`, opaque here by design.
};

/// One established TLS session. Reading and writing mirror `recv`/`send` so the
/// transport's request loop is the same shape for plaintext and TLS.
class TlsConnection {
public:
    ~TlsConnection();

    TlsConnection(const TlsConnection&) = delete;
    TlsConnection& operator=(const TlsConnection&) = delete;

    /// Read up to `length` decrypted bytes. Returns the byte count, 0 at
    /// end-of-stream, and a negative value on error.
    [[nodiscard]] long read(char* buffer, std::size_t length);

    /// Write every byte of `buffer`. False on any write failure.
    [[nodiscard]] bool writeAll(const char* buffer, std::size_t length);

    /// Send `close_notify` (best effort).
    void shutdown();

private:
    friend class TlsContext;
    explicit TlsConnection(void* nativeSession) : native_(nativeSession) {}

    void* native_ = nullptr;   ///< `SSL*`, opaque here by design.
};

}  // namespace palmier::services

#endif  // PALMIER_SERVICES_TLSTRANSPORT_HPP
