// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/LocalizationManager.hpp — UI-agnostic interface localization.
//
// The Localization_Manager (design.md; Requirement 12) presents the interface in
// the user's selected language. In the Linux port the eventual UI is Qt 6, whose
// tr()/QTranslator machinery ultimately backs the on-screen strings. However the
// *policy* that governs localization — which languages are offered, how a missing
// translation falls back to English, how the selection is persisted and reapplied
// on launch, and how the default language is chosen on first run — is plain C++
// with no Qt dependency. Keeping that logic here (a Qt-free service) makes it
// unit-testable headlessly, and lets the Qt UI (task 19.x) bind to it: on a
// language change the UI observes the notification and re-pulls its strings
// (via this manager or its own QTranslator swap) without an application restart.
//
// Requirement 12 coverage:
//   12.1  supportedLanguages() exposes the same set as the macOS edition.
//   12.2  selectLanguage() switches synchronously and notifies observers so the
//         UI can live-update visible text without a restart.
//   12.3  text() returns the English string when the selected language lacks a
//         translation for a key (and the key itself only as a last resort).
//   12.4  the selection is persisted through a LanguagePreferenceStore seam and
//         reapplied automatically on construction (i.e. on the next launch).
//   12.5  on first launch (no persisted selection) the manager defaults to the
//         system language when supported, otherwise to English.
//
// Persistence and system-language lookup are abstracted behind seams so tests can
// inject deterministic fakes (mirroring services/SecretStore.hpp).

#ifndef PALMIER_SERVICES_LOCALIZATIONMANAGER_HPP
#define PALMIER_SERVICES_LOCALIZATIONMANAGER_HPP

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/Error.hpp"
#include "core/Result.hpp"

namespace palmier::services {

/// A BCP-47-style interface-language identifier (e.g. "en", "pt-BR", "zh-Hans").
using LanguageTag = std::string;

/// The canonical English base language. Every key that has any translation at
/// all is expected to have an English string, which is the universal fallback
/// (Requirement 12.3).
inline constexpr const char* kBaseLanguage = "en";

/// The set of interface languages Palmier offers, mirroring the macOS edition.
///
/// This list documents the macOS edition's localized languages: English,
/// Spanish, French, German, Italian, Japanese, Korean, Simplified Chinese,
/// Traditional Chinese, Brazilian Portuguese, Russian, and Dutch. English is
/// first and is the fallback language (Requirements 12.1, 12.3). If the macOS
/// edition's language set changes, update this single list to stay in parity.
[[nodiscard]] const std::vector<LanguageTag>& supportedLanguages();

/// Whether `tag` is one of the supported interface languages (exact match).
[[nodiscard]] bool isSupportedLanguage(const LanguageTag& tag);

/// Resolve an arbitrary locale identifier (possibly with a region/underscore, or
/// differing case, e.g. "en_US", "PT-br") to a supported LanguageTag, or
/// std::nullopt when it cannot be unambiguously mapped. Used to interpret both a
/// persisted value and the system locale (Requirements 12.4, 12.5). Matching is:
///   1. exact (case-insensitive, '_' treated as '-') against the supported set;
///   2. otherwise the primary subtag (e.g. "en" of "en-GB") — but only when
///      exactly one supported language shares that primary subtag, so ambiguous
///      primaries (e.g. "zh", shared by zh-Hans and zh-Hant) do not mis-resolve.
[[nodiscard]] std::optional<LanguageTag> resolveSupportedLanguage(
    const LanguageTag& tag);

// ---------------------------------------------------------------------------
// Seam 1: persistence of the selected language (Requirement 12.4).
// ---------------------------------------------------------------------------

/// Persists and restores the user's selected interface language.
///
/// Kept behind an interface (mirroring services/SecretStore.hpp) so the
/// selection/default/fallback policy is testable without touching disk. Tests
/// inject InMemoryLanguagePreferenceStore; production wires in the file-backed
/// store from makeFileLanguagePreferenceStore().
class LanguagePreferenceStore {
public:
    virtual ~LanguagePreferenceStore() = default;

