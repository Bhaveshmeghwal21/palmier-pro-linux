// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/LocalizationManager_test.cpp — unit tests for the localization policy
// (task 17.2). Focus areas from the task: English fallback for missing
// translations (Requirement 12.3) and the system-language / English default
// logic (Requirement 12.5). Also covers the supported set (12.1), live-switch
// notification (12.2), and persistence + reapply-on-launch (12.4).

#include "services/LocalizationManager.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace palmier::services {
namespace {

// A translation catalog with English complete and other languages partial, so
// fallback behavior can be exercised.
LocalizationManager::Catalog makeCatalog() {
    return {
        {"en",
         {{"menu.file", "File"}, {"menu.edit", "Edit"}, {"action.export", "Export"}}},
        {"fr",
         {{"menu.file", "Fichier"}, {"menu.edit", "Édition"}}},  // no "action.export"
        {"de",
         {{"menu.file", "Datei"}}},                              // only one key
    };
}

// ---- Requirement 12.1: supported set --------------------------------------

TEST(LocalizationSupportedSet, MirrorsMacEditionAndLeadsWithEnglish) {
    const auto& langs = supportedLanguages();
    // English is present and first (it is the fallback language).
    ASSERT_FALSE(langs.empty());
    EXPECT_EQ(langs.front(), "en");

    // The documented macOS-parity set is available for selection.
    for (const char* expected : {"en", "es", "fr", "de", "it", "ja", "ko",
                                 "zh-Hans", "zh-Hant", "pt-BR", "ru", "nl"}) {
        EXPECT_TRUE(isSupportedLanguage(expected)) << expected;
    }
    // An unrelated tag is not offered.
    EXPECT_FALSE(isSupportedLanguage("xx"));
    EXPECT_FALSE(isSupportedLanguage("klingon"));
}

// ---- Requirement 12.3: English fallback for missing translations ----------

TEST(LocalizationFallback, ReturnsSelectedLanguageWhenPresent) {
    LocalizationManager mgr(LocalizationManager::Config{makeCatalog(), nullptr, {}});
    ASSERT_TRUE(mgr.selectLanguage("fr").isOk());

    EXPECT_EQ(mgr.text("menu.file"), "Fichier");
    EXPECT_EQ(mgr.text("menu.edit"), "Édition");
}

TEST(LocalizationFallback, FallsBackToEnglishForUntranslatedKey) {
    LocalizationManager mgr(LocalizationManager::Config{makeCatalog(), nullptr, {}});
    ASSERT_TRUE(mgr.selectLanguage("fr").isOk());

    // "action.export" has no French translation -> English text (Req 12.3).
    EXPECT_FALSE(mgr.hasTranslation("action.export"));
    EXPECT_EQ(mgr.text("action.export"), "Export");

    // "menu.edit" is missing in German -> English text.
    ASSERT_TRUE(mgr.selectLanguage("de").isOk());
    EXPECT_EQ(mgr.text("menu.file"), "Datei");
    EXPECT_EQ(mgr.text("menu.edit"), "Edit");
}

TEST(LocalizationFallback, ReturnsKeyItselfWhenNeitherLanguageDefinesIt) {
    LocalizationManager mgr(LocalizationManager::Config{makeCatalog(), nullptr, {}});
    ASSERT_TRUE(mgr.selectLanguage("fr").isOk());

    // Defined in no catalog at all -> last-resort key echo.
    EXPECT_EQ(mgr.text("does.not.exist"), "does.not.exist");
}

// ---- Requirement 12.5: default selection on first launch ------------------

TEST(LocalizationDefault, UsesSystemLanguageWhenSupported) {
    LocalizationManager::Config config;
    config.translations = makeCatalog();
    config.store = nullptr;  // No prior selection persisted.
    config.systemLanguage = []() -> std::optional<LanguageTag> { return "fr"; };

    LocalizationManager mgr(std::move(config));
    EXPECT_EQ(mgr.currentLanguage(), "fr");
}

TEST(LocalizationDefault, NormalizesSystemLocaleToSupportedTag) {
    // A POSIX locale string with region + encoding resolves to the supported tag.
    LocalizationManager::Config config;
    config.translations = makeCatalog();
    config.systemLanguage = []() -> std::optional<LanguageTag> { return "de_DE.UTF-8"; };

    LocalizationManager mgr(std::move(config));
    EXPECT_EQ(mgr.currentLanguage(), "de");
}

TEST(LocalizationDefault, MapsRegionalPortugueseToSupportedVariant) {
    LocalizationManager::Config config;
    config.translations = makeCatalog();
    config.systemLanguage = []() -> std::optional<LanguageTag> { return "pt_BR"; };

    LocalizationManager mgr(std::move(config));
    EXPECT_EQ(mgr.currentLanguage(), "pt-BR");
}

TEST(LocalizationDefault, FallsBackToEnglishWhenSystemLanguageUnsupported) {
    LocalizationManager::Config config;
    config.translations = makeCatalog();
    config.systemLanguage = []() -> std::optional<LanguageTag> { return "sw_KE"; };  // Swahili

    LocalizationManager mgr(std::move(config));
    EXPECT_EQ(mgr.currentLanguage(), "en");
}

TEST(LocalizationDefault, FallsBackToEnglishWhenSystemLanguageUnknown) {
    LocalizationManager::Config config;
    config.translations = makeCatalog();
    config.systemLanguage = []() -> std::optional<LanguageTag> { return std::nullopt; };

    LocalizationManager mgr(std::move(config));
    EXPECT_EQ(mgr.currentLanguage(), "en");
}

TEST(LocalizationDefault, FallsBackToEnglishWhenNoSystemProvider) {
    LocalizationManager mgr(LocalizationManager::Config{makeCatalog(), nullptr, {}});
    EXPECT_EQ(mgr.currentLanguage(), "en");
}

TEST(LocalizationDefault, AmbiguousChinesePrimaryFallsBackToEnglish) {
    // "zh" alone is ambiguous (zh-Hans vs zh-Hant), so it must not silently pick
    // one; the default is English until the user explicitly selects a variant.
    LocalizationManager::Config config;
    config.translations = makeCatalog();
    config.systemLanguage = []() -> std::optional<LanguageTag> { return "zh"; };

    LocalizationManager mgr(std::move(config));
    EXPECT_EQ(mgr.currentLanguage(), "en");
}

TEST(LocalizationResolve, ExactScriptedChineseResolves) {
    EXPECT_EQ(resolveSupportedLanguage("zh-Hant"), std::optional<LanguageTag>("zh-Hant"));
    EXPECT_EQ(resolveSupportedLanguage("zh_hans"), std::optional<LanguageTag>("zh-Hans"));
    EXPECT_EQ(resolveSupportedLanguage("EN-us"), std::optional<LanguageTag>("en"));
    EXPECT_EQ(resolveSupportedLanguage("xx"), std::nullopt);
}

// ---- Requirement 12.4: persistence and reapply-on-launch ------------------

TEST(LocalizationPersistence, PersistedSelectionOverridesSystemDefault) {
    InMemoryLanguagePreferenceStore store("ja");  // User previously chose Japanese.

    LocalizationManager::Config config;
    config.translations = makeCatalog();
    config.store = &store;
    config.systemLanguage = []() -> std::optional<LanguageTag> { return "fr"; };

    LocalizationManager mgr(std::move(config));
    // Persisted selection wins over the system language (Req 12.4).
    EXPECT_EQ(mgr.currentLanguage(), "ja");
}

TEST(LocalizationPersistence, SelectionIsPersistedThenReappliedOnRelaunch) {
    InMemoryLanguagePreferenceStore store;

    {
        LocalizationManager::Config config;
        config.translations = makeCatalog();
        config.store = &store;
        config.systemLanguage = []() -> std::optional<LanguageTag> { return "en"; };
        LocalizationManager mgr(std::move(config));

        ASSERT_TRUE(mgr.selectLanguage("it").isOk());
    }
    ASSERT_TRUE(store.persisted().has_value());
    EXPECT_EQ(*store.persisted(), "it");

    // Simulate a subsequent launch reading the same store.
    {
        LocalizationManager::Config config;
        config.translations = makeCatalog();
        config.store = &store;
        config.systemLanguage = []() -> std::optional<LanguageTag> { return "en"; };
        LocalizationManager relaunched(std::move(config));
        EXPECT_EQ(relaunched.currentLanguage(), "it");
    }
}

TEST(LocalizationPersistence, UnsupportedPersistedValueFallsBackToDefault) {
    InMemoryLanguagePreferenceStore store("xx");  // No longer / never supported.

    LocalizationManager::Config config;
    config.translations = makeCatalog();
    config.store = &store;
    config.systemLanguage = []() -> std::optional<LanguageTag> { return "fr"; };

    LocalizationManager mgr(std::move(config));
    // Ignore the bad persisted value; fall to the system default (Req 12.5).
    EXPECT_EQ(mgr.currentLanguage(), "fr");
}

// ---- Requirement 12.2: live switching + selection guards ------------------

TEST(LocalizationSelection, RejectsUnsupportedLanguageWithoutChange) {
    LocalizationManager mgr(LocalizationManager::Config{makeCatalog(), nullptr, {}});
    const LanguageTag before = mgr.currentLanguage();

    const auto result = mgr.selectLanguage("xx");
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(mgr.currentLanguage(), before);  // Unchanged.
}

TEST(LocalizationSelection, ObserverNotifiedOnLiveSwitch) {
    LocalizationManager mgr(LocalizationManager::Config{makeCatalog(), nullptr, {}});

    std::vector<LanguageTag> seen;
    auto sub = mgr.observe([&seen](const LanguageTag& lang) { seen.push_back(lang); });

    ASSERT_TRUE(mgr.selectLanguage("fr").isOk());
    ASSERT_TRUE(mgr.selectLanguage("de").isOk());

    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[0], "fr");
    EXPECT_EQ(seen[1], "de");

