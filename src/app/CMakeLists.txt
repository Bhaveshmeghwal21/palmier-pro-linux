# SPDX-License-Identifier: GPL-3.0-or-later
#
# src/app — application composition root / entry point.
#
# This module builds two things:
#
#   1. palmier_app_platform (Palmier::app_platform) — a Qt-free static library
#      holding the launch-time platform compatibility checker (task 19.1;
#      Requirements 1.1, 1.4, 1.5, 13.3, 13.4). Its verdict logic is pure and
#      dependency-free, so it is compiled ALWAYS — including in build/test
#      environments (such as CI or the sandbox) where Qt 6 is not installed —
#      and its unit tests link only this library. The real host probe uses
#      dlopen(3), so the platform's dl library is linked when one is required.
#
#   2. palmier-pro — the Qt 6 editor executable (entry point in main.cpp). It is
#      built ONLY when the UI is enabled and Qt 6 was found, so a missing Qt does
#      not break configuration. main.cpp runs the Qt-free checker first and, on
#      an unsupported platform / missing dependency, shows a message naming each
#      unmet item and exits without constructing the editor (Requirements 1.4,
#      1.5); otherwise it shows the MainWindow (Requirement 1.3). PALMIER_HAVE_QT
#      is defined for this target so main.cpp compiles its Qt path.
#
# The application is fully wired together (all components composed) by task 21.1.

# ---------------------------------------------------------------------------
# 1. Qt-free platform compatibility checker — always built.
# ---------------------------------------------------------------------------
add_library(palmier_app_platform STATIC
    PlatformCompatibility.cpp)
add_library(Palmier::app_platform ALIAS palmier_app_platform)

target_include_directories(palmier_app_platform PUBLIC
    "${PROJECT_SOURCE_DIR}/src")

target_compile_features(palmier_app_platform PUBLIC cxx_std_20)

target_link_libraries(palmier_app_platform PUBLIC
    Palmier::core)

# The real host probe loads candidate shared libraries with dlopen(3); link the
# platform's dl library (empty on platforms where dlopen lives in libc).
if(CMAKE_DL_LIBS)
    target_link_libraries(palmier_app_platform PRIVATE ${CMAKE_DL_LIBS})
endif()

if(PALMIER_WERROR)
    target_compile_options(palmier_app_platform PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic -Werror>)
endif()

# ---------------------------------------------------------------------------
# 2. Qt 6 editor executable — built only when the UI is enabled and Qt is found.
# ---------------------------------------------------------------------------
if(PALMIER_BUILD_UI AND TARGET Qt6::Widgets)
    add_executable(palmier-pro
        main.cpp)

    target_compile_definitions(palmier-pro PRIVATE PALMIER_HAVE_QT=1)

    target_link_libraries(palmier-pro PRIVATE
        Palmier::app_platform
        Palmier::ui_shell
        Palmier::core
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets)

    set_target_properties(palmier-pro PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

    if(PALMIER_WERROR)
        target_compile_options(palmier-pro PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic -Werror>)
    endif()
endif()



# ---------------------------------------------------------------------------
# 3. Task 21.1: application composition root — Qt-free wiring library.
# ---------------------------------------------------------------------------
#
# ApplicationComposition constructs and connects every component of the
# application (GpuContext with software fallback, TimelineEngine, the media
# library, project I/O, auth + BYOK, the generative client + coordinator, the
# shared ToolRegistry, the McpToolExecutor, the McpServer, the agent
# orchestrator, and localization) and owns the MCP-server-on-launch /
# stop-on-close lifecycle (Requirements 1.6, 7.1, 7.2, 7.9, 13.3).
#
# The wiring is deliberately Qt-free so it builds and is unit-tested on hosts
# without Qt/FFmpeg/Vulkan/shaderc/lcms2/libsecret (the vendor paths degrade to
# software/stub behind the project's guard macros). Only main.cpp keeps the thin
# Qt entry glue (behind PALMIER_HAVE_QT). Declared with a UNIQUE target name and
# APPENDED here so the palmier_app_platform and palmier-pro targets defined above
# are preserved verbatim.
#
# AppSettings.cpp (task 6.1; Requirements 10.2, 16.3) joins the same library: it
# resolves the AppConfig this library consumes from built-in defaults, the
# key=value file at $XDG_CONFIG_HOME/palmier-pro/config, the environment and the
# command line, and has no dependency the composition root does not already have.
add_library(palmier_app_composition STATIC
    ApplicationComposition.cpp
    AppSettings.cpp)
add_library(Palmier::app_composition ALIAS palmier_app_composition)

target_include_directories(palmier_app_composition PUBLIC
    "${PROJECT_SOURCE_DIR}/src")

target_compile_features(palmier_app_composition PUBLIC cxx_std_20)

# Task 7.5 adds the Playback_Engine of Requirement 1.1 to the composition: the
# single gpu::Compositor, the single media::DecoderTeardownQueue, the single
# media::DecoderClipFrameProvider and the single ui::PreviewController transport.
# Palmier::media supplies the decode side, and Palmier::ui_preview_vm the Qt-free
# transport (that target is defined above src/ui's PALMIER_BUILD_UI guard exactly
# so this Qt-free library can link it in both build trees).
target_link_libraries(palmier_app_composition PUBLIC
    Palmier::core
    Palmier::gpu
    Palmier::media
    Palmier::ui_preview_vm
    Palmier::services)

if(PALMIER_WERROR)
    target_compile_options(palmier_app_composition PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic -Werror>)
endif()

# Complete the other half of the MainWindow <-> ApplicationComposition link
# (task 11.2): src/ui/CMakeLists.txt defines palmier_ui_shell (MainWindow,
# TimelinePanel, ExportDialog) WITHOUT linking Palmier::app_composition, because
# src/ui is added before src/app in the top-level CMakeLists.txt and
# app_composition does not exist yet at that point. Extending an existing
# target's link interface from a later CMakeLists.txt (after the referenced
# target has been created) is ordinary, supported CMake, and keeps the existing
# subdirectory order unchanged.
if(TARGET palmier_ui_shell)
    target_link_libraries(palmier_ui_shell PUBLIC Palmier::app_composition)
endif()

# Link the composition root into the editor executable. Appended (guarded by a
# TARGET check) so the existing palmier-pro target definition above is not
# modified or reordered; main.cpp constructs the composition after the
# platform-compatibility check passes, starts the MCP server on launch, and stops
# it on close.
if(TARGET palmier-pro)
    target_link_libraries(palmier-pro PRIVATE Palmier::app_composition)
endif()
