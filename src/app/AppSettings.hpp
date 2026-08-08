// SPDX-License-Identifier: GPL-3.0-or-later
//
// app/AppSettings.hpp — resolves the application's runtime configuration
// (task 6.1; Requirements 10.2, 16.3).
//
// Four layers, each one overriding the one below it:
//
//   1. built-in defaults              — the values baked into `AppConfig` and
//                                       `services::RemoteAccessConfig`;
//   2. a `key=value` file             — `$XDG_CONFIG_HOME/palmier-pro/config`
//                                       (or `$HOME/.config/palmier-pro/config`);
//   3. environment variables          — the `PALMIER_*` names in the key table;
//   4. command-line flags             — the `--kebab-case` names in the key table.
//
// The product is an `AppConfig`, ready to hand to `ApplicationComposition`.
//
// Secure by default (Requirements 10.1-10.3). Resolution NEVER enables remote
// access as a side effect: `remote.enabled` stays false and the endpoint stays on
// loopback unless configuration says `true` in so many words. Anything the parser
// cannot make sense of — a malformed line, an unknown key, a value outside its
// documented bounds — is recorded as a diagnostic and *ignored*, leaving the
// lower-precedence (ultimately the default, i.e. the safe) value in place. Even a
// fully populated remote configuration only *offers* a non-loopback bind; the
// decision itself belongs to `services::RemoteAccessGate` (task 6.2).
//
// Two further properties matter:
//
//   * The bearer token is read but never reproduced. No diagnostic, and no
//     accessor other than `config().remote.bearerToken`, contains it or any
//     substring of it (Requirements 10.3, 10.8).
//   * Environment access is by explicit name only. The lookup seam takes a single
//     variable name and the implementation asks for exactly the names in the key
//     table plus `XDG_CONFIG_HOME`, `HOME` and `PALMIER_CONFIG_FILE`. The
//     environment block is never enumerated or copied.

#ifndef PALMIER_APP_APPSETTINGS_HPP
#define PALMIER_APP_APPSETTINGS_HPP

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "app/ApplicationComposition.hpp"  // AppConfig

namespace palmier::app {

/// The resolved configuration plus the story of how it was resolved.
class AppSettings {
public:
    /// Which layer supplied a key's final value, in increasing precedence.
    enum class Layer { BuiltInDefault, ConfigFile, Environment, CommandLine };

    /// Reads ONE named environment variable. Returns nullopt when the variable is
    /// unset or empty. Deliberately name-at-a-time: no implementation of this seam
    /// may enumerate the environment.
    using EnvironmentLookup = std::function<std::optional<std::string>(std::string_view name)>;

    struct Options {
        /// Environment access. When empty, `systemEnvironment()` is used.
        EnvironmentLookup environment;

        /// Explicit config-file path, overriding `--config`, `PALMIER_CONFIG_FILE`
        /// and the XDG derivation. Tests use this (or a scratch `XDG_CONFIG_HOME`)
        /// so that no real user configuration is ever read.
        std::optional<std::filesystem::path> configFile;
    };

    /// An environment lookup backed by `std::getenv` for the requested name only.
    [[nodiscard]] static EnvironmentLookup systemEnvironment();

    /// Resolve all four layers. `commandLine` holds the arguments WITHOUT the
    /// program name (see `fromArgv`). Never throws for bad input: unusable input
    /// becomes a diagnostic.
    [[nodiscard]] static AppSettings load(const std::vector<std::string>& commandLine = {},
                                          Options options = {});

    /// `load()` for a C `main(argc, argv)` pair; skips `argv[0]`.
    [[nodiscard]] static AppSettings fromArgv(int argc, const char* const* argv,
                                              Options options = {});

    /// The resolved configuration.
    [[nodiscard]] const AppConfig& config() const noexcept { return config_; }

    /// Human-readable notes about input that was ignored: malformed config lines,
    /// unknown keys, unknown flags, out-of-range or unparsable values. Never
    /// contains a bearer token or any substring of one. Empty when every input was
    /// understood.
    [[nodiscard]] const std::vector<std::string>& diagnostics() const noexcept {
        return diagnostics_;
    }

    /// True when the command line asked for the option list (`--help` or `-h`).
    /// The entry point prints `usage()` and exits WITHOUT constructing a session;
    /// asking for help is not a diagnostic, so it never appears in
    /// `diagnostics()` and the resolved `config()` is still valid.
    [[nodiscard]] bool helpRequested() const noexcept { return helpRequested_; }

    /// The accepted options, one per line, generated from the same key table that
    /// drives resolution — so the text can never drift from what is actually
    /// understood (Requirement 16.3). Ends with a newline. Names no value, so it is
    /// safe to print anywhere a diagnostic is safe to print.
    [[nodiscard]] static std::string usage();

    /// The config file that was consulted, if any (before knowing whether it
    /// exists), and whether it was actually read.
    [[nodiscard]] const std::optional<std::filesystem::path>& configFilePath() const noexcept {
        return configFilePath_;
    }
    [[nodiscard]] bool configFileRead() const noexcept { return configFileRead_; }

    /// Which layer won for `key` (a key from `recognizedKeys()`).
    /// `Layer::BuiltInDefault` for keys no layer set; nullopt for unknown keys.
    [[nodiscard]] std::optional<Layer> sourceOf(std::string_view key) const;

    /// Arguments that were not options (e.g. a project path). Not interpreted here.
    [[nodiscard]] const std::vector<std::string>& positionalArguments() const noexcept {
        return positional_;
    }

    /// Every key the config file understands, in declaration order.
    [[nodiscard]] static std::vector<std::string_view> recognizedKeys();

    /// The environment variable / command-line flag bound to `key`, or empty when
    /// `key` is not recognized. Documented so Requirement 16.3's operator
    /// documentation and the settings table cannot drift apart.
    [[nodiscard]] static std::string_view environmentVariableFor(std::string_view key);
    [[nodiscard]] static std::string_view commandLineFlagFor(std::string_view key);

private:
    AppSettings() = default;

    AppConfig                            config_{};
    std::vector<std::string>             diagnostics_{};
    std::vector<std::string>             positional_{};
    std::map<std::string, Layer>         sources_{};
    std::optional<std::filesystem::path> configFilePath_{};
    bool                                 configFileRead_ = false;
    bool                                 helpRequested_ = false;
};

}  // namespace palmier::app

#endif  // PALMIER_APP_APPSETTINGS_HPP
