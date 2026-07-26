// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/app/app_settings_test.cpp — unit tests for app::AppSettings (task 6.1;
// Requirements 10.2, 16.3).
//
// What is pinned here:
//
//   * the precedence chain, one layer at a time: built-in defaults < the
//     key=value file < environment variables < command-line flags, each layer
//     demonstrably overriding the one below it;
//   * an absent config file resolves to the defaults with no diagnostic;
//   * a malformed line, an unknown key and an unknown flag are reported and
//     ignored, and never stop the surrounding valid input from being applied;
//   * out-of-range values fall back to the layer below rather than being clamped
//     into something the operator did not ask for;
//   * remote access stays OFF, on loopback, unless configuration says `true` in
//     so many words (Requirements 10.1-10.3);
//   * the bearer token never appears in a diagnostic (Requirements 10.3, 10.8).
//
// Isolation. Every test drives the environment through the injected lookup seam,
// so the process environment is never mutated and `getenv` is never consulted.
// Where a config file is needed, XDG_CONFIG_HOME is pointed at a scratch
// directory whose name carries the pid (ctest runs one process per test case, in
// parallel), so no real user configuration directory is read or written.

#include <gtest/gtest.h>

#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "app/AppSettings.hpp"

namespace {

using palmier::app::AppSettings;
using Layer = AppSettings::Layer;

// --- Scratch XDG_CONFIG_HOME ------------------------------------------------

/// A unique-per-process, unique-per-test scratch directory that stands in for
/// $XDG_CONFIG_HOME, removed on destruction.
class ScratchConfigHome {
public:
    explicit ScratchConfigHome(std::string_view label) {
        root_ = std::filesystem::temp_directory_path() /
                ("palmier_app_settings_" + std::to_string(static_cast<long>(::getpid())) + "_" +
                 std::string(label));
        std::filesystem::remove_all(root_);
        std::filesystem::create_directories(root_);
    }

    ~ScratchConfigHome() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    ScratchConfigHome(const ScratchConfigHome&) = delete;
    ScratchConfigHome& operator=(const ScratchConfigHome&) = delete;

    [[nodiscard]] const std::filesystem::path& root() const { return root_; }

    /// Write `contents` to $XDG_CONFIG_HOME/palmier-pro/config.
    void writeConfig(std::string_view contents) const {
        const std::filesystem::path directory = root_ / "palmier-pro";
        std::filesystem::create_directories(directory);
        std::ofstream output(directory / "config", std::ios::binary | std::ios::trunc);
        output << contents;
    }

