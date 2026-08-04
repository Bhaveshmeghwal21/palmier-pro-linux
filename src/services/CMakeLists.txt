# SPDX-License-Identifier: GPL-3.0-or-later
#
# src/services — service layer.
#
# MCP server (:19789/mcp), .palmier project I/O, auth & subscription client,
# generative AI client, the in-app agent orchestrator, and the (Qt-free)
# localization policy. Links libsecret for secure credential storage and lcms2
# for color management. Further concrete sources are added by later tasks
# (5.x, 14.x, 15.x, 16.x).

add_library(palmier_services
    AuthenticationService.cpp
    ByokCredentialManager.cpp
    GenerativeClient.cpp
    GenerativeMediaCoordinator.cpp
    KeyMomentDetector.cpp
    KeyMomentMarkers.cpp
    LocalizationManager.cpp
    ProjectSaveService.cpp
    ProjectStore.cpp
    SecretStore.cpp
    TranscriptionService.cpp)
add_library(Palmier::services ALIAS palmier_services)

target_include_directories(palmier_services PUBLIC
    "${PROJECT_SOURCE_DIR}/src")

target_compile_features(palmier_services PUBLIC cxx_std_20)

# libsecret (Secret Service API) provides non-plaintext credential storage
# (design.md Security Considerations; Requirement 9.5). It is a required
# dependency (see cmake/PalmierDependencies.cmake), so the real libsecret-backed
# SecretStore is always compiled in for this library. The PALMIER_HAVE_LIBSECRET
# define guards that platform code so the same source compiles (as an
# Unsupported-returning stub) in build/test configurations that do not link
# libsecret — e.g. the standalone unit-test binaries.
target_compile_definitions(palmier_services PRIVATE PALMIER_HAVE_LIBSECRET=1)

target_link_libraries(palmier_services PUBLIC
    Palmier::core
    Palmier::media
    PkgConfig::LIBSECRET
    PkgConfig::LCMS2)


# --- Task 15.1: MCP tool surface (ToolRegistry + JSON value) ----------------
#
# The editor tool surface shared by the MCP HTTP server (tasks 15.2/15.3) and
# the in-app agent orchestrator (task 16.1). It maps each edit tool to the same
# EditCommand path the UI drives (design.md Component 2 / Property P4;
# Requirements 7.4, 7.8). Json is a dependency-light JSON value used to express
# tool input schemas and carry tool call arguments/results, so this surface
# stays transport-agnostic and depends only on Palmier::core. Appended here so
# the existing source list above is preserved verbatim.
target_sources(palmier_services PRIVATE
    Json.cpp
    ToolRegistry.cpp)



# --- Task 15.2: MCP HTTP transport + server lifecycle -----------------------
#
# The MCP server's transport half (design.md Component 2; Requirements 7.1, 7.2,
# 7.3, 7.9): a loopback-only HTTP listener at 127.0.0.1:19789 serving `/mcp`,
# with start()/stop() lifecycle and port-in-use refusal. It delegates request
# handling to an injected handler (task 15.3's executor is plugged in by the
# composition root, task 21.1), so it stays transport-only and depends only on
# Palmier::core (Result/Error) plus the service-layer Json value. Uses POSIX
# sockets only (no Qt/FFmpeg/Vulkan). Appended so the existing source list is
# preserved verbatim.
target_sources(palmier_services PRIVATE
    McpServer.cpp)



# --- Task 15.3: MCP tool execution policy (McpToolExecutor) -----------------
#
# The transport-agnostic execution policy for the MCP server (design.md
# Component 2): given a decoded tool name + JSON arguments it runs the tool from
# the shared ToolRegistry against the TimelineEngine under the required policy —
# recognized tool executes within the time budget (7.4); unknown tool -> error,
# project unchanged (7.5); execution failure or timeout -> roll back to the
# pre-invocation state + error (7.6, 7.7); no project open -> error (7.10); and
# tool inputs are validated against their JSON schema before any EditCommand is
# created (7.10). The HTTP endpoint/lifecycle (task 15.2) calls into this
# executor. Depends only on Palmier::core and the Json/ToolRegistry surface, so
# it stays free of any HTTP transport dependency. Appended here so the existing
# source list above is preserved verbatim.
target_sources(palmier_services PRIVATE
    McpToolExecutor.cpp)




