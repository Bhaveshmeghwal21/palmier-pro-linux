// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/GenerativeBackendRegistry.hpp — configuration id -> Generative_Backend
// (task 10.5; design.md D9; Requirements 12.1, 12.2, 12.4, 12.8).
//
// Requirement 12.1: "THE Composition_Root SHALL accept exactly one
// Generative_Backend implementation through configuration, SHALL route every
// `generation.generate` invocation arriving from the Tool_Surface, the
// MCP_Endpoint or the in-app agent through that implementation, and SHALL install
// the offline stub backend when no generative backend is configured."
//
// This is the mapping half of that, and it is the exact shape task 10.1's
// `AgentInterpreterRegistry` established, for the same reasons. A configuration
// string names one of three implementations, ALL of which are compiled into every
// build, and `selectGenerativeBackend` returns the one backend to install together
// with the id actually installed (Requirement 12.2: "selectable at application
// start by a configuration identifier without recompilation"). The composition
// root does the owning and the exposing, through `generativeBackendId()`.
//
//   * `offline` (the default) — the offline stub. Contacts nothing, holds no
//     transport at all, and reports the unmet precondition by name. This is what
//     Requirement 12.4 demands and it is the only backend that can honour it
//     within 1 second unconditionally, because it has no way to block.
//   * `hosted`  — `HostedGenerativeBackend`, the hosted Palmier service client.
//     Requires an authenticated hosted account, reported by the injected
//     `credentials` probe, and reads its endpoint credential from the
//     `SecretStore` at request time.
//   * `byok`    — `ByokGenerativeBackend`, a user-supplied provider key, likewise
//     probed and likewise read from the `SecretStore` at request time.
//
// No credential value is compiled in anywhere beneath this header
// (Requirement 12.6). Task 10.8's Property 68 scans the whole repository for
// exactly that, so a hard-coded key would fail the suite rather than ship.
//
// Fallback policy (why an unknown id is not fatal)
// -----------------------------------------------
// Requirement 12.8 makes the policy explicit: an unrecognised id, or a recognised
// one whose credentials are absent, installs `offline`, records a startup error
// naming the rejected id and the unmet requirement, and STILL constructs every
// other component named in Requirement 1. Generation is an optional capability;
// letting a mistyped configuration string prevent the editor from opening would
// trade a working editor for a typo. The observable consequence is that
// `GenerativeBackendSelection::id` is the id actually in force, which may differ
// from the one requested.
//
// Requirement 12.5 is the other half of that bargain, and it is why the offline
// backend is a real component rather than a hole: with `offline` in force every
// non-generation operation — timeline editing, playback, project save and open,
// export, and the MCP endpoint — keeps working exactly as specified in
// Requirements 1 through 10. Nothing in this file touches any of them.

#ifndef PALMIER_SERVICES_GENERATIVEBACKENDREGISTRY_HPP
#define PALMIER_SERVICES_GENERATIVEBACKENDREGISTRY_HPP

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "services/GenerativeClient.hpp"        // IGenerativeBackend, GenerationRequest
#include "services/GenerativeHttpTransport.hpp" // GenerativeHttpTransport, GenerativeEndpoint

namespace palmier::services {

class SecretStore;  // services/SecretStore.hpp

// ---------------------------------------------------------------------------
// The ids
// ---------------------------------------------------------------------------

/// The offline stub — the default, and the fallback (Requirements 12.1, 12.8).
inline constexpr std::string_view kGenerativeBackendOffline = "offline";

/// The hosted Palmier service (needs an authenticated hosted account).
inline constexpr std::string_view kGenerativeBackendHosted = "hosted";

/// A user-supplied provider key (BYOK).
inline constexpr std::string_view kGenerativeBackendByok = "byok";

/// Every selectable id, in the order Requirement 12.2 lists them, `offline` first
/// because it is the default.
[[nodiscard]] const std::vector<std::string_view>& generativeBackendIds();

/// True iff `id` names a backend in the registry.
[[nodiscard]] bool isGenerativeBackendId(std::string_view id);

/// The id installed when configuration names nothing.
[[nodiscard]] inline std::string_view defaultGenerativeBackendId() {
    return kGenerativeBackendOffline;
}

// ---------------------------------------------------------------------------
// The backend interface the registry installs
// ---------------------------------------------------------------------------

/// A registry-installable generative backend: an `IGenerativeBackend` that can
/// also name itself and report — without contacting anything — whether it is able
/// to submit at all.
///
/// `unmetPrecondition()` is the mechanism behind Requirement 12.4. The generative
/// pipeline's entitlement gate answers a different question ("is this user
/// entitled?"); this one answers "can this backend reach a generative service?"
/// and names the specific reason it cannot: no reachable network, no authenticated
/// account, or no BYOK credentials. Asking the SELECTED backend first is what lets
/// the `generation.generate` hook reject an offline request in microseconds, with
/// the precondition named, having created no media library entry, no clip and no
/// undo-history entry — because nothing downstream of the hook has run.
class GenerativeBackend : public IGenerativeBackend {
public:
    /// The registry id this instance was installed under.
    [[nodiscard]] virtual std::string_view backendId() const noexcept = 0;

