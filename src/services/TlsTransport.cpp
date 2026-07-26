// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/TlsTransport.cpp — implementation of the optional TLS layer
// (task 6.3; Requirements 10.6, 10.12).
//
// The whole file compiles in both configurations. With `PALMIER_HAVE_OPENSSL`
// defined it is the real OpenSSL 3.x implementation; without it every entry point
// still exists and reports `Unsupported`, so a host with no OpenSSL configures,
// builds and tests exactly as before and the gate simply treats configured TLS
// material as an unmet prerequisite (design.md's migration rule: no stage may
// break configuration on a host that configured at the previous stage).
//
// Reading a file before parsing it is deliberate, not defensive: it is the only
// way to tell Requirement 10.12's "cannot be read" apart from its "cannot be
// parsed", and the requirement asks the startup error to say which occurred.

#include "services/TlsTransport.hpp"

#include <fstream>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(PALMIER_HAVE_OPENSSL)
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#endif

namespace palmier::services {
namespace {

/// Can this path be opened and read at all? Returns an `Io` error naming the path
/// and the condition when it cannot — Requirement 10.12's first condition.
Result<void> ensureReadable(const std::filesystem::path& path, std::string_view role) {
    std::error_code ec;
    if (path.empty()) {
        return err(makeError(ErrorCode::Io,
                             "TLS " + std::string(role) + " path is empty, so it cannot be read"));
    }
    if (!std::filesystem::exists(path, ec) || ec) {
        return err(makeError(ErrorCode::Io,
                             "TLS " + std::string(role) + " '" + path.string() +
                                 "' cannot be read (no such file)"));
    }
    if (std::filesystem::is_directory(path, ec)) {
        return err(makeError(ErrorCode::Io,
                             "TLS " + std::string(role) + " '" + path.string() +
                                 "' cannot be read (it is a directory)"));
    }
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return err(makeError(ErrorCode::Io,
                             "TLS " + std::string(role) + " '" + path.string() +
                                 "' cannot be read (permission denied or unreadable)"));
    }
    char probe = 0;
    in.read(&probe, 1);
    if (in.bad()) {
        return err(makeError(ErrorCode::Io,
                             "TLS " + std::string(role) + " '" + path.string() +
                                 "' cannot be read (read error)"));
    }
    return ok();
}

Result<void> ensureBothReadable(const std::filesystem::path& certificate,
                                const std::filesystem::path& privateKey) {
    if (Result<void> r = ensureReadable(certificate, "certificate"); r.isError()) return r;
    return ensureReadable(privateKey, "private key");
}

Result<void> unavailable() {
    return err(unsupported(
        "TLS is configured but this build has no TLS transport: OpenSSL was not "
        "found at configure time, so PALMIER_HAVE_OPENSSL is undefined"));
}

#if defined(PALMIER_HAVE_OPENSSL)

/// Build a server `SSL_CTX` from the material, classifying every failure the way
/// Requirement 10.12 enumerates. On success the caller owns the context.
///
/// The certificate and the key are parsed *independently* before either is
/// installed, and the pairing is then checked explicitly. That order is what makes
/// the three conditions distinguishable: `SSL_CTX_use_PrivateKey_file` refuses a
/// key that does not match the already-installed certificate, so installing first
/// would report a genuine mismatch as a parse failure and Requirement 10.12's
/// "indicating which of those three conditions occurred" would be wrong.
Result<SSL_CTX*> makeServerContext(const std::filesystem::path& certificate,
                                  const std::filesystem::path& privateKey) {
    if (Result<void> readable = ensureBothReadable(certificate, privateKey);
        readable.isError()) {
        return err<SSL_CTX*>(std::move(readable).error());
    }

    ERR_clear_error();

    X509*     cert = nullptr;
    EVP_PKEY* key = nullptr;
    // Single cleanup path, so no early return can leak an OpenSSL object.
    const auto finish = [&cert, &key](Result<SSL_CTX*> outcome) {
        if (cert != nullptr) X509_free(cert);
        if (key != nullptr) EVP_PKEY_free(key);
        ERR_clear_error();
        return outcome;
    };

    if (BIO* certBio = BIO_new_file(certificate.string().c_str(), "r"); certBio != nullptr) {
        cert = PEM_read_bio_X509(certBio, nullptr, nullptr, nullptr);
        BIO_free(certBio);
    }
    if (cert == nullptr) {
        return finish(err<SSL_CTX*>(invalidArgument("TLS certificate '" + certificate.string() +
                                                   "' cannot be parsed as a PEM certificate")));
    }

    if (BIO* keyBio = BIO_new_file(privateKey.string().c_str(), "r"); keyBio != nullptr) {
        key = PEM_read_bio_PrivateKey(keyBio, nullptr, nullptr, nullptr);
        BIO_free(keyBio);
    }
    if (key == nullptr) {
        return finish(err<SSL_CTX*>(invalidArgument("TLS private key '" + privateKey.string() +
                                                   "' cannot be parsed as a PEM private key")));
    }

    if (X509_check_private_key(cert, key) != 1) {
        return finish(err<SSL_CTX*>(failedPrecondition(
            "TLS certificate '" + certificate.string() + "' and private key '" +
            privateKey.string() + "' do not form a matching pair")));
    }

    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (ctx == nullptr) {
        return finish(err<SSL_CTX*>(
            makeError(ErrorCode::Internal, "TLS server context could not be created")));
    }
    // TLS 1.2 is the floor: the endpoint is a fresh surface, so there is no legacy
    // client to accommodate.
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

    if (SSL_CTX_use_certificate(ctx, cert) != 1 || SSL_CTX_use_PrivateKey(ctx, key) != 1) {
        SSL_CTX_free(ctx);
        return finish(err<SSL_CTX*>(makeError(
            ErrorCode::Internal,
            "TLS certificate '" + certificate.string() + "' and private key '" +
                privateKey.string() + "' parsed and matched but could not be installed")));
    }
    return finish(Result<SSL_CTX*>(ctx));
}

#endif  // PALMIER_HAVE_OPENSSL

}  // namespace

