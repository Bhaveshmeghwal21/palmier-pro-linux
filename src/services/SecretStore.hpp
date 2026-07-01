// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/SecretStore.hpp — abstraction over the platform secret store used for
// secure credential persistence (design.md Security Considerations: "Store auth
// tokens via the platform secret store (libsecret / Secret Service API), not
// plaintext"; Requirement 9.5).
//
// Credentials (auth tokens and BYOK API keys) must never be written to disk in
// plaintext. On Linux the platform mechanism is the Secret Service API, accessed
// via libsecret. To keep the credential policy (ByokCredentialManager) fully
// unit-testable without a running Secret Service daemon — and without linking
// libsecret into the test binaries — all persistence goes through this narrow
// SecretStore interface. Tests inject an InMemorySecretStore; production wires in
// the libsecret-backed store returned by makeSystemSecretStore().
//
// The real libsecret implementation lives in SecretStore.cpp and is compiled in
// only when the PALMIER_HAVE_LIBSECRET build define is set (see
// src/services/CMakeLists.txt); this header is dependency-free.

#ifndef PALMIER_SERVICES_SECRETSTORE_HPP
#define PALMIER_SERVICES_SECRETSTORE_HPP

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#include "core/Error.hpp"
#include "core/Result.hpp"

namespace palmier::services {

/// A minimal key/secret store backed by the platform secret service.
///
/// Keys are opaque, stable strings chosen by the caller (e.g.
/// "palmier/byok/<user>/<provider>"). Secrets are arbitrary UTF-8 strings.
/// Implementations MUST NOT persist secrets in plaintext on disk.
class SecretStore {
public:
    virtual ~SecretStore() = default;

    /// Store (or overwrite) the secret at `key`. Returns an error (ErrorCode::Io)
    /// if the underlying store could not be written.
    [[nodiscard]] virtual Result<void> store(const std::string& key,
                                             const std::string& secret) = 0;

    /// Look up the secret at `key`. Returns std::nullopt (not an error) when no
    /// secret is stored under that key; an Error only on a genuine backend
    /// failure.
    [[nodiscard]] virtual Result<std::optional<std::string>> lookup(
        const std::string& key) const = 0;

    /// Remove the secret at `key`. Removing a missing key succeeds (idempotent).
    [[nodiscard]] virtual Result<void> remove(const std::string& key) = 0;
};

/// An in-memory SecretStore for tests and headless use.
///
/// This is deliberately header-only and free of any platform dependency so test
/// binaries can exercise the credential policy without a Secret Service daemon
/// (or libsecret) present. It is NOT secure persistence — secrets live only for
/// the lifetime of the process — and must never be used in production.
class InMemorySecretStore final : public SecretStore {
public:
    [[nodiscard]] Result<void> store(const std::string& key,
                                     const std::string& secret) override {
        entries_[key] = secret;
        return ok();
    }

    [[nodiscard]] Result<std::optional<std::string>> lookup(
        const std::string& key) const override {
        const auto it = entries_.find(key);
        if (it == entries_.end()) {
            return Result<std::optional<std::string>>(std::optional<std::string>{});
        }
        return Result<std::optional<std::string>>(std::optional<std::string>{it->second});
    }

    [[nodiscard]] Result<void> remove(const std::string& key) override {
        entries_.erase(key);
        return ok();
    }

    /// Test helper: number of stored secrets.
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    /// Test helper: whether a key is present without exposing the secret value.
    [[nodiscard]] bool contains(const std::string& key) const {
        return entries_.find(key) != entries_.end();
    }

private:
    std::unordered_map<std::string, std::string> entries_;
};

/// Construct the platform-backed secret store.
///
/// When Palmier is built with libsecret support (PALMIER_HAVE_LIBSECRET) this
/// returns a store backed by the Secret Service API. When built without it, the
/// call returns an ErrorCode::Unsupported error so callers can degrade
/// gracefully rather than silently persisting secrets insecurely.
[[nodiscard]] Result<std::unique_ptr<SecretStore>> makeSystemSecretStore();

} // namespace palmier::services

#endif // PALMIER_SERVICES_SECRETSTORE_HPP
