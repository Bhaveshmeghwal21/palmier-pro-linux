// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/OpenSslGenerativeHttpTransport.hpp — a real, OpenSSL-backed HTTPS
// implementation of the GenerativeHttpTransport seam (usable-editor spec Phase
// 2, Requirement 11).
//
// Why this is a separate file from GenerativeHttpTransport.cpp
// --------------------------------------------------------------------------
// Six test executables compile GenerativeHttpTransport.cpp as a standalone
// source (see tests/CMakeLists.txt) to exercise HttpGenerativeJobProtocol's
// request-building/error-mapping logic with no TLS dependency at all; four of
// those six link no OpenSSL. Putting real OpenSSL calls directly into that file
// would force every one of those targets to either link OpenSSL or fail to
// build. This translation unit is added ONLY to the palmier_services library
// target (src/services/CMakeLists.txt), which already links OpenSSL whenever
// PALMIER_ENABLE_OPENSSL AND OPENSSL_FOUND — so none of the six test targets
// need to change, and the six existing ScriptedTransport/ForbiddenTransport/
// RecordingTransport test doubles that already implement GenerativeHttpTransport
// continue to exercise the SAME interface this class also implements.
//
// What this class does, matching Requirement 11's five hard obligations:
//
//   11.1 A real HTTPS request, server certificate verified by default. This is
//        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr) plus
//        SSL_CTX_set_default_verify_paths() (the host's trust store) plus
//        SSL_set1_host()/SSL_set_verify (hostname match against the URL's
//        authority, not just chain validity — a chain can be valid for the
//        wrong name). This is the one place this codebase's existing TLS code
//        (TlsTransport.hpp, RemoteAccessGate) does NOT already have a pattern
//        to copy: every existing OpenSSL use in src/ is SERVER-side
//        (SSL_accept), and the only existing CLIENT-side OpenSSL code is a test
//        helper (remote_access_http_integration_test.cpp's httpsPost) that
//        deliberately calls SSL_VERIFY_NONE, which Requirement 11.1 forbids in
//        product code.
//   11.2 Headers/authorization carried unchanged; no credential logged. This
//        class never inspects header VALUES for logging — every diagnostic
//        message below names the URL/host/status, never a header.
//   11.3 A request timeout, and distinct error codes for timeout / connect
//        failure / TLS verification failure, mapped onto codes the existing
//        HttpGenerativeJobProtocol backends already branch on
//        (see GenerativeHttpTransport.cpp's mapGenerativeHttpStatus for the
//        HTTP-status half of that vocabulary; this class supplies the
//        transport-level half): Io for DNS/connect failure, Timeout for a send/
//        receive/connect deadline, PermissionDenied for a certificate or
//        hostname verification failure.
//   11.4 A plaintext http:// endpoint is refused without sending a byte.
//        HttpGenerativeJobProtocol::urlFor() already enforces this one level up
//        (GenerativeHttpTransport.cpp's isHttpsUrl()/urlFor()), so this class
//        reasserts it defensively rather than relying on that alone — a caller
//        that somehow constructed a GenerativeHttpRequest with a non-https URL
//        directly (bypassing the protocol layer) must still be refused here.
//   11.5 Installed by default; falls back to the unavailable transport only
//        where the build excludes TLS. See ApplicationComposition.cpp's
//        installation site — this header only declares the class and its
//        factory; the composition root decides when to use it.
//
// Guarded end-to-end by PALMIER_HAVE_OPENSSL: the whole body of the .cpp is
// inside that guard, and this header's factory function is declared
// unconditionally but returns nullptr when the guard is absent, so a caller
// needs no #ifdef of its own to ask "is a real transport available here".

#ifndef PALMIER_SERVICES_OPENSSLGENERATIVEHTTPTRANSPORT_HPP
#define PALMIER_SERVICES_OPENSSLGENERATIVEHTTPTRANSPORT_HPP

#include <chrono>
#include <memory>

#include "services/GenerativeHttpTransport.hpp"

namespace palmier::services {

/// Tunable knobs a caller may want to override in a test; every field has a
/// production-reasonable default.
struct OpenSslTransportOptions {
    /// Applied to the connect, and separately to the whole send+receive exchange
    /// (Requirement 11.3). 30 seconds each is generous enough for a cold-started
    /// generative job submission over a real network while still bounding a
    /// hung peer.
    std::chrono::milliseconds connectTimeout{10'000};
    std::chrono::milliseconds ioTimeout{30'000};

    /// When false, the server certificate chain and hostname are NOT verified.
    /// Exists only so a test can point this transport at a self-signed fixture
    /// server without needing that fixture to be CA-signed; defaults to true
    /// (verification ON) so Requirement 11.1 holds for every caller that does
    /// not explicitly opt out. Production code (ApplicationComposition) never
    /// sets this to false.
    bool verifyServerCertificate = true;
};

/// True iff this build was compiled with PALMIER_HAVE_OPENSSL, i.e. iff
/// makeOpenSslGenerativeHttpTransport() can return a non-null transport. Exposed
/// so a caller (or a test) can distinguish "no transport was requested" from
/// "no TLS-capable transport exists in this build" without depending on the
/// preprocessor symbol directly.
[[nodiscard]] bool openSslGenerativeHttpTransportAvailable() noexcept;

/// Construct a real, OpenSSL-backed HTTPS transport. Returns nullptr when this
/// build has no PALMIER_HAVE_OPENSSL, in which case the caller should install
/// makeUnavailableGenerativeHttpTransport() instead (Requirement 11.7's "falls
/// back to the unavailable transport only where the build excludes TLS
/// support").
[[nodiscard]] std::unique_ptr<GenerativeHttpTransport> makeOpenSslGenerativeHttpTransport(
    OpenSslTransportOptions options = {});

}  // namespace palmier::services

#endif  // PALMIER_SERVICES_OPENSSLGENERATIVEHTTPTRANSPORT_HPP
