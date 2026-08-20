// SPDX-License-Identifier: GPL-3.0-or-later
//
// app/AppSettings.cpp — the four-layer configuration resolver (task 6.1;
// Requirements 10.2, 16.3). See AppSettings.hpp for the layering contract and the
// two security properties this file has to keep: the bearer token never reaches a
// diagnostic, and the environment is read one explicitly named variable at a time.

#include "app/AppSettings.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

namespace palmier::app {
namespace {

// ---------------------------------------------------------------------------
// Small text helpers.
// ---------------------------------------------------------------------------

[[nodiscard]] std::string_view trim(std::string_view text) {
    const auto isSpace = [](char c) {
        return std::isspace(static_cast<unsigned char>(c)) != 0;
    };
    while (!text.empty() && isSpace(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && isSpace(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

[[nodiscard]] std::string toLower(std::string_view text) {
    std::string lowered(text);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    return lowered;
}

// ---------------------------------------------------------------------------
// Value parsers. Each reports failure through `error` and leaves the target
// untouched, so the lower-precedence (ultimately the default) value survives.
// No parser ever copies the offending value into `error`: for the bearer token
// that is a hard requirement (10.3, 10.8), and applying it uniformly means no
// future key can leak by accident.
// ---------------------------------------------------------------------------

[[nodiscard]] bool parseBool(std::string_view value, bool& out, std::string& error) {
    const std::string lowered = toLower(trim(value));
    if (lowered == "true" || lowered == "1" || lowered == "yes" || lowered == "on") {
        out = true;
        return true;
    }
    if (lowered == "false" || lowered == "0" || lowered == "no" || lowered == "off") {
        out = false;
        return true;
    }
    error = "expected a boolean (true/false, 1/0, yes/no, on/off)";
    return false;
}

/// Would `parseBool` accept this text? Used to decide whether a bare boolean flag
/// should swallow the argument that follows it: `--remote-enabled false` means
/// what it says, while `--remote-enabled project.palmier` means the flag plus a
/// positional argument, not a flag with a nonsense value.
[[nodiscard]] bool looksBoolean(std::string_view value) {
    bool ignoredValue = false;
    std::string ignoredError;
    return parseBool(value, ignoredValue, ignoredError);
}

[[nodiscard]] bool parseInteger(std::string_view value, long long minimum, long long maximum,
                                long long& out, std::string& error) {
    const std::string text(trim(value));
    if (text.empty()) {
        error = "expected an integer";
        return false;
    }
    std::size_t consumed = 0;
    long long parsed = 0;
    try {
        parsed = std::stoll(text, &consumed);
    } catch (const std::exception&) {
        error = "expected an integer";
        return false;
    }
    if (consumed != text.size()) {
        error = "expected an integer";
        return false;
    }
    if (parsed < minimum || parsed > maximum) {
        std::ostringstream message;
        message << "value out of range " << minimum << ".." << maximum;
        error = message.str();
        return false;
    }
    out = parsed;
    return true;
}

/// Comma-separated list; blank entries are dropped, entries are trimmed. An empty
/// string clears the list (which, per Requirement 10.5, means "bound address and
/// loopback only" — the safe reading).
[[nodiscard]] std::vector<std::string> parseList(std::string_view value) {
    std::vector<std::string> entries;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t comma = value.find(',', start);
        const std::string_view piece =
            trim(value.substr(start, comma == std::string_view::npos ? std::string_view::npos
                                                                    : comma - start));
        if (!piece.empty()) {
            entries.emplace_back(piece);
        }
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
    return entries;
}

// ---------------------------------------------------------------------------
// The key table — the single source of truth for what is configurable, and for
// the config-file key / environment variable / command-line flag that name it.
// Requirement 16.3's operator documentation is written against this table.
// ---------------------------------------------------------------------------

using Applier = bool (*)(std::string_view value, AppConfig& config, std::string& error);

struct KeySpec {
    std::string_view key;
    std::string_view envVar;
    std::string_view flag;
    bool             valueOptional;  // bare `--flag` means true
    Applier          apply;
};

constexpr KeySpec kKeys[] = {
    {"mcp.host", "PALMIER_MCP_HOST", "--mcp-host", false,
     [](std::string_view value, AppConfig& config, std::string& error) {
         const std::string_view host = trim(value);
         if (host.empty()) {
             error = "expected a host address";
             return false;
         }
         config.mcpHost = std::string(host);
         return true;
     }},
    {"mcp.port", "PALMIER_MCP_PORT", "--mcp-port", false,
     [](std::string_view value, AppConfig& config, std::string& error) {
         long long port = 0;
         if (!parseInteger(value, 0, 65535, port, error)) {
             return false;
         }
         config.mcpPort = static_cast<std::uint16_t>(port);
         return true;
     }},
    {"remote.enabled", "PALMIER_REMOTE_ENABLED", "--remote-enabled", true,
     [](std::string_view value, AppConfig& config, std::string& error) {
         return parseBool(value, config.remote.enabled, error);
     }},
    {"remote.bind_address", "PALMIER_REMOTE_BIND_ADDRESS", "--remote-bind-address", false,
     [](std::string_view value, AppConfig& config, std::string& error) {
         const std::string_view address = trim(value);
         if (address.empty()) {
             error = "expected an IPv4 or IPv6 literal";
             return false;
         }
         // Syntactic validation of the literal is the gate's bind-time job
         // (Requirement 10.2); the settings layer only carries the string.
         config.remote.bindAddress = std::string(address);
         return true;
     }},
    {"remote.port", "PALMIER_REMOTE_PORT", "--remote-port", false,
     [](std::string_view value, AppConfig& config, std::string& error) {
         long long port = 0;
         if (!parseInteger(value, 1, 65535, port, error)) {
             return false;
         }
         config.remote.port = static_cast<std::uint16_t>(port);
         return true;
     }},
    {"remote.bearer_token", "PALMIER_REMOTE_BEARER_TOKEN", "--remote-bearer-token", false,
     [](std::string_view value, AppConfig& config, std::string&) {
         // Carried verbatim, since Requirement 10.4 compares byte-for-byte.
         // (Config-file values arrive already trimmed; an environment variable or
         // a flag is taken exactly as given.) Length and character-class checks
         // belong to the gate, and neither this function nor its caller ever
         // reproduces the value.
         config.remote.bearerToken = std::string(value);
         return true;
     }},
    {"remote.acknowledged", "PALMIER_REMOTE_ACKNOWLEDGED", "--remote-acknowledged", true,
     [](std::string_view value, AppConfig& config, std::string& error) {
         return parseBool(value, config.remote.acknowledged, error);
     }},
    {"remote.origin_allow_list", "PALMIER_REMOTE_ORIGIN_ALLOW_LIST", "--remote-origin-allow-list",
     false,
     [](std::string_view value, AppConfig& config, std::string&) {
         config.remote.originAllowList = parseList(value);
         return true;
     }},
    {"remote.tls_certificate", "PALMIER_REMOTE_TLS_CERTIFICATE", "--remote-tls-certificate", false,
     [](std::string_view value, AppConfig& config, std::string& error) {
         const std::string_view path = trim(value);
         if (path.empty()) {
             error = "expected a file path";
             return false;
         }
         config.remote.tlsCertificate = std::filesystem::path(std::string(path));
         return true;
     }},
    {"remote.tls_private_key", "PALMIER_REMOTE_TLS_PRIVATE_KEY", "--remote-tls-private-key", false,
     [](std::string_view value, AppConfig& config, std::string& error) {
         const std::string_view path = trim(value);
         if (path.empty()) {
             error = "expected a file path";
             return false;
         }
         config.remote.tlsPrivateKey = std::filesystem::path(std::string(path));
         return true;
     }},
    {"remote.max_sessions", "PALMIER_REMOTE_MAX_SESSIONS", "--remote-max-sessions", false,
     [](std::string_view value, AppConfig& config, std::string& error) {
         long long sessions = 0;
         if (!parseInteger(value, 1, 32, sessions, error)) {  // Requirement 10.9
             return false;
         }
         config.remote.maxSessions = static_cast<int>(sessions);
         return true;
     }},
    {"remote.idle_timeout_seconds", "PALMIER_REMOTE_IDLE_TIMEOUT_SECONDS",
     "--remote-idle-timeout-seconds", false,
     [](std::string_view value, AppConfig& config, std::string& error) {
         long long seconds = 0;
         if (!parseInteger(value, 30, 3600, seconds, error)) {  // Requirement 10.11
             return false;
         }
         config.remote.idleTimeout = std::chrono::seconds{seconds};
         return true;
     }},
    // Task 10.1 / Requirement 11.1: which Agent_Interpreter to install
    // (`offline`, `hosted` or `byok`).
    //
    // Only emptiness is checked here. Deciding whether the id names a real
    // interpreter is `services::AgentInterpreterRegistry`'s job — the same split
    // `remote.bind_address` above uses, and for the same reason: the settings layer
    // carries the operator's string, and the component that owns the value set
    // validates it. That split is load-bearing here, because the registry's answer
    // to an unrecognised id is not a rejection but a documented FALLBACK to
    // `offline` plus a startup error naming the rejected id (Requirement 11.8);
    // rejecting the value at this layer would silently substitute the default and
    // lose that diagnostic.
    {"agent.interpreter", "PALMIER_AGENT_INTERPRETER", "--agent-interpreter", false,
     [](std::string_view value, AppConfig& config, std::string& error) {
         const std::string_view id = trim(value);
         if (id.empty()) {
             error = "expected an agent interpreter id ('offline', 'hosted' or 'byok')";
             return false;
         }
         config.agentInterpreterId = std::string(id);
         return true;
     }},
    // Task 10.5 / Requirement 12.2: which Generative_Backend to install
    // (`offline`, `hosted` or `byok`). All three are compiled in, so this string is
    // the only thing that selects between them — without it the hosted and BYOK
    // clients are unreachable from a shipped binary.
    //
    // Validation is split exactly as it is for `agent.interpreter` above, and for
    // the same reason: `services::GenerativeBackendRegistry` answers an
    // unrecognised id with a documented fallback to `offline` plus a startup error
    // naming the rejected id (Requirement 12.8), which rejecting the value here
    // would silently throw away.
    {"generative.backend", "PALMIER_GENERATIVE_BACKEND", "--generative-backend", false,
     [](std::string_view value, AppConfig& config, std::string& error) {
         const std::string_view id = trim(value);
         if (id.empty()) {
             error = "expected a generative backend id ('offline', 'hosted' or 'byok')";
             return false;
         }
         config.generativeBackendId = std::string(id);
         return true;
     }},
    // usable-editor spec Phase 2, task 6: the base URL the selected `hosted`/
    // `byok` HTTPS client sends its requests to. A location, never a credential
    // (Requirement 12.6 applies here exactly as it does to `generative.backend`
    // above): the credential VALUE is still read from the SecretStore at request
    // time and never touches this setting. Without this key set, `generative
    // BackendRequest::endpoint.baseUrl` stays empty and the selected client
    // reports "no generative endpoint is configured" per request — a working
    // transport (task 6.2) with nowhere configured to send to is exactly as
    // unreachable as no transport at all, which is why this key exists alongside
    // it rather than being deferred.
    {"generative.endpoint", "PALMIER_GENERATIVE_ENDPOINT", "--generative-endpoint", false,
     [](std::string_view value, AppConfig& config, std::string& error) {
         const std::string_view url = trim(value);
         if (url.empty()) {
             error = "expected an https:// base URL";
             return false;
         }
         constexpr std::string_view kScheme = "https://";
         if (url.size() <= kScheme.size() || url.substr(0, kScheme.size()) != kScheme) {
             error = "expected an https:// URL; a plaintext endpoint is refused before any "
                     "request is built";
             return false;
         }
         config.generativeEndpoint.baseUrl = std::string(url);
         return true;
     }},
};

[[nodiscard]] const KeySpec* findKey(std::string_view key) {
    for (const KeySpec& spec : kKeys) {
        if (spec.key == key) {
            return &spec;
        }
    }
    return nullptr;
}

[[nodiscard]] const KeySpec* findFlag(std::string_view flag) {
    for (const KeySpec& spec : kKeys) {
        if (spec.flag == flag) {
            return &spec;
        }
    }
    return nullptr;
}

// The variables read by name outside the key table. Listed here so the complete
// set of environment reads is auditable in one place.
constexpr std::string_view kEnvConfigFile = "PALMIER_CONFIG_FILE";
constexpr std::string_view kEnvXdgConfigHome = "XDG_CONFIG_HOME";
constexpr std::string_view kEnvHome = "HOME";

constexpr std::string_view kConfigFlag = "--config";
constexpr std::string_view kConfigDirName = "palmier-pro";
constexpr std::string_view kConfigFileName = "config";

// Asking for the option list. Handled before layer 4 so it is neither an unknown
// option nor a positional argument.
constexpr std::string_view kHelpFlagLong = "--help";
constexpr std::string_view kHelpFlagShort = "-h";

}  // namespace

// ---------------------------------------------------------------------------
// Environment seam.
// ---------------------------------------------------------------------------

AppSettings::EnvironmentLookup AppSettings::systemEnvironment() {
    return [](std::string_view name) -> std::optional<std::string> {
        // One named variable per call. The environment block is never walked.
        const std::string key(name);
        const char* value = std::getenv(key.c_str());
        if (value == nullptr || value[0] == '\0') {
            return std::nullopt;
        }
        return std::string(value);
    };
}

std::vector<std::string_view> AppSettings::recognizedKeys() {
    std::vector<std::string_view> keys;
    keys.reserve(std::size(kKeys));
    for (const KeySpec& spec : kKeys) {
        keys.push_back(spec.key);
    }
    return keys;
}

std::string AppSettings::usage() {
    std::ostringstream text;
    text << "Usage: palmier-pro [options] [project.palmier]\n"
         << "\nOptions:\n"
         << "  " << kHelpFlagLong << ", " << kHelpFlagShort
         << "                 show this option list and exit\n"
         << "  " << kConfigFlag << " <path>              read settings from <path> instead of\n"
         << "                              $XDG_CONFIG_HOME/" << kConfigDirName << "/"
         << kConfigFileName << "\n";
    for (const KeySpec& spec : kKeys) {
        text << "  " << spec.flag << (spec.valueOptional ? " [<value>]" : " <value>") << '\n'
             << "                              config key " << spec.key << ", environment "
             << spec.envVar << '\n';
    }
    text << "\nPrecedence, lowest first: built-in defaults, the config file, the\n"
            "environment, the command line. An option that cannot be understood is\n"
            "reported and ignored, leaving the safe default in place.\n";
    return text.str();
}

std::string_view AppSettings::environmentVariableFor(std::string_view key) {
    const KeySpec* spec = findKey(key);
    return spec != nullptr ? spec->envVar : std::string_view{};
}

std::string_view AppSettings::commandLineFlagFor(std::string_view key) {
    const KeySpec* spec = findKey(key);
    return spec != nullptr ? spec->flag : std::string_view{};
}

std::optional<AppSettings::Layer> AppSettings::sourceOf(std::string_view key) const {
    if (findKey(key) == nullptr) {
        return std::nullopt;
    }
    const auto found = sources_.find(std::string(key));
    return found != sources_.end() ? found->second : Layer::BuiltInDefault;
}

AppSettings AppSettings::fromArgv(int argc, const char* const* argv, Options options) {
    std::vector<std::string> arguments;
    for (int index = 1; index < argc; ++index) {  // argv[0] is the program name
        if (argv[index] != nullptr) {
            arguments.emplace_back(argv[index]);
        }
    }
    return load(arguments, std::move(options));
}

// ---------------------------------------------------------------------------
// load() — defaults, then the file, then the environment, then the flags.
// ---------------------------------------------------------------------------

AppSettings AppSettings::load(const std::vector<std::string>& commandLine, Options options) {
    AppSettings settings;  // Layer 1: the built-in defaults, as declared in AppConfig.

    EnvironmentLookup environment =
        options.environment ? std::move(options.environment) : systemEnvironment();

    const auto record = [&settings](const KeySpec& spec, Layer layer) {
        settings.sources_[std::string(spec.key)] = layer;
    };
    const auto applyValue = [&](const KeySpec& spec, std::string_view value, Layer layer,
                                std::string_view origin) {
        std::string error;
        if (spec.apply(value, settings.config_, error)) {
            record(spec, layer);
            return;
        }
        // Note the KEY and the reason only — never the rejected value.
        settings.diagnostics_.push_back(std::string(origin) + ": " + std::string(spec.key) +
                                       ": " + error + " (ignored)");
    };

    // --- Which file? --config beats PALMIER_CONFIG_FILE beats the XDG derivation,
    // and an explicit Options::configFile beats all three (tests point this at a
    // scratch directory so no real user configuration is ever read).
    std::optional<std::filesystem::path> configFile = options.configFile;
    if (!configFile) {
        for (std::size_t index = 0; index < commandLine.size(); ++index) {
            const std::string& argument = commandLine[index];
            if (argument.rfind(std::string(kConfigFlag) + "=", 0) == 0) {
                configFile = std::filesystem::path(argument.substr(kConfigFlag.size() + 1));
            } else if (argument == kConfigFlag && index + 1 < commandLine.size()) {
                configFile = std::filesystem::path(commandLine[index + 1]);
            }
        }
    }
    if (!configFile) {
        if (const std::optional<std::string> fromEnv = environment(kEnvConfigFile)) {
            configFile = std::filesystem::path(*fromEnv);
        }
    }
    if (!configFile) {
        std::optional<std::filesystem::path> base;
        if (const std::optional<std::string> xdg = environment(kEnvXdgConfigHome)) {
            base = std::filesystem::path(*xdg);
        } else if (const std::optional<std::string> home = environment(kEnvHome)) {
            base = std::filesystem::path(*home) / ".config";
        }
        if (base) {
            configFile = *base / kConfigDirName / kConfigFileName;
        }
    }
    settings.configFilePath_ = configFile;

    // --- Layer 2: the key=value file. An absent file is not an error: the defaults
    // simply stand (Requirement 10.1's loopback-only endpoint among them).
    if (configFile) {
        std::ifstream input(*configFile);
        if (input) {
            settings.configFileRead_ = true;
            std::string line;
            int lineNumber = 0;
            while (std::getline(input, line)) {
                ++lineNumber;
                const std::string_view content = trim(line);
                if (content.empty() || content.front() == '#' || content.front() == ';') {
                    continue;
                }
                const std::size_t separator = content.find('=');
                const auto where = [&] {
                    return configFile->string() + ":" + std::to_string(lineNumber);
                };
                if (separator == std::string_view::npos) {
                    // Malformed: no `=`. The line's text is withheld, since a
                    // mangled line could contain part of a secret.
                    settings.diagnostics_.push_back(
                        where() + ": malformed line, expected key=value (ignored)");
                    continue;
                }
                const std::string key = toLower(trim(content.substr(0, separator)));
                const std::string_view value = trim(content.substr(separator + 1));
                if (key.empty()) {
                    settings.diagnostics_.push_back(
                        where() + ": malformed line, empty key (ignored)");
                    continue;
                }
                const KeySpec* spec = findKey(key);
                if (spec == nullptr) {
                    settings.diagnostics_.push_back(where() + ": unknown key '" + key +
                                                    "' (ignored)");
                    continue;
                }
                applyValue(*spec, value, Layer::ConfigFile, where());
            }
        }
    }

    // --- Layer 3: the environment. Only the names in the key table are requested.
    for (const KeySpec& spec : kKeys) {
        if (const std::optional<std::string> value = environment(spec.envVar)) {
            applyValue(spec, *value, Layer::Environment, spec.envVar);
        }
    }

    // --- Layer 4: command-line flags, the last word.
    for (std::size_t index = 0; index < commandLine.size(); ++index) {
        const std::string& argument = commandLine[index];
        if (argument == kHelpFlagLong || argument == kHelpFlagShort) {
            // Asking for the option list is a request, not a setting and not a
            // mistake: it sets no key, records no diagnostic and is not positional.
            settings.helpRequested_ = true;
            continue;
        }
        if (argument == kConfigFlag) {
            ++index;  // its value was consumed by the pre-scan above
            continue;
        }
        if (argument.rfind(std::string(kConfigFlag) + "=", 0) == 0) {
            continue;
        }
        if (argument.rfind("--", 0) != 0) {
            settings.positional_.push_back(argument);
            continue;
        }

        std::string name = argument;
        std::optional<std::string> inlineValue;
        if (const std::size_t equals = argument.find('='); equals != std::string::npos) {
            name = argument.substr(0, equals);
            inlineValue = argument.substr(equals + 1);
        }

        const KeySpec* spec = findFlag(name);
        if (spec == nullptr) {
            settings.diagnostics_.push_back("command line: unknown option '" + name +
                                           "' (ignored)");
            continue;
        }
        if (inlineValue) {
            applyValue(*spec, *inlineValue, Layer::CommandLine, "command line");
            continue;
        }
        const bool hasNext =
            index + 1 < commandLine.size() && commandLine[index + 1].rfind("--", 0) != 0;
        // A boolean flag only consumes the next argument when that argument is
        // itself a boolean; otherwise the bare flag means true and the argument is
        // a positional one.
        if (hasNext && (!spec->valueOptional || looksBoolean(commandLine[index + 1]))) {
            applyValue(*spec, commandLine[index + 1], Layer::CommandLine, "command line");
            ++index;
            continue;
        }
        if (spec->valueOptional) {
            applyValue(*spec, "true", Layer::CommandLine, "command line");
            continue;
        }
        settings.diagnostics_.push_back("command line: option '" + name +
                                       "' requires a value (ignored)");
    }

    return settings;
}

}  // namespace palmier::app