    /// Empty when the backend is able to submit. Otherwise a single sentence
    /// naming the unmet precondition, suitable for surfacing to the user verbatim.
    /// Never contains a credential value; it is computed without any network
    /// activity, so calling it is free and cannot block.
    [[nodiscard]] virtual std::string unmetPrecondition() const = 0;
};

/// The offline stub backend (Requirements 12.1, 12.4, 12.5).
///
/// `reason` is the unmet precondition it reports — the one the composition root
/// determined when it fell back. An empty `reason` yields the generic "no
/// generative backend is configured". Every method fails fast; `cancel` succeeds,
/// because a job that was never submitted is not running.
[[nodiscard]] std::unique_ptr<GenerativeBackend> makeOfflineGenerativeBackend(
    std::string reason = {});

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------

/// What the registry needs in order to decide. Nothing here is a credential
/// VALUE: `secretStore` is where values are read from, at request time.
struct GenerativeBackendRequest {
    /// The configured id. Empty means "use the default".
    std::string id;

    /// Reports whether the credentials `id` requires are present and usable. Left
    /// empty it means "no credentials are present", which is the Offline_Mode
    /// default and the state a bare composition is in. `offline` never consults it.
    std::function<bool(std::string_view id)> credentials;

    /// Where `hosted` / `byok` read their endpoint credential at request time.
    /// Null means no store is available, which is itself an unmet precondition.
    SecretStore* secretStore = nullptr;

    /// The injectable network seam. Null installs
    /// `makeUnavailableGenerativeHttpTransport()`, so a build with no HTTPS client
    /// still selects the id and reports the capability as absent per request.
    GenerativeHttpTransport* transport = nullptr;

    /// The configured endpoint. A location, never a credential.
    GenerativeEndpoint endpoint;

    /// The BYOK provider whose stored key `byok` should load.
    std::string byokProvider;

    /// The secret-store key scope, matching `ByokCredentialManager`'s user scope.
    std::string userId = "default";
};

/// The outcome: the backend to install, the id actually in force, and the startup
/// diagnostic when the two differ.
struct GenerativeBackendSelection {
    /// The id in force — `request.id` when it was accepted, otherwise `offline`.
    /// This is what the composition root's accessor reports (Requirement 12.2).
    std::string id;

    /// The backend to install. Never null: some backend is always installed.
    std::unique_ptr<GenerativeBackend> backend;

    /// Empty when the requested id was installed as asked. Otherwise a single line
    /// naming the rejected id and the unmet requirement, suitable for
    /// `startupErrors()` (Requirement 12.8). Never contains a credential value.
    std::string startupError;

    /// True when `id` differs from what was requested.
    [[nodiscard]] bool fellBack() const noexcept { return !startupError.empty(); }
};

/// Resolve `request` into the one backend to install. Never throws, never touches
/// the network and never fails: an unusable request yields the offline stub plus a
/// startup error (Requirement 12.8).
[[nodiscard]] GenerativeBackendSelection selectGenerativeBackend(
    const GenerativeBackendRequest& request);

}  // namespace palmier::services

#endif  // PALMIER_SERVICES_GENERATIVEBACKENDREGISTRY_HPP