    [[nodiscard]] std::filesystem::path configPath() const {
        return root_ / "palmier-pro" / "config";
    }

private:
    std::filesystem::path root_;
};

/// An environment that answers only the names it was given, one at a time. A test
/// double for the seam, and proof that resolution needs nothing but named reads.
AppSettings::EnvironmentLookup fakeEnvironment(std::map<std::string, std::string> variables) {
    return [variables = std::move(variables)](
               std::string_view name) -> std::optional<std::string> {
        const auto found = variables.find(std::string(name));
        if (found == variables.end() || found->second.empty()) {
            return std::nullopt;
        }
        return found->second;
    };
}

/// Options with an environment that knows nothing at all.
AppSettings::Options emptyEnvironment() {
    AppSettings::Options options;
    options.environment = fakeEnvironment({});
    return options;
}

/// Options whose XDG_CONFIG_HOME is the scratch directory.
AppSettings::Options withScratch(const ScratchConfigHome& scratch,
                                 std::map<std::string, std::string> extra = {}) {
    extra["XDG_CONFIG_HOME"] = scratch.root().string();
    AppSettings::Options options;
    options.environment = fakeEnvironment(std::move(extra));
    return options;
}

[[nodiscard]] bool anyDiagnosticContains(const AppSettings& settings, std::string_view needle) {
    for (const std::string& diagnostic : settings.diagnostics()) {
        if (diagnostic.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Layer 1 — built-in defaults.
// ---------------------------------------------------------------------------

TEST(AppSettings, BuiltInDefaultsAreLoopbackOnly) {
    const AppSettings settings = AppSettings::load({}, emptyEnvironment());

    EXPECT_EQ(settings.config().mcpHost, "127.0.0.1");
    EXPECT_EQ(settings.config().mcpPort, 19789);

    // Requirement 10.1: nothing configured => remote access off, loopback only.
    EXPECT_FALSE(settings.config().remote.enabled);
    EXPECT_TRUE(settings.config().remote.bindAddress.empty());
    EXPECT_TRUE(settings.config().remote.bearerToken.empty());
    EXPECT_FALSE(settings.config().remote.acknowledged);
    EXPECT_TRUE(settings.config().remote.originAllowList.empty());
    EXPECT_FALSE(settings.config().remote.tlsCertificate.has_value());
    EXPECT_FALSE(settings.config().remote.tlsPrivateKey.has_value());
    EXPECT_EQ(settings.config().remote.port, 19789);
    EXPECT_EQ(settings.config().remote.maxSessions, 8);
    EXPECT_EQ(settings.config().remote.idleTimeout, std::chrono::seconds{300});

    // No layer above the defaults spoke, and nothing was ignored.
    EXPECT_EQ(settings.sourceOf("remote.enabled"), Layer::BuiltInDefault);
    EXPECT_TRUE(settings.diagnostics().empty());
    EXPECT_FALSE(settings.configFileRead());

    // The main-thread invoker is a seam supplied by the composition, not config.
    EXPECT_FALSE(static_cast<bool>(settings.config().mainThreadInvoker));
}

TEST(AppSettings, AbsentConfigFileLeavesTheDefaultsStanding) {
    const ScratchConfigHome scratch("absent");  // directory exists, config file does not

    const AppSettings settings = AppSettings::load({}, withScratch(scratch));

    ASSERT_TRUE(settings.configFilePath().has_value());
    EXPECT_EQ(*settings.configFilePath(), scratch.configPath());
    EXPECT_FALSE(settings.configFileRead());
    EXPECT_TRUE(settings.diagnostics().empty());
    EXPECT_EQ(settings.config().mcpPort, 19789);
    EXPECT_FALSE(settings.config().remote.enabled);
}

TEST(AppSettings, ConfigPathFallsBackToHomeDotConfigAndIsAbsentWithNeitherVariable) {
    AppSettings::Options withHome;
    withHome.environment = fakeEnvironment({{"HOME", "/home/somebody"}});
    const AppSettings fromHome = AppSettings::load({}, withHome);
    ASSERT_TRUE(fromHome.configFilePath().has_value());
    EXPECT_EQ(*fromHome.configFilePath(),
              std::filesystem::path("/home/somebody/.config/palmier-pro/config"));

    const AppSettings noHome = AppSettings::load({}, emptyEnvironment());
    EXPECT_FALSE(noHome.configFilePath().has_value());
    EXPECT_TRUE(noHome.diagnostics().empty());
}

// ---------------------------------------------------------------------------
// Layer 2 — the config file overrides the defaults.
// ---------------------------------------------------------------------------

TEST(AppSettings, ConfigFileOverridesBuiltInDefaults) {
    const ScratchConfigHome scratch("file_layer");
    scratch.writeConfig(
        "# Palmier Pro configuration\n"
        "; both comment styles are skipped\n"
        "\n"
        "mcp.host = 127.0.0.1\n"
        "mcp.port = 21000\n"
        "remote.enabled = true\n"
        "remote.bind_address = 192.168.1.20\n"
        "remote.port = 20001\n"
        "remote.acknowledged = yes\n"
        "remote.origin_allow_list = https://a.example , https://b.example\n"
        "remote.max_sessions = 12\n"
        "remote.idle_timeout_seconds = 90\n"
        "remote.tls_certificate = /etc/palmier/cert.pem\n"
        "remote.tls_private_key = /etc/palmier/key.pem\n");

    const AppSettings settings = AppSettings::load({}, withScratch(scratch));

    EXPECT_TRUE(settings.diagnostics().empty());
    EXPECT_TRUE(settings.configFileRead());
    EXPECT_EQ(settings.config().mcpPort, 21000);
    EXPECT_TRUE(settings.config().remote.enabled);
    EXPECT_EQ(settings.config().remote.bindAddress, "192.168.1.20");
    EXPECT_EQ(settings.config().remote.port, 20001);
    EXPECT_TRUE(settings.config().remote.acknowledged);
    EXPECT_EQ(settings.config().remote.originAllowList,
              (std::vector<std::string>{"https://a.example", "https://b.example"}));
    EXPECT_EQ(settings.config().remote.maxSessions, 12);
    EXPECT_EQ(settings.config().remote.idleTimeout, std::chrono::seconds{90});
    ASSERT_TRUE(settings.config().remote.tlsCertificate.has_value());
    EXPECT_EQ(*settings.config().remote.tlsCertificate,
              std::filesystem::path("/etc/palmier/cert.pem"));
    ASSERT_TRUE(settings.config().remote.tlsPrivateKey.has_value());
    EXPECT_EQ(*settings.config().remote.tlsPrivateKey,
              std::filesystem::path("/etc/palmier/key.pem"));

    EXPECT_EQ(settings.sourceOf("remote.max_sessions"), Layer::ConfigFile);
    EXPECT_EQ(settings.sourceOf("remote.bearer_token"), Layer::BuiltInDefault);
    EXPECT_EQ(settings.sourceOf("no.such.key"), std::nullopt);
}

// ---------------------------------------------------------------------------
// Layer 3 — the environment overrides the file (and the defaults).
// ---------------------------------------------------------------------------

TEST(AppSettings, EnvironmentOverridesConfigFile) {
    const ScratchConfigHome scratch("env_layer");
    scratch.writeConfig(
        "mcp.port = 21000\n"
        "remote.enabled = false\n"
        "remote.max_sessions = 12\n"
        "remote.idle_timeout_seconds = 90\n");

    const AppSettings settings =
        AppSettings::load({}, withScratch(scratch, {{"PALMIER_MCP_PORT", "22000"},
                                                    {"PALMIER_REMOTE_ENABLED", "true"},
                                                    {"PALMIER_REMOTE_MAX_SESSIONS", "5"}}));

    EXPECT_TRUE(settings.diagnostics().empty());
    EXPECT_EQ(settings.config().mcpPort, 22000);                  // env beat the file
    EXPECT_TRUE(settings.config().remote.enabled);                // env beat the file
    EXPECT_EQ(settings.config().remote.maxSessions, 5);           // env beat the file
    EXPECT_EQ(settings.config().remote.idleTimeout,
              std::chrono::seconds{90});                          // file beat the default

    EXPECT_EQ(settings.sourceOf("mcp.port"), Layer::Environment);
    EXPECT_EQ(settings.sourceOf("remote.idle_timeout_seconds"), Layer::ConfigFile);
}

// ---------------------------------------------------------------------------
// Layer 4 — command-line flags override everything.
// ---------------------------------------------------------------------------

TEST(AppSettings, CommandLineOverridesEnvironmentAndFile) {
    const ScratchConfigHome scratch("cli_layer");
    scratch.writeConfig(
        "mcp.port = 21000\n"
        "remote.max_sessions = 12\n"
        "remote.bind_address = 10.0.0.1\n");

    const std::vector<std::string> arguments{"--mcp-port=23000", "--remote-max-sessions", "3",
                                             "--remote-enabled", "project.palmier"};
    const AppSettings settings =
        AppSettings::load(arguments, withScratch(scratch, {{"PALMIER_MCP_PORT", "22000"},
                                                           {"PALMIER_REMOTE_MAX_SESSIONS", "7"}}));

    EXPECT_TRUE(settings.diagnostics().empty());
    EXPECT_EQ(settings.config().mcpPort, 23000);         // --flag=value beat env and file
    EXPECT_EQ(settings.config().remote.maxSessions, 3);  // --flag value beat env and file
    EXPECT_TRUE(settings.config().remote.enabled);       // bare boolean flag means true
    EXPECT_EQ(settings.config().remote.bindAddress, "10.0.0.1");  // untouched file value

    EXPECT_EQ(settings.sourceOf("mcp.port"), Layer::CommandLine);
    EXPECT_EQ(settings.sourceOf("remote.bind_address"), Layer::ConfigFile);
    EXPECT_EQ(settings.positionalArguments(), (std::vector<std::string>{"project.palmier"}));
}

TEST(AppSettings, ConfigFlagSelectsTheFileAndFromArgvSkipsTheProgramName) {
    const ScratchConfigHome scratch("config_flag");
    const std::filesystem::path alternate = scratch.root() / "alternate.conf";
    {
        std::ofstream output(alternate);
        output << "mcp.port = 24000\n";
    }
    const std::string flag = "--config=" + alternate.string();
    const char* argv[] = {"palmier-pro", flag.c_str(), "--remote-max-sessions=9"};

    AppSettings::Options options;
    options.environment = fakeEnvironment({});  // no XDG_CONFIG_HOME at all
    const AppSettings settings = AppSettings::fromArgv(3, argv, options);

    EXPECT_TRUE(settings.configFileRead());
    ASSERT_TRUE(settings.configFilePath().has_value());
    EXPECT_EQ(*settings.configFilePath(), alternate);
    EXPECT_EQ(settings.config().mcpPort, 24000);
    EXPECT_EQ(settings.config().remote.maxSessions, 9);
    EXPECT_TRUE(settings.positionalArguments().empty());  // argv[0] was skipped
    EXPECT_TRUE(settings.diagnostics().empty());
}

// ---------------------------------------------------------------------------
// Bad input is reported and ignored.
// ---------------------------------------------------------------------------

TEST(AppSettings, MalformedLineIsReportedAndTheRestOfTheFileStillApplies) {
    const ScratchConfigHome scratch("malformed");
    scratch.writeConfig(
        "mcp.port = 21000\n"
        "this line has no equals sign\n"
        "= 12\n"
        "remote.max_sessions = 4\n");

    const AppSettings settings = AppSettings::load({}, withScratch(scratch));

    // Both bad lines were reported, by line number...
    ASSERT_EQ(settings.diagnostics().size(), 2U);
    EXPECT_TRUE(anyDiagnosticContains(settings, ":2: malformed line"));
    EXPECT_TRUE(anyDiagnosticContains(settings, ":3: malformed line"));
    // ...the offending text was withheld...
    EXPECT_FALSE(anyDiagnosticContains(settings, "this line has no equals sign"));
    // ...and the valid lines on either side of them still took effect.
    EXPECT_EQ(settings.config().mcpPort, 21000);
    EXPECT_EQ(settings.config().remote.maxSessions, 4);
}

TEST(AppSettings, UnknownKeysAndUnknownFlagsAreReportedAndIgnored) {
    const ScratchConfigHome scratch("unknown");
    scratch.writeConfig(
        "remote.enabled = true\n"
        "remote.super_powers = on\n"
        "mcp.hostname = 10.0.0.5\n");

    const AppSettings settings =
        AppSettings::load({"--turbo-mode", "--mcp-hostname=10.0.0.6"}, withScratch(scratch));

    EXPECT_TRUE(anyDiagnosticContains(settings, "unknown key 'remote.super_powers'"));
    EXPECT_TRUE(anyDiagnosticContains(settings, "unknown key 'mcp.hostname'"));
    EXPECT_TRUE(anyDiagnosticContains(settings, "unknown option '--turbo-mode'"));
    EXPECT_TRUE(anyDiagnosticContains(settings, "unknown option '--mcp-hostname'"));
    EXPECT_EQ(settings.diagnostics().size(), 4U);

    // Unknown input changes nothing; the recognized key still applied.
    EXPECT_TRUE(settings.config().remote.enabled);
    EXPECT_EQ(settings.config().mcpHost, "127.0.0.1");
}

TEST(AppSettings, OutOfRangeAndUnparsableValuesFallBackToTheLayerBelow) {
    const ScratchConfigHome scratch("ranges");
    scratch.writeConfig(
        "remote.max_sessions = 12\n"
        "remote.idle_timeout_seconds = 10\n"   // below the 30 s floor (10.11)
        "remote.port = 0\n"                    // below the 1..65535 range
        "mcp.port = eighty\n");                // not a number at all

    const AppSettings settings = AppSettings::load(
        {"--remote-max-sessions=99"},          // above the 32 ceiling (10.9)
        withScratch(scratch));

    EXPECT_EQ(settings.diagnostics().size(), 4U);
    EXPECT_TRUE(anyDiagnosticContains(settings, "remote.idle_timeout_seconds: value out of range 30..3600"));
    EXPECT_TRUE(anyDiagnosticContains(settings, "remote.port: value out of range 1..65535"));
    EXPECT_TRUE(anyDiagnosticContains(settings, "mcp.port: expected an integer"));
    EXPECT_TRUE(anyDiagnosticContains(settings, "remote.max_sessions: value out of range 1..32"));

    // Rejected values are not clamped into something nobody asked for: the layer
    // below survives.
    EXPECT_EQ(settings.config().remote.maxSessions, 12);  // the file's value stands
    EXPECT_EQ(settings.config().remote.idleTimeout, std::chrono::seconds{300});
    EXPECT_EQ(settings.config().remote.port, 19789);
    EXPECT_EQ(settings.config().mcpPort, 19789);
}

// ---------------------------------------------------------------------------
// Secure by default (Requirements 10.1-10.3, 10.8).
// ---------------------------------------------------------------------------

TEST(AppSettings, RemoteAccessStaysOffUnlessExplicitlyEnabled) {
    const ScratchConfigHome scratch("secure_default");
    // Everything a non-loopback bind would need EXCEPT the opt-in switch.
    scratch.writeConfig(
        "remote.bind_address = 203.0.113.7\n"
        "remote.bearer_token = 0123456789abcdef0123456789abcdef\n"
        "remote.acknowledged = true\n"
        "remote.origin_allow_list = https://studio.example\n");

    const AppSettings settings = AppSettings::load({}, withScratch(scratch));

    EXPECT_TRUE(settings.diagnostics().empty());
    EXPECT_FALSE(settings.config().remote.enabled);
    EXPECT_EQ(settings.sourceOf("remote.enabled"), Layer::BuiltInDefault);
}

TEST(AppSettings, NonBooleanOrNegativeOptInDoesNotEnableRemoteAccess) {
    const ScratchConfigHome scratch("no_optin");
    scratch.writeConfig("remote.enabled = maybe\n");  // unparsable

    const AppSettings garbled = AppSettings::load({}, withScratch(scratch));
    EXPECT_FALSE(garbled.config().remote.enabled);
    EXPECT_TRUE(anyDiagnosticContains(garbled, "remote.enabled: expected a boolean"));

    // An explicit `false` at a higher-precedence layer turns it back off, too.
    scratch.writeConfig("remote.enabled = true\n");
    const AppSettings turnedOff = AppSettings::load(
        {"--remote-enabled=false"}, withScratch(scratch, {{"PALMIER_REMOTE_ENABLED", "true"}}));
    EXPECT_FALSE(turnedOff.config().remote.enabled);
    EXPECT_EQ(turnedOff.sourceOf("remote.enabled"), Layer::CommandLine);
}

TEST(AppSettings, BearerTokenIsCarriedVerbatimAndNeverAppearsInADiagnostic) {
    const ScratchConfigHome scratch("token");
    const std::string token = "s3cret-token-with-32-plus-characters";
    scratch.writeConfig("remote.bearer_token = " + token +
                        "\n"
                        "a malformed line right after the secret\n"
                        "remote.max_sessions = 77\n");  // out of range, forces a diagnostic

    const AppSettings settings = AppSettings::load({}, withScratch(scratch));

    // Carried through exactly, so the gate's byte-for-byte comparison (10.4) sees
    // the operator's value.
    EXPECT_EQ(settings.config().remote.bearerToken, token);
    EXPECT_FALSE(settings.diagnostics().empty());
    for (const std::string& diagnostic : settings.diagnostics()) {
        EXPECT_EQ(diagnostic.find(token), std::string::npos) << diagnostic;
        EXPECT_EQ(diagnostic.find("s3cret"), std::string::npos) << diagnostic;
    }
}

// ---------------------------------------------------------------------------
// The documented key table (Requirement 16.3).
// ---------------------------------------------------------------------------

TEST(AppSettings, EveryRecognizedKeyHasAnEnvironmentVariableAndAFlag) {
    const std::vector<std::string_view> keys = AppSettings::recognizedKeys();
    EXPECT_FALSE(keys.empty());
    for (const std::string_view key : keys) {
        EXPECT_FALSE(AppSettings::environmentVariableFor(key).empty()) << key;
        EXPECT_FALSE(AppSettings::commandLineFlagFor(key).empty()) << key;
        EXPECT_EQ(AppSettings::environmentVariableFor(key).substr(0, 8), "PALMIER_") << key;
        EXPECT_EQ(AppSettings::commandLineFlagFor(key).substr(0, 2), "--") << key;
    }
    EXPECT_TRUE(AppSettings::environmentVariableFor("no.such.key").empty());
    EXPECT_TRUE(AppSettings::commandLineFlagFor("no.such.key").empty());
}

}  // namespace
