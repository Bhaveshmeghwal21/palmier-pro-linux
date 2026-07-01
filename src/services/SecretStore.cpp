// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/SecretStore.cpp — platform secret store factory and the libsecret
// (Secret Service API) backed implementation.
//
// The libsecret code is compiled in ONLY when the build defines
// PALMIER_HAVE_LIBSECRET (set by src/services/CMakeLists.txt when the libsecret
// dependency is available). This lets the SecretStore interface — and the
// credential policy that depends on it — be compiled and unit-tested without
// linking libsecret, while production builds get real, non-plaintext storage via
// the platform Secret Service (design.md Security Considerations; Requirement
// 9.5).

#include "services/SecretStore.hpp"

#ifdef PALMIER_HAVE_LIBSECRET
#include <libsecret/secret.h>
#endif

namespace palmier::services {

#ifdef PALMIER_HAVE_LIBSECRET

namespace {

// Schema describing how Palmier's secrets are stored in the Secret Service. The
// single "key" attribute is the caller-chosen opaque identifier; libsecret uses
// it both to file the secret and to look it up again. The schema name is stable
// so entries remain retrievable across releases.
const SecretSchema* palmierSecretSchema() {
    static const SecretSchema schema = {
        "io.palmier.Credentials",
        SECRET_SCHEMA_NONE,
        {
            {"key", SECRET_SCHEMA_ATTRIBUTE_STRING},
            {nullptr, static_cast<SecretSchemaAttributeType>(0)},
        },
        // Reserved fields.
        0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    };
    return &schema;
}

/// libsecret-backed SecretStore. Each operation is synchronous; libsecret talks
/// to the Secret Service (e.g. gnome-keyring / KWallet's Secret Service bridge)
/// over D-Bus, which transparently encrypts secrets at rest.
class LibSecretStore final : public SecretStore {
public:
    [[nodiscard]] Result<void> store(const std::string& key,
                                     const std::string& secret) override {
        GError* error = nullptr;
        const gboolean stored = secret_password_store_sync(
            palmierSecretSchema(), SECRET_COLLECTION_DEFAULT,
            /*label*/ ("Palmier: " + key).c_str(), secret.c_str(),
            /*cancellable*/ nullptr, &error, "key", key.c_str(), nullptr);
        if (error != nullptr) {
            const std::string message = error->message ? error->message : "unknown error";
            g_error_free(error);
            return err(makeError(ErrorCode::Io,
                                 "Failed to store credential securely: " + message));
        }
        if (stored == FALSE) {
            return err(makeError(ErrorCode::Io,
                                 "The platform secret store rejected the credential."));
        }
        return ok();
    }

    [[nodiscard]] Result<std::optional<std::string>> lookup(
        const std::string& key) const override {
        GError* error = nullptr;
        gchar* password = secret_password_lookup_sync(
            palmierSecretSchema(), /*cancellable*/ nullptr, &error, "key",
            key.c_str(), nullptr);
        if (error != nullptr) {
            const std::string message = error->message ? error->message : "unknown error";
            g_error_free(error);
            return err<std::optional<std::string>>(makeError(
                ErrorCode::Io, "Failed to read credential from secure store: " + message));
        }
        if (password == nullptr) {
            return Result<std::optional<std::string>>(std::optional<std::string>{});
        }
        std::optional<std::string> value{std::string(password)};
        secret_password_free(password);
        return Result<std::optional<std::string>>(std::move(value));
    }

    [[nodiscard]] Result<void> remove(const std::string& key) override {
        GError* error = nullptr;
        // A missing key returns FALSE with no error; that is a success for our
        // idempotent remove() contract.
        (void)secret_password_clear_sync(palmierSecretSchema(),
                                         /*cancellable*/ nullptr, &error, "key",
                                         key.c_str(), nullptr);
        if (error != nullptr) {
            const std::string message = error->message ? error->message : "unknown error";
            g_error_free(error);
            return err(makeError(ErrorCode::Io,
                                 "Failed to remove credential from secure store: " + message));
        }
        return ok();
    }
};

} // namespace

Result<std::unique_ptr<SecretStore>> makeSystemSecretStore() {
    return Result<std::unique_ptr<SecretStore>>(
        std::unique_ptr<SecretStore>(std::make_unique<LibSecretStore>()));
}

#else // !PALMIER_HAVE_LIBSECRET

Result<std::unique_ptr<SecretStore>> makeSystemSecretStore() {
    return err<std::unique_ptr<SecretStore>>(makeError(
        ErrorCode::Unsupported,
        "Secure credential storage is unavailable: this build was compiled "
        "without libsecret (Secret Service) support."));
}

#endif // PALMIER_HAVE_LIBSECRET

} // namespace palmier::services