# --- Task 16.1: In-app agent chat orchestrator -----------------------------
#
# The built-in agent chat (design.md Component 6; Requirements 8.1, 8.5, 8.6,
# 8.7). It maintains a chat session and routes user intents into the SAME MCP
# tool executor + ToolRegistry the MCP server drives, so the UI, MCP, and agent
# all mutate the project through one atomic/undoable path (Property P4): a
# successful edit is reflected synchronously (8.6) and a failed edit is rolled
# back leaving the project unchanged (8.7). It gates each send on
# subscription/BYOK via the AuthenticationService while preserving the unsent
# message content on rejection (8.5). @-mention resolution (task 16.2) plugs
# into a preprocessor seam. Transport/UI-agnostic (no Qt): depends only on
# Palmier::core plus the service-layer Json/ToolRegistry/McpToolExecutor and
# AuthenticationService. Appended here so the existing source list is preserved
# verbatim.
target_sources(palmier_services PRIVATE
    AgentOrchestrator.cpp)




# --- Task 16.2: @-mention resolution ---------------------------------------
#
# The in-app agent chat's @-mention resolver (design.md Component 6;
# Requirements 8.2, 8.3, 8.4). It parses @ mentions from a user message and
# resolves each against the current project's media library (MediaManager /
# Project.assets): a unique match is rewritten to a canonical @<assetId>
# reference (8.2); an unmatched mention is rejected so the message is not
# submitted (8.3); a mention matching multiple items yields a distinct
# "needs selection" result listing the candidates (8.4). It plugs into the
# AgentOrchestrator's MessagePreprocessor seam (task 16.1) via the
# makeMentionPreprocessor() adapters, staying transport/UI-agnostic: depends
# only on Palmier::core plus the service-layer Json/AgentOrchestrator surface.
# Appended here so the existing source list above is preserved verbatim.
target_sources(palmier_services PRIVATE
    MentionResolver.cpp)



# --- Task 3.1: one argument specification per tool (ToolSchema) -------------
#
# The single declaration of a tool's accepted arguments (design.md D3,
# "Schema/handler agreement"; Requirements 9.3, 9.9, 9.12). Both the JSON Schema
# published by `tools/list` and the validator McpToolExecutor runs before any
# EditCommand is created are generated from the same ArgSpec list, so the
# advertised schema and the handler cannot drift. Depends only on Palmier::core
# (Result/Error/Uuid) and the service-layer Json value. Appended here so the
# existing source list above is preserved verbatim.
target_sources(palmier_services PRIVATE
    ToolSchema.cpp)




# --- Tasks 2.1 / 2.2: Project_Session ---------------------------------------
#
# services::ProjectSession is the central new abstraction of the end-to-end
# editor integration (design.md D1): the single owner of the currently open
# project — exactly one TimelineEngine for its whole lifetime, the MediaManager
# media library, the project identity, the optional on-disk document path, a
# monotonically increasing revision counter and the modified flag derived from
# it. createProject/openProject are not EditCommands and are not undoable: they
# build a complete Project value locally and commit through
# TimelineEngine::reset only on full success, so a failed open leaves the
# previous project, its path, its modified flag and its undo history unchanged
# (Requirements 1.10, 3.2, 3.4, 3.9, 4.10).
#
# Task 2.2 adds the off-thread save with the revision guard (design.md D6, the
# Linux adaptation of upstream PR 403): requestSave captures
# (Project snapshot, revision) and hands them to a std::jthread running
# ProjectSaveService::save, returning immediately so a slow or failing write
# never blocks the UI thread (Requirements 4.1, 4.4, 4.6, 14.6, 14.7). It
# therefore builds on ProjectSaveService/ProjectStore (already in this target)
# and depends only on Palmier::core beyond them. Appended here so the existing
# source list above is preserved verbatim.
target_sources(palmier_services PRIVATE
    ProjectSession.cpp)



# --- Task 4.1: Media_Import_Service ----------------------------------------
#
# services::MediaImportService is the single probe -> validate -> register path by
# which a file on disk becomes an asset of the current project's media library
# (design.md "services::MediaImportService"; Requirements 2.1-2.9). Every
# importer — the media-browser action, the `media.import` tool and the agent —
# funnels through it, which is what makes "exactly one entry per distinct
# filesystem location" true regardless of who asked: requests are canonicalised
# with std::filesystem::weakly_canonical and compared against the library
# (Requirements 2.5, 2.6). Nothing is registered until the probe, the validation
# and the decode check have all succeeded, so every rejection — unrecognised
# container or wholly undecodable codecs (2.3), an empty / missing / unreadable /
# undecodable target (2.4), no open project (2.7), or a probe that exceeds the
# 30 s limit (2.8) — leaves the library's entry count and contents unchanged, and
# the path is reported pending until registration completes (2.9).
#
# It composes the existing media layer rather than duplicating it: media/MediaProbe
# (through its injectable MediaProbeBackend seam) reads the container, and
# media/ImportValidation supplies the acceptance policy and the format describer.
# Palmier::media is already linked by this target, so no new dependency enters
# here. Appended so the existing source list above is preserved verbatim.
target_sources(palmier_services PRIVATE
    MediaImportService.cpp)



