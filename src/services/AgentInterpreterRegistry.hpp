// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/AgentInterpreterRegistry.hpp — configuration id -> IntentInterpreter
// (task 10.1; design.md D9; Requirements 11.1, 11.3, 11.8).
//
// Requirement 11.1: "THE Composition_Root SHALL accept exactly one
// Agent_Interpreter implementation through configuration, SHALL expose the active
// implementation through a public accessor, and SHALL route every in-app agent
// utterance through that implementation."
//
// This is the mapping half of that: a configuration string names one of three
// compiled-in implementations, and `selectAgentInterpreter` returns the one
// interpreter to install together with the id that was actually installed. The
// composition root does the owning and the exposing.
//
//   * `offline` (the default) — `OfflineIntentInterpreter`: the documented phrase
//     table, no network, well inside 1 s (Requirement 11.3). Always available,
//     needs no credentials and cannot fail to construct.
//   * `hosted`  — the hosted reasoning service. Requires an authenticated hosted
//     account, reported by the injected `credentials` probe.
//   * `byok`    — a user-supplied provider key, likewise reported by the probe.
//
// This replaces `makeUnconfiguredInterpreter()`, the previous default, which
// returned `FailedPrecondition` for every utterance and so made the agent inert
// in any build without a model backend. There is no longer a configuration in
// which the agent has no interpreter at all.
//
// Fallback policy (why an unknown id is not fatal)
// -----------------------------------------------
// An unrecognised id, or a recognised one whose credentials are absent, installs
// `offline` and records a startup error naming the rejected id and the unmet
// requirement. It is not a startup failure, for the same reason design.md gives
// for the generative backend: the agent is an optional convenience, and letting a
// mistyped configuration string prevent the editor from opening would trade a
// working editor for a typo. The observable consequence is that
// `AgentInterpreterSelection::id` is the id that is actually in force, which may
// differ from the one that was requested.
//
// Credential and backend failures (Requirement 11.8)
// -------------------------------------------------
// When `hosted` or `byok` IS selected — its credentials are reported present —
// but the transport that would carry the request is not wired in this build, the
// installed interpreter returns an error naming the failure reason for every
// utterance. It invokes no tool and touches no project, which it cannot do in any
// case: an `IntentInterpreter` is a pure translation and has no access to the
// session. The concrete HTTPS clients arrive with task 10.5; until then the id is
// selectable and its failure mode is the documented one rather than a crash.

#ifndef PALMIER_SERVICES_AGENTINTERPRETERREGISTRY_HPP
#define PALMIER_SERVICES_AGENTINTERPRETERREGISTRY_HPP

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "services/AgentOrchestrator.hpp"          // IntentInterpreter
#include "services/OfflineIntentInterpreter.hpp"   // OfflineIntentInterpreter::Options

namespace palmier::services {

// ---------------------------------------------------------------------------
// The ids
// ---------------------------------------------------------------------------

/// The built-in offline interpreter — the default, and the fallback.
inline constexpr std::string_view kAgentInterpreterOffline = "offline";

/// The hosted reasoning service (needs an authenticated hosted account).
inline constexpr std::string_view kAgentInterpreterHosted = "hosted";

/// A user-supplied provider key (BYOK).
inline constexpr std::string_view kAgentInterpreterByok = "byok";

/// Every selectable id, in the order design.md D9 lists them. `offline` first,
/// because it is the default.
[[nodiscard]] const std::vector<std::string_view>& agentInterpreterIds();

/// True iff `id` names an interpreter in the registry.
[[nodiscard]] bool isAgentInterpreterId(std::string_view id);

/// The id installed when configuration names nothing.
[[nodiscard]] inline std::string_view defaultAgentInterpreterId() {
    return kAgentInterpreterOffline;
}

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------

/// What the registry needs in order to decide.
struct AgentInterpreterRequest {
    /// The configured id. Empty means "use the default".
    std::string id;

    /// How the offline interpreter is built when it is the one installed (whether
    /// by request or by fallback).
    OfflineIntentInterpreter::Options offlineOptions;

    /// Reports whether the credentials `id` requires are present and usable. Left
    /// empty it means "no credentials are present", which is the Offline_Mode
    /// default and the state a bare composition is in. `offline` never consults it.
    std::function<bool(std::string_view id)> credentials;
};

/// The outcome: the interpreter to install, the id actually in force, and the
/// startup diagnostic when the two differ.
struct AgentInterpreterSelection {
    /// The id that is in force — `request.id` when it was accepted, otherwise
    /// `offline`. This is what the composition root's accessor reports.
    std::string id;

    /// The interpreter to install. Never empty: some interpreter is always
    /// installed.
    IntentInterpreter interpreter;

    /// Empty when the requested id was installed as asked. Otherwise a single line
    /// naming the rejected id and the unmet requirement, suitable for
    /// `startupErrors()`. Never contains a credential value.
    std::string startupError;

    /// True when `id` differs from what was requested.
    [[nodiscard]] bool fellBack() const noexcept { return !startupError.empty(); }
};

/// Resolve `request` into the one interpreter to install. Never throws, never
/// touches the network and never fails: an unusable request yields the offline
/// interpreter plus a startup error.
[[nodiscard]] AgentInterpreterSelection selectAgentInterpreter(
    const AgentInterpreterRequest& request);

}  // namespace palmier::services

#endif  // PALMIER_SERVICES_AGENTINTERPRETERREGISTRY_HPP
