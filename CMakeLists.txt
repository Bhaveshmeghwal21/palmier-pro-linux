# SPDX-License-Identifier: GPL-3.0-or-later
#
# Palmier Pro for Linux — top-level build configuration.
#
# This is the root build file for the ground-up C++20 Linux port of Palmier Pro.
# It targets C++20, discovers all third-party dependencies (failing configuration
# with a clear, per-dependency message when any are missing), exposes build options
# for the vendor hardware-codec paths (VAAPI / NVENC / QSV), and wires together the
# module subdirectory layout: core, media, gpu, services, ui, app, and tests.

cmake_minimum_required(VERSION 3.22)

project(PalmierProLinux
    VERSION 0.4.0
    DESCRIPTION "Palmier Pro — AI-native multi-track video editor (Linux port with GPU acceleration)"
    HOMEPAGE_URL "https://github.com/Bhaveshmeghwal21/palmier-pro-linux"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Language standard: C++20, required, no compiler extensions.
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Generate compile_commands.json for tooling (clang-tidy, IDEs, etc.).
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default to a reasonable build type when the user has not chosen one.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING
        "Choose the build type (Debug, Release, RelWithDebInfo, MinSizeRel)" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
endif()

# Position-independent code everywhere (Qt/shared libraries expect it).
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Make our custom CMake modules discoverable.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ---------------------------------------------------------------------------
# Build options.
# ---------------------------------------------------------------------------
include(PalmierOptions)

# ---------------------------------------------------------------------------
# Dependency discovery. Populates imported targets / variables and aborts
# configuration with a clear message naming every missing dependency.
# ---------------------------------------------------------------------------
include(PalmierDependencies)

# ---------------------------------------------------------------------------
# Testing. Enabled by default; can be turned off with -DPALMIER_BUILD_TESTS=OFF.
# ---------------------------------------------------------------------------
if(PALMIER_BUILD_TESTS)
    enable_testing()
    include(CTest)
endif()

# ---------------------------------------------------------------------------
# The editor tool surface's translation units.
#
# services/ToolRegistry.cpp used to be a single 2767-line file holding the
# registry container, every shared helper and all 42 tool factories in one
# anonymous namespace. It is now one translation unit per subject, so the surface
# is no longer a single file name a target can spell out.
#
# That matters here because the surface is compiled in two different ways. The
# palmier_services library compiles it as part of the library, and twenty-two
# standalone test targets compile the same sources DIRECTLY into the test binary
# instead of linking Palmier::services — deliberately, so those tests avoid the
# Qt/FFmpeg/libsecret/lcms2 dependencies the full library pulls in.
#
# Naming the list once, here, before the subdirectories that consume it, means
# adding a translation unit to the surface is a one-line change rather than
# twenty-three separate edits that can silently disagree with each other. A
# missing entry in any one of them is a link error, not a quiet omission.
# ---------------------------------------------------------------------------
set(PALMIER_TOOL_SURFACE_SOURCES
    "${PROJECT_SOURCE_DIR}/src/services/ToolRegistry.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ToolVocabulary.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ToolSerialization.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ToolArguments.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/TimelineClipTools.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/TimelineEffectTools.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/TimelineTextTools.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectTools.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/MediaTools.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/TrackTools.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/TimelineMarkerTools.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/GenerationExportTools.cpp")


# --------------------------------------------------------------------------
# The project store, split the same way and for the same reason.
# 
# services/ProjectStore.cpp was 1082 lines carrying the JSON parser, the JSON
# writer, the format vocabulary and every per-entity reader and writer. It is now
# one unit per subject sharing services/ProjectStoreInternal.hpp.
# 
# Twenty-six standalone test targets compile these sources directly rather than
# linking Palmier::services, so the list is named once here. Adding a unit is a
# one-line change instead of twenty-seven edits that can disagree.
# --------------------------------------------------------------------------
set(PALMIER_PROJECT_STORE_SOURCES
    "${PROJECT_SOURCE_DIR}/src/services/ProjectStore.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectStoreVocabulary.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectStoreWrite.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectStoreRead.cpp")


# --------------------------------------------------------------------------
# Export coordination, likewise: services/ExportCoordinator.cpp was 1080 lines
# holding the worker, the request validation, progress reporting and the agent
# tool surface. The shared internal contract is
# services/ExportCoordinatorInternal.hpp.
# --------------------------------------------------------------------------
set(PALMIER_EXPORT_COORDINATOR_SOURCES
    "${PROJECT_SOURCE_DIR}/src/services/ExportCoordinator.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ExportValidation.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ExportProgress.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ExportToolSurface.cpp")


# ---------------------------------------------------------------------------
# Module subdirectory layout.
#
#   src/core     — UI-agnostic domain core (types, project model, timeline engine)
#   src/media    — FFmpeg-based media engine (demux/decode/encode/audio)
#   src/gpu      — Vulkan GPU abstraction layer (compositor, effect kernels, HW bridge)
#   src/services — MCP server, project I/O, auth, generative AI client, agent chat
#   src/ui       — Qt 6 (QML) presentation layer
#   src/app      — application composition root / entry point
#   tests        — GoogleTest + RapidCheck unit and property tests
# ---------------------------------------------------------------------------
add_subdirectory(src/core)
add_subdirectory(src/media)
add_subdirectory(src/gpu)
add_subdirectory(src/services)
add_subdirectory(src/ui)
add_subdirectory(src/app)

if(PALMIER_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Configuration summary.
# ---------------------------------------------------------------------------
include(PalmierSummary)

# ---------------------------------------------------------------------------
# The `PALMIER_*` cache manifest read by the documentation consistency checker
# (task 12.7; Requirements 16.7, 16.8).
#
# Deliberately the LAST thing this file does: `tests/` contributes
# PALMIER_PBT_MIN_SUCCESS and PalmierDependencies contributes the six
# PALMIER_*_AVAILABLE internals, so any earlier call would emit an incomplete set
# and the checker would report perfectly well-documented options as undefined.
# ---------------------------------------------------------------------------
include(PalmierOptionsManifest)
palmier_write_options_manifest("${PROJECT_BINARY_DIR}/palmier_options.txt")

# ---------------------------------------------------------------------------
# Packaging (usable-editor tasks.md task 16.1, 16.3; Requirement 13.1, 13.4).
#
# Included unconditionally: `cpack` only does anything when actually invoked,
# and declaring the configuration here (rather than only when UI/tests are on)
# keeps `cpack --list-generators`/`cpack --help` well-defined on every configure.
# ---------------------------------------------------------------------------
include(PalmierPackaging)
