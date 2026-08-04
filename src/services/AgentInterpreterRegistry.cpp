// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/AgentInterpreterRegistry.cpp — implementation (task 10.1;
// Requirements 11.1, 11.3, 11.8). See the header for the policy.

#include "services/AgentInterpreterRegistry.hpp"

#include <algorithm>
#include <utility>

#include "core/Error.hpp"

namespace palmier::services {
namespace {

/// The interpreter installed for a selected-but-unwired `hosted`/`byok`: every
/// utterance reports the backend failure by name and invokes no tool
/// (Requirement 11.8). It cannot touch the project — an IntentInterpreter has no
/// access to one — so "leaves the project unchanged" holds by construction.
[[nodiscard]] IntentInterpreter makeUnwiredBackendInterpreter(std::string id) {
    return [id = std::move(id)](std::string_view) -> Result<AgentIntent> {
        return err<AgentIntent>(makeError(
            ErrorCode::Unsupported,
            "the '" + id +
                "' agent interpreter reported a backend failure: its request transport is "
                "not available in this build; no tool was invoked"));
    };
}

/// The one line `startupErrors()` carries when a requested id was not installed.
/// Names the rejected id and the unmet requirement, and nothing else — in
/// particular no credential value, which this function never sees.
[[nodiscard]] std::string fallbackNotice(std::string_view rejected, std::string_view reason) {
    return "agent interpreter '" + std::string(rejected) + "' was not installed (" +
           std::string(reason) + "); the built-in '" +
           std::string(kAgentInterpreterOffline) + "' interpreter is in force instead";
}

}  // namespace

const std::vector<std::string_view>& agentInterpreterIds() {
    static const std::vector<std::string_view> ids = {
        kAgentInterpreterOffline, kAgentInterpreterHosted, kAgentInterpreterByok};
    return ids;
}

bool isAgentInterpreterId(std::string_view id) {
    const std::vector<std::string_view>& ids = agentInterpreterIds();
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

AgentInterpreterSelection selectAgentInterpreter(const AgentInterpreterRequest& request) {
    AgentInterpreterSelection selection;

    const std::string requested =
        request.id.empty() ? std::string(defaultAgentInterpreterId()) : request.id;

    // --- An id no backend answers to: install offline, say which id was rejected.
    if (!isAgentInterpreterId(requested)) {
        selection.id = std::string(kAgentInterpreterOffline);
        selection.interpreter = makeOfflineIntentInterpreter(request.offlineOptions);
        selection.startupError = fallbackNotice(
            requested, "it names no interpreter in the registry, which offers 'offline', "
                       "'hosted' and 'byok'");
        return selection;
    }

    // --- offline: always available, no credentials, no network. ---------------
    if (requested == kAgentInterpreterOffline) {
        selection.id = std::string(kAgentInterpreterOffline);
        selection.interpreter = makeOfflineIntentInterpreter(request.offlineOptions);
        return selection;
    }

    // --- hosted / byok: credentials decide. ----------------------------------
    const bool authorized = request.credentials && request.credentials(requested);
    if (!authorized) {
        selection.id = std::string(kAgentInterpreterOffline);
        selection.interpreter = makeOfflineIntentInterpreter(request.offlineOptions);
        selection.startupError = fallbackNotice(
            requested,
            requested == kAgentInterpreterHosted
                ? "it requires an authenticated hosted account and none is present"
                : "it requires BYOK provider credentials and none are present");
        return selection;
    }

    selection.id = requested;
    selection.interpreter = makeUnwiredBackendInterpreter(requested);
    return selection;
}

}  // namespace palmier::services