bool tlsTransportAvailable() noexcept {
#if defined(PALMIER_HAVE_OPENSSL)
    return true;
#else
    return false;
#endif
}

Result<void> checkTlsMaterial(const std::filesystem::path& certificate,
                              const std::filesystem::path& privateKey) {
#if defined(PALMIER_HAVE_OPENSSL)
    Result<SSL_CTX*> ctx = makeServerContext(certificate, privateKey);
    if (ctx.isError()) return err(std::move(ctx).error());
    SSL_CTX_free(ctx.value());
    return ok();
#else
    // The paths are still checked for readability so a build without OpenSSL gives
    // the same diagnostic for a plainly missing file; the capability error is what
    // ultimately makes this an unmet prerequisite.
    (void)ensureBothReadable(certificate, privateKey);
    return unavailable();
#endif
}

// ---------------------------------------------------------------------------
// TlsContext
// ---------------------------------------------------------------------------

Result<std::unique_ptr<TlsContext>> TlsContext::create(const std::filesystem::path& certificate,
                                                       const std::filesystem::path& privateKey) {
#if defined(PALMIER_HAVE_OPENSSL)
    Result<SSL_CTX*> ctx = makeServerContext(certificate, privateKey);
    if (ctx.isError()) return err<std::unique_ptr<TlsContext>>(std::move(ctx).error());
    return ok(std::unique_ptr<TlsContext>(new TlsContext(ctx.value())));
#else
    (void)certificate;
    (void)privateKey;
    Result<void> e = unavailable();
    return err<std::unique_ptr<TlsContext>>(std::move(e).error());
#endif
}

TlsContext::~TlsContext() {
#if defined(PALMIER_HAVE_OPENSSL)
    if (native_ != nullptr) {
        SSL_CTX_free(static_cast<SSL_CTX*>(native_));
        native_ = nullptr;
    }
#endif
}

Result<std::unique_ptr<TlsConnection>> TlsContext::accept(int fd) {
#if defined(PALMIER_HAVE_OPENSSL)
    if (native_ == nullptr) {
        return err<std::unique_ptr<TlsConnection>>(
            failedPrecondition("TLS context is not initialized"));
    }
    SSL* ssl = SSL_new(static_cast<SSL_CTX*>(native_));
    if (ssl == nullptr) {
        return err<std::unique_ptr<TlsConnection>>(
            makeError(ErrorCode::Internal, "TLS session could not be created"));
    }
    if (SSL_set_fd(ssl, fd) != 1) {
        SSL_free(ssl);
        return err<std::unique_ptr<TlsConnection>>(
            makeError(ErrorCode::Io, "TLS session could not be attached to the connection"));
    }

    ERR_clear_error();
    if (SSL_accept(ssl) != 1) {
        // This is the plaintext-on-a-TLS-port path of Requirement 10.6: the
        // handshake fails, the session is discarded, and no HTTP request is ever
        // produced for the caller to dispatch.
        SSL_free(ssl);
        ERR_clear_error();
        return err<std::unique_ptr<TlsConnection>>(invalidArgument(
            "TLS handshake failed: the client did not present a TLS ClientHello "
            "(a plaintext request on a TLS port is refused, not served)"));
    }
    ERR_clear_error();
    return ok(std::unique_ptr<TlsConnection>(new TlsConnection(ssl)));
#else
    (void)fd;
    Result<void> e = unavailable();
    return err<std::unique_ptr<TlsConnection>>(std::move(e).error());
#endif
}

// ---------------------------------------------------------------------------
// TlsConnection
// ---------------------------------------------------------------------------

TlsConnection::~TlsConnection() {
#if defined(PALMIER_HAVE_OPENSSL)
    if (native_ != nullptr) {
        SSL_free(static_cast<SSL*>(native_));
        native_ = nullptr;
    }
#endif
}

long TlsConnection::read(char* buffer, std::size_t length) {
#if defined(PALMIER_HAVE_OPENSSL)
    if (native_ == nullptr || buffer == nullptr) return -1;
    const int n = SSL_read(static_cast<SSL*>(native_), buffer, static_cast<int>(length));
    if (n > 0) return n;
    const int reason = SSL_get_error(static_cast<SSL*>(native_), n);
    ERR_clear_error();
    return reason == SSL_ERROR_ZERO_RETURN ? 0 : -1;
#else
    (void)buffer;
    (void)length;
    return -1;
#endif
}

bool TlsConnection::writeAll(const char* buffer, std::size_t length) {
#if defined(PALMIER_HAVE_OPENSSL)
    if (native_ == nullptr || buffer == nullptr) return false;
    std::size_t sent = 0;
    while (sent < length) {
        const int n = SSL_write(static_cast<SSL*>(native_), buffer + sent,
                                static_cast<int>(length - sent));
        if (n <= 0) {
            ERR_clear_error();
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
#else
    (void)buffer;
    (void)length;
    return false;
#endif
}

void TlsConnection::shutdown() {
#if defined(PALMIER_HAVE_OPENSSL)
    if (native_ != nullptr) {
        SSL_shutdown(static_cast<SSL*>(native_));
        ERR_clear_error();
    }
#endif
}

}  // namespace palmier::services
