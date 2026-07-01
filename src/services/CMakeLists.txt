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