    /// Load the persisted language. Returns std::nullopt (not an error) when the
    /// user has never selected a language; an Error only on a genuine backend
    /// failure.
    [[nodiscard]] virtual Result<std::optional<LanguageTag>> load() const = 0;

    /// Persist (or overwrite) the selected language.
    [[nodiscard]] virtual Result<void> save(const LanguageTag& tag) = 0;
};

/// An in-memory LanguagePreferenceStore for tests and headless use.
///
/// Header-only and dependency-free so test binaries can exercise the localization
/// policy without any config file present. It does NOT survive process exit and
/// must not be used where persistence across launches is required.
class InMemoryLanguagePreferenceStore final : public LanguagePreferenceStore {
public:
    InMemoryLanguagePreferenceStore() = default;
    explicit InMemoryLanguagePreferenceStore(LanguageTag initial)
        : value_(std::move(initial)) {}

    [[nodiscard]] Result<std::optional<LanguageTag>> load() const override {
        return Result<std::optional<LanguageTag>>(value_);
    }

    [[nodiscard]] Result<void> save(const LanguageTag& tag) override {
        value_ = tag;
        return ok();
    }

    /// Test helper: the currently persisted value, if any.
    [[nodiscard]] const std::optional<LanguageTag>& persisted() const noexcept {
        return value_;
    }

private:
    std::optional<LanguageTag> value_;
};

/// Construct a file-backed LanguagePreferenceStore.
///
/// The default path is $XDG_CONFIG_HOME/palmier/language.conf (falling back to
/// ~/.config/palmier/language.conf). Callers may pass an explicit path (used by
/// tests and by callers managing their own config location).
[[nodiscard]] std::unique_ptr<LanguagePreferenceStore>
makeFileLanguagePreferenceStore();
[[nodiscard]] std::unique_ptr<LanguagePreferenceStore>
makeFileLanguagePreferenceStore(std::string filePath);

/// Resolve the default per-user config path for the language selection.
[[nodiscard]] std::string defaultLanguagePreferencePath();

// ---------------------------------------------------------------------------
// Seam 2: system-language lookup (Requirement 12.5).
// ---------------------------------------------------------------------------

/// Returns the host system's preferred UI language as a raw locale identifier
/// (e.g. "fr_FR.UTF-8", "de", "pt_BR"), or std::nullopt when it cannot be
/// determined. The manager normalizes/resolves the result via
/// resolveSupportedLanguage(). Tests inject a fixed value; production may pass
/// makeEnvSystemLanguageProvider() which reads the standard locale environment.
using SystemLanguageProvider = std::function<std::optional<LanguageTag>()>;

/// A SystemLanguageProvider that inspects the standard POSIX locale environment
/// variables (LC_ALL, LC_MESSAGES, LANG, then LANGUAGE), returning the first
/// non-empty value. Dependency-free.
[[nodiscard]] SystemLanguageProvider makeEnvSystemLanguageProvider();

// ---------------------------------------------------------------------------
// Observer subscription (Requirement 12.2 — live switching without restart).
// ---------------------------------------------------------------------------

/// RAII handle for a language-change observer. Destroying (or reset()-ing) the
/// handle unregisters the callback. Move-only.
class Subscription {
public:
    Subscription() = default;
    explicit Subscription(std::function<void()> unsubscribe)
        : unsubscribe_(std::move(unsubscribe)) {}

    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;

    Subscription(Subscription&& other) noexcept
        : unsubscribe_(std::move(other.unsubscribe_)) {
        other.unsubscribe_ = nullptr;
    }
    Subscription& operator=(Subscription&& other) noexcept {
        if (this != &other) {
            reset();
            unsubscribe_ = std::move(other.unsubscribe_);
            other.unsubscribe_ = nullptr;
        }
        return *this;
    }

