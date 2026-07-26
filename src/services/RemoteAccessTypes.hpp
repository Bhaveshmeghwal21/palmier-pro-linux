// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/RemoteAccessTypes.hpp — the bind-time value type shared by the MCP
// transport and the (later) remote-access gate.
//
// design.md D4 gives `BindDecision` as part of `services::RemoteAccessGate`
// (task 6.2): the gate's `validate()` turns a `RemoteAccessConfig` into the
// decision the transport then binds. The transport (task 5.3) needs the decision
// type *before* the gate exists, so the type lives here, in its own tiny header,
// rather than inside `RemoteAccessGate.hpp`:
//
//   * `McpServer::start(const BindDecision&)` can take it without depending on
//     the gate at all — the transport binds what it is told to bind and holds no
//     policy;
//   * task 6.2 **extends this struct in place** (adding
//     `std::vector<std::string> unmetPrerequisites` and
//     `std::optional<std::string> plaintextWarning`, the two members design.md
//     lists that only the gate produces and only the startup log consumes) and
//     includes this header from `RemoteAccessGate.hpp`. It must not declare a
//     second `BindDecision`: there is exactly one, and it is this one.
//
// Only the four members the transport actually needs are declared today: the
// host and port to bind, whether the bind is loopback-only, and whether the
// listener is expected to serve TLS. Requirement 10.1's default — loopback
// `127.0.0.1:19789`, refusing every non-loopback source — is expressed by
// `BindDecision::loopback()`, so "secure by default" is a value, not a
// convention.

#ifndef PALMIER_SERVICES_REMOTEACCESSTYPES_HPP
#define PALMIER_SERVICES_REMOTEACCESSTYPES_HPP

#include <cstdint>
#include <string>

namespace palmier::services {

/// Where the MCP endpoint should listen, and under what transport constraints.
///
/// `loopbackOnly` is the admission contract, not a hint: when it is true the
/// transport refuses to bind anything but a loopback literal (Requirement 10.1).
/// `tlsEnabled` is honoured by the optional TLS transport of task 6.3; until
/// that lands, a decision asking for TLS is refused rather than silently served
/// as plaintext.
struct BindDecision {
    std::string   host = "127.0.0.1";
    std::uint16_t port = 19789;
    bool          loopbackOnly = true;
    bool          tlsEnabled = false;

    /// The Requirement 10.1 default: loopback-only, plaintext, port 19789.
    /// `port == 0` binds an ephemeral port, which is what tests use.
    [[nodiscard]] static BindDecision loopback(std::uint16_t port = 19789) {
        BindDecision decision;
        decision.host = "127.0.0.1";
        decision.port = port;
        decision.loopbackOnly = true;
        decision.tlsEnabled = false;
        return decision;
    }
};

}  // namespace palmier::services

#endif  // PALMIER_SERVICES_REMOTEACCESSTYPES_HPP
