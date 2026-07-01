// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/LocalizationManager.cpp — localization policy implementation.
//
// See LocalizationManager.hpp for the Requirement 12 mapping. This translation
// unit is Qt-free and dependency-free (only the standard library and the core
// Result/Error types) so it links into headless/test builds unchanged.

#include "services/LocalizationManager.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <string_view>
#include <system_error>

#include <filesystem>

namespace palmier::services {

namespace {

// Lowercase a copy of `s` (ASCII only — language tags are ASCII).
[[nodiscard]] std::string toLowerAscii(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

// Trim leading/trailing ASCII whitespace.
[[nodiscard]] std::string_view trim(std::string_view s) noexcept {
    const auto isSpace = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    std::size_t begin = 0;
    while (begin < s.size() && isSpace(s[begin])) ++begin;
    std::size_t end = s.size();
    while (end > begin && isSpace(s[end - 1])) --end;
    return s.substr(begin, end - begin);
}

// Canonicalize a locale/language token for matching: keep only the language and
// optional script/region up to the first '.', '@', or ':' modifier, replace '_'
// with '-', and lowercase. E.g. "PT_br.UTF-8" -> "pt-br", "zh-Hant" -> "zh-hant".
[[nodiscard]] std::string canonicalizeForMatch(std::string_view raw) {
    std::string_view trimmed = trim(raw);
    // Drop encoding/modifier suffixes ("fr_FR.UTF-8", "de@euro", "en:en_GB").
    for (const char delimiter : {'.', '@', ':'}) {
        if (const auto pos = trimmed.find(delimiter); pos != std::string_view::npos) {
            trimmed = trimmed.substr(0, pos);
        }
    }
    std::string out(trimmed);
    std::replace(out.begin(), out.end(), '_', '-');
    return toLowerAscii(out);
}

// The primary subtag (text before the first '-') of a canonicalized tag.
[[nodiscard]] std::string_view primarySubtag(std::string_view canonical) noexcept {
    const auto dash = canonical.find('-');
    return dash == std::string_view::npos ? canonical : canonical.substr(0, dash);
}

} // namespace

const std::vector<LanguageTag>& supportedLanguages() {
    // Mirrors the macOS edition's localized interface languages (Requirement
    // 12.1). English is first and is the fallback language (12.3).
    static const std::vector<LanguageTag> kLanguages = {
        "en",       // English (base / fallback)
        "es",       // Spanish
        "fr",       // French
        "de",       // German
        "it",       // Italian
        "ja",       // Japanese
        "ko",       // Korean
        "zh-Hans",  // Simplified Chinese
        "zh-Hant",  // Traditional Chinese
        "pt-BR",    // Brazilian Portuguese
        "ru",       // Russian
        "nl",       // Dutch
    };
    return kLanguages;
}

bool isSupportedLanguage(const LanguageTag& tag) {
    const auto& all = supportedLanguages();
    return std::find(all.begin(), all.end(), tag) != all.end();
}

std::optional<LanguageTag> resolveSupportedLanguage(const LanguageTag& tag) {
    const std::string canonical = canonicalizeForMatch(tag);
    if (canonical.empty()) return std::nullopt;

    const auto& all = supportedLanguages();

    // 1. Exact (case-insensitive) match against the supported set.
    for (const auto& supported : all) {
        if (canonicalizeForMatch(supported) == canonical) return supported;
    }

    // 2. Primary-subtag match, but only when unambiguous (exactly one supported
    //    language shares that primary subtag). This keeps "zh" — shared by
    //    zh-Hans and zh-Hant — from resolving to an arbitrary script.
    const std::string_view wanted = primarySubtag(canonical);
    const LanguageTag* sole = nullptr;
    for (const auto& supported : all) {
        if (primarySubtag(canonicalizeForMatch(supported)) == wanted) {
            if (sole != nullptr) {
                sole = nullptr;  // Ambiguous: more than one candidate.
                break;
            }
            sole = &supported;
        }
    }
    if (sole != nullptr) return *sole;

    return std::nullopt;
}

// ---------------------------------------------------------------------------
// File-backed LanguagePreferenceStore.
// ---------------------------------------------------------------------------

namespace {

/// Plain-text, single-key config file persisting the selected language.
class FileLanguagePreferenceStore final : public LanguagePreferenceStore {
public:
    explicit FileLanguagePreferenceStore(std::string filePath)
        : path_(std::move(filePath)) {}

    [[nodiscard]] Result<std::optional<LanguageTag>> load() const override {
        namespace fs = std::filesystem;
        std::error_code ec;
        if (!fs::exists(fs::path(path_), ec) || ec) {
            return Result<std::optional<LanguageTag>>(std::optional<LanguageTag>{});
        }

        std::ifstream in(path_);
        if (!in) {
            return err<std::optional<LanguageTag>>(
                makeError(ErrorCode::Io, "unable to open language file: " + path_));
        }

        std::string line;
        while (std::getline(in, line)) {
            const std::string_view view = trim(line);
            if (view.empty() || view.front() == '#') continue;
            const auto eq = view.find('=');
            if (eq == std::string_view::npos) continue;
            if (trim(view.substr(0, eq)) == "language") {
                const std::string_view value = trim(view.substr(eq + 1));
                if (!value.empty()) {
                    return Result<std::optional<LanguageTag>>(
                        std::optional<LanguageTag>{std::string(value)});
                }
            }
        }
        // File exists but carries no usable "language=" line: treat as "unset"
        // rather than an error so a first launch can proceed to the default.
        return Result<std::optional<LanguageTag>>(std::optional<LanguageTag>{});
    }