# --- Tasks 5.1 / 5.2 / 5.3: the MCP protocol layer --------------------------
#
# design.md D3 splits the endpoint into three separated concerns, of which two
# are new here and the third (McpServer, already in this target) is extended in
# place:
#
#   * services::McpSessionRegistry (task 5.1) — session identity, the
#     concurrent-session maximum and the idle timeout. A session id is a 256-bit
#     std::random_device value rendered as 64 lowercase hex characters, unique for
#     the whole process lifetime because every issued id is retained in a
#     never-pruned set while only live records expire (Requirements 9.10, 9.11,
#     9.14, 9.15, 10.9, 10.11). Its clock is injectable, so expiry is testable
#     without sleeping.
#   * services::McpProtocolHandler (task 5.2) — JSON-RPC 2.0 dispatch:
#     envelope validation, initialize / notifications/initialized / tools/list /
#     tools/call, the parse (-32700) -> envelope (-32600) -> method (-32601) ->
#     session state -> ToolSchema::validate (-32602) -> execute order, and the
#     MainThreadInvoker seam carrying Requirement 9.16's 60-second budget
#     (Requirements 9.1-9.16). It holds the ToolRegistry, the McpToolExecutor and
#     the session registry, and knows nothing about sockets.
#   * services::McpServer (task 5.3) — gains start(const BindDecision&), header
#     capture into McpRequestContext, Mcp-Session-Id emission, the 202-with-empty-
#     body answer and the 1 MiB body cap yielding -32700, delegating to the
#     protocol handler through the McpProtocolDelegate seam while dispatch() keeps
#     its pure, socket-free shape.
#
# services/RemoteAccessTypes.hpp (header-only) carries BindDecision so the
# transport needs no dependency on the remote-access gate; task 6.2 extends that
# struct in place rather than declaring a second one. All of this depends only on
# Palmier::core and the service-layer Json/ToolSchema/ToolRegistry surface.
# Appended here so the existing source list above is preserved verbatim.
target_sources(palmier_services PRIVATE
    McpSessionRegistry.cpp
    McpProtocolHandler.cpp)



# --- Tasks 6.2 / 6.3: Remote_Access_Gate + the optional TLS transport -------
#
# services::RemoteAccessGate is the admission control of design.md D4, with two
# deliberately separated responsibilities:
#
#   * bind-time — validate() turns a RemoteAccessConfig into the BindDecision the
#     transport binds. Absent configuration is loopback 127.0.0.1:19789
#     (Requirement 10.1); enabling a non-loopback bind requires ALL of a valid
#     IPv4/IPv6 literal, a 32-512 printable-ASCII bearer token and an explicit
#     acknowledgement, plus — when TLS material is configured — a certificate and
#     key that load and match. Any unmet prerequisite still binds loopback and
#     carries the NAMED prerequisites for the startup error, never the token
#     (Requirements 10.2, 10.3, 10.12). Enabled without TLS still binds and offers
#     exactly one plaintext warning (Requirement 10.7).
#   * per-request — admit() allows everything on a loopback-only binding, so the
#     current default developer experience is preserved bit for bit
#     (Requirement 10.10); on a non-loopback binding it checks the source block
#     list, the bearer token by constant-time compare (401), the Origin allow-list
#     extended with the bound address and loopback hosts (403), and the
#     session-count limit (Requirements 10.4, 10.5, 10.9, 10.13).
#
# Every rejection goes to a RejectionLog sink as a UTC millisecond timestamp, the
# source address and a reason CODE; the presented credential is never a parameter
# of the logging path, so it cannot appear in a record (Requirement 10.8).
#
# services::TlsTransport is the OpenSSL 3.x layer behind PALMIER_ENABLE_OPENSSL
# (default ON) with OPTIONAL detection setting PALMIER_HAVE_OPENSSL. The guard is
# defined only when the option is ON *and* OpenSSL 3.x was found, and the imported
# target is linked only in that same branch — the ENABLE-and-FOUND form
# src/gpu/CMakeLists.txt already models. With the guard absent the same sources
# still compile and report the capability unavailable, so configuring TLS becomes
# an unmet prerequisite and the gate falls back to loopback: a host without
# OpenSSL configures, builds and tests exactly as it did before this stage.
#
# Appended here so the existing source list above is preserved verbatim.
target_sources(palmier_services PRIVATE
    RemoteAccessGate.cpp
    TlsTransport.cpp)

if(PALMIER_ENABLE_OPENSSL AND OPENSSL_FOUND)
    target_compile_definitions(palmier_services PRIVATE PALMIER_HAVE_OPENSSL=1)
    target_link_libraries(palmier_services PRIVATE PkgConfig::OPENSSL)
endif()