    ~Subscription() { reset(); }

    /// Unregister early; safe to call more than once.
    void reset() {
        if (unsubscribe_) {
            unsubscribe_();
            unsubscribe_ = nullptr;
        }
    }

    /// Whether this handle still refers to a live registration.
    [[nodiscard]] bool active() const noexcept {
        return static_cast<bool>(unsubscribe_);
    }

private:
    std::function<void()> unsubscribe_;
};

// ---------------------------------------------------------------------------
// LocalizationManager
// ---------------------------------------------------------------------------

/// Owns the selected interface language and the translation catalog, applies the
/// English fallback, persists selections, chooses the launch default, and
/// notifies observers so the UI can live-switch. UI-agnostic and Qt-free.
class LocalizationManager {
public:
    /// A translation catalog: language tag -> (message key -> localized string).
    /// The "en" entry is the base; any key present for another language is
    /// expected to also exist under "en" so fallback (12.3) has a value.
    using Catalog = std::map<LanguageTag, std::map<std::string, std::string>>;

    struct Config {
        /// Translations keyed by language then message key.
        Catalog translations;

        /// Persistence seam (not owned). When null, selections are not persisted
        /// and every construction chooses the launch default (12.5).
        LanguagePreferenceStore* store = nullptr;

        /// System-language seam. When null, the default on first launch is
        /// English (12.5).
        SystemLanguageProvider systemLanguage;
    };

    /// Construct and immediately apply the launch language (Requirements 12.4,
    /// 12.5): a supported persisted selection if one exists, otherwise the
    /// system language when supported, otherwise English.
    explicit LocalizationManager(Config config);

    /// The languages offered in settings (Requirement 12.1).
    [[nodiscard]] const std::vector<LanguageTag>& availableLanguages() const;

    /// The language currently applied.
    [[nodiscard]] const LanguageTag& currentLanguage() const noexcept {
        return current_;
    }

    /// Select a supported interface language (Requirements 12.2, 12.4).
    ///
    /// On success the manager switches to `tag`, notifies all observers so the UI
    /// can update visible text without a restart, and persists the selection via
    /// the store (if any). Selecting an unsupported language returns
    /// ErrorCode::InvalidArgument and changes nothing. Selecting the already-
    /// current language is a no-op success (no notification, no re-persist). If
    /// persistence fails the in-memory switch and notification still stand and
    /// the persistence error is returned.
    [[nodiscard]] Result<void> selectLanguage(const LanguageTag& tag);

    /// Return the localized string for `key` in the current language, falling
    /// back to the English string when the current language lacks it, and to
    /// `key` itself when neither language defines it (Requirement 12.3).
    [[nodiscard]] std::string text(const std::string& key) const;

    /// Whether `key` has an explicit translation in the current language (i.e.
    /// text() would not need to fall back). Useful for diagnostics/tests.
    [[nodiscard]] bool hasTranslation(const std::string& key) const;

    /// Register an observer invoked (synchronously) after each language change,
    /// receiving the new language (Requirement 12.2). The returned Subscription
    /// unregisters the observer when destroyed.
    [[nodiscard]] Subscription observe(std::function<void(const LanguageTag&)> cb);

private:
    struct Observers {
        std::map<std::size_t, std::function<void(const LanguageTag&)>> callbacks;
        std::size_t nextId = 0;
    };

    [[nodiscard]] std::optional<std::string> lookup(const LanguageTag& lang,
                                                    const std::string& key) const;
    [[nodiscard]] LanguageTag chooseLaunchLanguage() const;
    void notify() const;

    Catalog                    translations_;
    LanguagePreferenceStore*   store_ = nullptr;
    SystemLanguageProvider     systemLanguage_;
    LanguageTag                current_;
    std::shared_ptr<Observers> observers_ = std::make_shared<Observers>();
};

} // namespace palmier::services

#endif // PALMIER_SERVICES_LOCALIZATIONMANAGER_HPP
