// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/RemoteAccessGate.hpp — the Remote_Access_Gate's configuration value
// type (Requirements 10.1-10.3, 10.9, 10.11, 16.3; design.md decision D4).
//
// STATUS: task 6.1 introduces ONLY `RemoteAccessConfig`, the value type that
// `app::AppSettings` resolves from defaults / the config file / the environment /
// the command line. Task 6.2 owns this header and extends it in place with
// `BindDecision`, `RejectionReason`, `Admission`, `RejectionLog` and the
// `RemoteAccessGate` class itself (bind-time `validate()` and per-request
// `admit()`). The configuration type lives here — rather than in the app layer —
// precisely so that 6.2 extends this declaration instead of duplicating it.
// `BindDecision`, the *output* of `validate()`, already lives in
// `services/RemoteAccessTypes.hpp` because the MCP transport needs it; task 6.2
// includes that header here rather than declaring a second one.
//
// Secure by default: a default-constructed RemoteAccessConfig describes the
// loopback-only endpoint of Requirement 10.1. Nothing in the settings layer
// turns remote access on; only an explicit `true` in configuration does, and even
// then the gate (task 6.2) still has to find a valid bind address, a 32-512
// printable-ASCII bearer token and `acknowledged == true` before a non-loopback
// address is bound (Requirements 10.2, 10.3).

#ifndef PALMIER_SERVICES_REMOTEACCESSGATE_HPP
#define PALMIER_SERVICES_REMOTEACCESSGATE_HPP

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace palmier::services {

/// Everything the Remote_Access_Gate needs to decide (a) whether a non-loopback
/// bind is permitted at all and (b) whether an individual request is admitted.
/// Populated by `app::AppSettings`; validated by `RemoteAccessGate` (task 6.2).
struct RemoteAccessConfig {
    /// Opt-in switch. False (the default) means the MCP endpoint binds only
    /// 127.0.0.1:19789 and serves loopback clients exactly as it does today
    /// (Requirements 10.1, 10.10).
    bool enabled = false;

    /// The non-loopback address to bind when `enabled`. Must be a syntactically
    /// valid IPv4 or IPv6 literal (Requirement 10.2); empty is an unmet
    /// prerequisite, not an invitation to bind a wildcard address.
    std::string bindAddress;

    /// The port for the endpoint (loopback or not); 19789 is the documented
    /// default (Requirements 10.1, 16.2).
    std::uint16_t port = 19789;

    /// Bearer token required on every request once a non-loopback address is
    /// bound: 32 to 512 printable ASCII characters (Requirement 10.2). Never
    /// logged, never echoed in a diagnostic (Requirements 10.3, 10.8).
    std::string bearerToken;

    /// The operator's explicit acknowledgement that the endpoint is being exposed
    /// beyond loopback (Requirement 10.2).
    bool acknowledged = false;

    /// Hosts accepted in an `Origin` header. An empty list is treated as empty,
    /// i.e. only the bound address and loopback hosts are accepted
    /// (Requirement 10.5).
    std::vector<std::string> originAllowList;

    /// Optional TLS material. Both must be present, loadable and a matching pair,
    /// or the gate falls back to loopback (Requirements 10.6, 10.12).
    std::optional<std::filesystem::path> tlsCertificate;
    std::optional<std::filesystem::path> tlsPrivateKey;

    /// Concurrent MCP session ceiling, 1..32, default 8 (Requirement 10.9).
    int maxSessions = 8;

    /// Idle-session timeout, 30..3600 seconds, default 300 (Requirement 10.11).
    std::chrono::seconds idleTimeout{300};
};

}  // namespace palmier::services

#endif  // PALMIER_SERVICES_REMOTEACCESSGATE_HPP