    // Selecting the current language again is a no-op: no extra notification.
    ASSERT_TRUE(mgr.selectLanguage("de").isOk());
    EXPECT_EQ(seen.size(), 2u);
}

TEST(LocalizationSelection, ObserverNotNotifiedAfterUnsubscribe) {
    LocalizationManager mgr(LocalizationManager::Config{makeCatalog(), nullptr, {}});

    int count = 0;
    {
        auto sub = mgr.observe([&count](const LanguageTag&) { ++count; });
        ASSERT_TRUE(mgr.selectLanguage("fr").isOk());
        EXPECT_EQ(count, 1);
    }  // sub destroyed -> unsubscribed

    ASSERT_TRUE(mgr.selectLanguage("de").isOk());
    EXPECT_EQ(count, 1);  // No further notifications.
}

TEST(LocalizationSelection, TextReflectsCurrentLanguageAfterSwitch) {
    LocalizationManager mgr(LocalizationManager::Config{makeCatalog(), nullptr, {}});
    EXPECT_EQ(mgr.text("menu.file"), "File");  // Default English.

    ASSERT_TRUE(mgr.selectLanguage("fr").isOk());
    EXPECT_EQ(mgr.text("menu.file"), "Fichier");
}

}  // namespace
}  // namespace palmier::services