# --- Task 10.1: the offline interpreter + the interpreter registry -----------
#
# design.md D9 makes agent reasoning pluggable by configuration id, and makes the
# DEFAULT a real implementation rather than an inert one:
#
#   * services::OfflineIntentInterpreter — the documented table of 12 phrase
#     patterns matched case-insensitively on the whitespace-trimmed, interior-
#     whitespace-collapsed utterance, resolving to tool invocations with captured
#     arguments. It is pure translation over a string: no socket, no credential,
#     no project access, so it issues no network request and answers well inside
#     the 1 s bound (Requirement 11.3), and a rejected or unmappable utterance
#     cannot have changed anything (Requirements 11.4, 11.9). The one route to the
#     network any interpreter built on it would use is a declared, injectable seam,
#     which is what lets the property test PROVE the no-network claim rather than
#     assume it.
#   * services::AgentInterpreterRegistry — ids `offline` (default), `hosted`,
#     `byok`, replacing the composition root's `makeUnconfiguredInterpreter()`. An
#     unknown id, or a credential-less `hosted`/`byok`, installs `offline` and
#     records a startup error naming the rejected id and the unmet requirement, so
#     a mistyped configuration string can never stop the editor from opening
#     (Requirements 11.1, 11.8).
#
# Both depend only on Palmier::core plus the service-layer Json/AgentOrchestrator
# surface and ProjectSession (for the session-backed editor-context provider); no
# HTTP, no Qt and no new dependency enters here. Appended so the existing source
# list above is preserved verbatim.
target_sources(palmier_services PRIVATE
    OfflineIntentInterpreter.cpp
    AgentInterpreterRegistry.cpp)


# --- Task 10.5: the generative backend registry and its HTTPS clients --------
#
# Requirement 12.2 requires the hosted service backend, a BYOK provider backend
# and the offline stub to be "each selectable at application start by a
# configuration identifier without recompilation". That is a statement about THIS
# list: all three are compiled into every build, so the choice is a string in a
# configuration file and never a `-D` and a rebuild.
#
#   * services::GenerativeHttpTransport — the injectable network seam plus the
#     submit -> poll -> fetch -> cancel wire protocol both HTTPS clients share.
#     No HTTP client library is linked: the default transport reports the
#     capability as absent without contacting anything, and a real one is a matter
#     of implementing one interface. The seam is also what makes request
#     construction, credential loading and error mapping testable with no endpoint,
#     and what lets a test PROVE the offline backend never touches the network.
#   * services::HostedGenerativeBackend / services::ByokGenerativeBackend — GPLv3
#     client code only (Requirement 12.6: the hosted service implementation stays
#     out of this repository). Each reads its credential from the SecretStore at
#     REQUEST time, so no credential value is compiled in anywhere — which task
#     10.8's Property 68 enforces by scanning the tree.
#   * services::GenerativeBackendRegistry — ids `offline` (default), `hosted`,
#     `byok`, replacing the composition root's private offline stub. An unknown id,
#     or a credential-less `hosted`/`byok`, installs the offline stub and records a
#     startup error naming the rejected id and the unmet requirement, while every
#     other component is still constructed (Requirement 12.8).
#
# All four depend only on Palmier::core plus the service layer's Json and
# SecretStore surfaces; no HTTP, no Qt and no new dependency enters here. Appended
# so the existing source list above is preserved verbatim.
target_sources(palmier_services PRIVATE
    GenerativeHttpTransport.cpp
    HostedGenerativeBackend.cpp
    ByokGenerativeBackend.cpp
    GenerativeBackendRegistry.cpp)



# --- Task 9.4: the Export_Coordinator ----------------------------------------
#
# services::ExportCoordinator owns admission, isolation, reporting and cleanup for
# one export (Requirements 6.5, 6.10, 6.11, 7.1-7.7, 7.9, 7.10, 7.11, 8.11):
#
#   * a PURE STATIC validate() run before anything is created, which is what makes
#     "create no file at the requested path" true for every rejection, and an
#     overwrite check that leaves an existing destination byte-for-byte intact;
#   * one std::thread per export over a VALUE-COPY Project snapshot with an
#     export-local gpu::GpuContext, gpu::Compositor, DecoderClipFrameProvider,
#     audio mix and media::MediaEncoder, and no reference to the ProjectSession at
#     all — which is how "an export never touches the project" is structural
#     rather than merely intended;
#   * progress marshalled to the owning thread, monotonic and at intervals of at
#     most one second measured on an INJECTED steady clock;
#   * a scope guard that calls MediaEncoder::finish() best-effort and then removes
#     the output path on any failure, cancellation or mid-export hardware failure.
#
# Encoder choice is delegated to media::EncoderSelector (task 9.1) rather than
# reimplemented. Palmier::media (and through it Palmier::gpu and Palmier::core) is
# already linked by this target, so no new dependency enters here. Appended so the
# existing source list above is preserved verbatim.
target_sources(palmier_services PRIVATE
    ExportCoordinator.cpp)