    [[nodiscard]] Result<void> save(const LanguageTag& tag) override {
        namespace fs = std::filesystem;
        const fs::path file(path_);
        std::error_code ec;
        if (file.has_parent_path()) {
            fs::create_directories(file.parent_path(), ec);
            if (ec) {
                return err(makeError(ErrorCode::Io,
                                     "unable to create config directory for " + path_ +
                                         ": " + ec.message()));
            }
        }

        std::ofstream out(path_, std::ios::trunc);
        if (!out) {
            return err(makeError(ErrorCode::Io,
                                 "unable to write language file: " + path_));
        }
        out << "# Palmier Pro Linux — persisted interface language (Requirement 12.4)\n";
        out << "language=" << tag << '\n';
        if (!out) {
            return err(makeError(ErrorCode::Io,
                                 "failed while writing language file: " + path_));
        }
        return ok();
    }

private:
    std::string path_;
};

} // namespace

std::string defaultLanguagePreferencePath() {
    namespace fs = std::filesystem;
    fs::path base;
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && xdg[0] != '\0') {
        base = fs::path(xdg);
    } else if (const char* home = std::getenv("HOME"); home && home[0] != '\0') {
        base = fs::path(home) / ".config";
    } else {
        base = fs::temp_directory_path();
    }
    return (base / "palmier" / "language.conf").string();
}

std::unique_ptr<LanguagePreferenceStore> makeFileLanguagePreferenceStore() {
    return std::make_unique<FileLanguagePreferenceStore>(defaultLanguagePreferencePath());
}

std::unique_ptr<LanguagePreferenceStore> makeFileLanguagePreferenceStore(
    std::string filePath) {
    return std::make_unique<FileLanguagePreferenceStore>(std::move(filePath));
}

// ---------------------------------------------------------------------------
// System-language provider.
// ---------------------------------------------------------------------------

SystemLanguageProvider makeEnvSystemLanguageProvider() {
    return []() -> std::optional<LanguageTag> {
        for (const char* var : {"LC_ALL", "LC_MESSAGES", "LANG", "LANGUAGE"}) {
            if (const char* value = std::getenv(var); value && value[0] != '\0') {
                // The "C"/"POSIX" locales carry no meaningful UI language.
                std::string_view v(value);
                if (v == "C" || v == "POSIX") continue;
                return std::string(value);
            }
        }
        return std::nullopt;
    };
}

// ---------------------------------------------------------------------------
// LocalizationManager
// ---------------------------------------------------------------------------

LocalizationManager::LocalizationManager(Config config)
    : translations_(std::move(config.translations)),
      store_(config.store),
      systemLanguage_(std::move(config.systemLanguage)) {
    current_ = chooseLaunchLanguage();
}

LanguageTag LocalizationManager::chooseLaunchLanguage() const {
    // Requirement 12.4: a previously persisted selection is reapplied on launch,
    // provided it is still a supported language.
    if (store_ != nullptr) {
        if (auto loaded = store_->load(); loaded.isOk()) {
            if (const auto& persisted = loaded.value(); persisted.has_value()) {
                if (auto resolved = resolveSupportedLanguage(*persisted)) {
                    return *resolved;
                }
            }
        }
        // A genuine load failure is non-fatal: fall through to the default.
    }

    // Requirement 12.5: no prior selection -> system language when supported,
    // otherwise English.
    if (systemLanguage_) {
        if (const auto sys = systemLanguage_()) {
            if (auto resolved = resolveSupportedLanguage(*sys)) {
                return *resolved;
            }
        }
    }
    return kBaseLanguage;
}

const std::vector<LanguageTag>& LocalizationManager::availableLanguages() const {
    return supportedLanguages();
}

std::optional<std::string> LocalizationManager::lookup(const LanguageTag& lang,
                                                       const std::string& key) const {
    const auto langIt = translations_.find(lang);
    if (langIt == translations_.end()) return std::nullopt;
    const auto keyIt = langIt->second.find(key);
    if (keyIt == langIt->second.end()) return std::nullopt;
    return keyIt->second;
}

std::string LocalizationManager::text(const std::string& key) const {
    // Requirement 12.3: current language, then English, then the key itself.
    if (auto value = lookup(current_, key)) return *value;
    if (current_ != kBaseLanguage) {
        if (auto english = lookup(kBaseLanguage, key)) return *english;
    }
    return key;
}

bool LocalizationManager::hasTranslation(const std::string& key) const {
    return lookup(current_, key).has_value();
}

Result<void> LocalizationManager::selectLanguage(const LanguageTag& tag) {
    // Requirement 12.1/12.2: only supported languages are selectable.
    if (!isSupportedLanguage(tag)) {
        return err(invalidArgument("unsupported interface language: " + tag));
    }
    if (tag == current_) {
        return ok();  // No-op: no switch, no notification, no re-persist.
    }

    // Switch in-memory and notify observers so the UI live-updates without a
    // restart (Requirement 12.2).
    current_ = tag;
    notify();

    // Persist the selection for subsequent launches (Requirement 12.4). The
    // in-memory switch above already stands even if persistence fails.
    if (store_ != nullptr) {
        if (auto saved = store_->save(tag); saved.isError()) {
            return saved;
        }
    }
    return ok();
}

void LocalizationManager::notify() const {
    // Copy handlers so an observer that unsubscribes during the callback does not
    // invalidate the iteration.
    const auto snapshot = observers_->callbacks;
    for (const auto& [id, cb] : snapshot) {
        (void)id;
        if (cb) cb(current_);
    }
}

Subscription LocalizationManager::observe(std::function<void(const LanguageTag&)> cb) {
    const std::size_t id = observers_->nextId++;
    observers_->callbacks.emplace(id, std::move(cb));

    std::weak_ptr<Observers> weak = observers_;
    return Subscription([weak, id]() {
        if (auto observers = weak.lock()) {
            observers->callbacks.erase(id);
        }
    });
}

} // namespace palmier::services
