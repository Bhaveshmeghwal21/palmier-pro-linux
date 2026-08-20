# SPDX-License-Identifier: GPL-3.0-or-later
#
# Dependency discovery for Palmier Pro Linux.
#
# Discovers every third-party dependency the project needs using find_package
# where a CMake config/module exists and pkg-config otherwise. Rather than
# aborting on the first missing dependency, discovery accumulates ALL failures
# and reports a single, clear message that names every missing dependency along
# with an install hint, then fails configuration.
#
# The vendor hardware-codec SDKs (libva, oneVPL/libmfx, ffnvcodec) are the
# exception: they are optional. A missing vendor SDK only records
# PALMIER_<PATH>_AVAILABLE=OFF plus a status message, and configuration succeeds
# with that hardware path compiled out (Requirements 8.1, 8.9).
#
# Discovered dependencies:
#   * Qt 6            (Core, Gui, Quick, Qml, Widgets)   — find_package
#   * FFmpeg / libav* (avformat avcodec avutil swscale swresample) — pkg-config
#   * Vulkan          (loader + headers)                 — find_package
#   * shaderc         (GLSL -> SPIR-V compilation)       — pkg-config / find_*
#   * libva           (VAAPI HW codec path)              — pkg-config [optional,
#                                                          never fatal]
#   * oneVPL / libvpl (Intel QSV HW codec path)          — pkg-config [optional,
#                                                          never fatal]
#   * ffnvcodec       (NVIDIA NVDEC/NVENC headers)       — pkg-config [optional,
#                                                          never fatal]
#   * LittleCMS/lcms2 (color management)                 — pkg-config
#   * libsecret       (secure credential storage)        — pkg-config

include_guard(GLOBAL)

find_package(PkgConfig REQUIRED)

# List that accumulates human-readable descriptions of missing dependencies.
set(_PALMIER_MISSING_DEPS "")

# ---------------------------------------------------------------------------
# palmier_require_pkgconfig(<result-var> <pretty-name> <install-hint> <modules...>)
#
# Uses pkg-config to locate a dependency. On success defines an IMPORTED target
# named <result-var>. On failure appends a descriptive entry to the missing
# dependency list (configuration continues so every missing dep is reported).
# ---------------------------------------------------------------------------
function(palmier_require_pkgconfig result_var pretty_name install_hint)
    set(_modules ${ARGN})
    pkg_check_modules(${result_var} IMPORTED_TARGET ${_modules})
    if(NOT ${result_var}_FOUND)
        list(JOIN _modules ", " _mods_str)
        list(APPEND _PALMIER_MISSING_DEPS
            "  - ${pretty_name} (pkg-config: ${_mods_str})\n      Install: ${install_hint}")
        set(_PALMIER_MISSING_DEPS "${_PALMIER_MISSING_DEPS}" PARENT_SCOPE)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Qt 6 — required components for UI + application shell.
# ---------------------------------------------------------------------------
if(PALMIER_BUILD_UI)
    find_package(Qt6 6.2 QUIET COMPONENTS Core Gui Quick Qml Widgets)
    if(NOT Qt6_FOUND)
        list(APPEND _PALMIER_MISSING_DEPS
            "  - Qt 6 (>= 6.2, components: Core Gui Quick Qml Widgets)\n      Install: apt install qt6-base-dev qt6-declarative-dev  |  dnf install qt6-qtbase-devel qt6-qtdeclarative-devel")
    endif()
endif()

# ---------------------------------------------------------------------------
# FFmpeg (libav*) — media I/O, codecs, scaling, resampling.
# ---------------------------------------------------------------------------
palmier_require_pkgconfig(FFMPEG "FFmpeg (libav*)"
    "apt install libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev  |  dnf install ffmpeg-devel"
    libavformat
    libavcodec
    libavutil
    libswscale
    libswresample)

# ---------------------------------------------------------------------------
# Vulkan — GPU context, compositor, render targets.
# ---------------------------------------------------------------------------
find_package(Vulkan QUIET)
if(NOT Vulkan_FOUND)
    list(APPEND _PALMIER_MISSING_DEPS
        "  - Vulkan (loader + headers)\n      Install: apt install libvulkan-dev vulkan-tools  |  dnf install vulkan-loader-devel vulkan-headers")
endif()

# ---------------------------------------------------------------------------
# shaderc — compiles GLSL effect kernels to SPIR-V.
# Prefer pkg-config; fall back to plain library/header search.
# ---------------------------------------------------------------------------
pkg_check_modules(SHADERC IMPORTED_TARGET shaderc)
if(NOT SHADERC_FOUND)
    find_library(SHADERC_LIBRARY NAMES shaderc_shared shaderc_combined shaderc)
    find_path(SHADERC_INCLUDE_DIR NAMES shaderc/shaderc.h)
    if(SHADERC_LIBRARY AND SHADERC_INCLUDE_DIR)
        set(SHADERC_FOUND TRUE)
    else()
        list(APPEND _PALMIER_MISSING_DEPS
            "  - shaderc (GLSL -> SPIR-V compiler)\n      Install: apt install libshaderc-dev  |  dnf install libshaderc-devel  (or the Vulkan SDK)")
    endif()
endif()

# ---------------------------------------------------------------------------
# LittleCMS (lcms2) — color management.
# ---------------------------------------------------------------------------
palmier_require_pkgconfig(LCMS2 "LittleCMS (lcms2)"
    "apt install liblcms2-dev  |  dnf install lcms2-devel"
    lcms2)

# ---------------------------------------------------------------------------
# libsecret — secure credential (auth token / BYOK) storage.
# ---------------------------------------------------------------------------
palmier_require_pkgconfig(LIBSECRET "libsecret"
    "apt install libsecret-1-dev  |  dnf install libsecret-devel"
    libsecret-1)

# ---------------------------------------------------------------------------
# Vendor hardware codec paths — OPTIONAL (Requirements 8.1, 8.9).
#
# A vendor SDK that is requested (PALMIER_ENABLE_<PATH>=ON) but not installed is
# NOT a configuration failure: discovery records the miss, emits a status message
# naming the SDK and how to install it, and configuration continues with that
# hardware path compiled out. Only the software (FFmpeg/CPU) path is mandatory.
#
# Contract for consumers (src/media, src/gpu, cmake/PalmierSummary.cmake, tests):
#
#   PALMIER_VAAPI_AVAILABLE  ON | OFF
#   PALMIER_QSV_AVAILABLE    ON | OFF
#   PALMIER_NVENC_AVAILABLE  ON | OFF
#
#   Each is ON if and only if BOTH the corresponding PALMIER_ENABLE_* option is
#   ON *and* that vendor's SDK was located at configure time. It is OFF when the
#   option is OFF (no lookup is attempted) and OFF when the option is ON but the
#   SDK is missing. So a consumer that wants the "ENABLE AND FOUND" gate can test
#   PALMIER_*_AVAILABLE alone, e.g.
#
#       if(PALMIER_VAAPI_AVAILABLE)
#           target_link_libraries(... PkgConfig::LIBVA)
#           target_compile_definitions(... PALMIER_HAVE_VAAPI=1)
#       endif()
#
#   All three are always defined (never left unset) and are stored as
#   CACHE INTERNAL, so they are readable from any directory or function scope and
#   are recomputed (INTERNAL implies FORCE) on every configure run.
#
#   To distinguish "disabled by option" from "SDK not found" — which the
#   configuration summary needs — combine with PALMIER_ENABLE_<PATH>:
#       option OFF                        -> disabled (option OFF)
#       option ON  and AVAILABLE ON       -> enabled (SDK found)
#       option ON  and AVAILABLE OFF      -> disabled (SDK not found)
#
#   The pkg-config prefixes LIBVA / LIBVPL / FFNVCODEC (and their imported
#   targets PkgConfig::LIBVA / PkgConfig::LIBVPL / PkgConfig::FFNVCODEC) are also
#   populated on a hit. The lookups are deliberately performed at this file's
#   top-level scope (not inside a helper function) so that LIBVA_FOUND,
#   LIBVPL_FOUND and FFNVCODEC_FOUND are visible to the src/* subdirectories.
# ---------------------------------------------------------------------------

# --- VAAPI (Intel / AMD via libva) -----------------------------------------
set(_palmier_vaapi_available OFF)
if(PALMIER_ENABLE_VAAPI)
    pkg_check_modules(LIBVA IMPORTED_TARGET libva libva-drm)
    if(LIBVA_FOUND)
        set(_palmier_vaapi_available ON)
        # LIBVA_VERSION is empty for a multi-module check; report libva's own version.
        message(STATUS "Palmier: VAAPI hardware codec path ENABLED — libva ${LIBVA_libva_VERSION} found.")
    else()
        message(STATUS "Palmier: VAAPI hardware codec path DISABLED — libva/libva-drm not found "
                       "(install: apt install libva-dev | dnf install libva-devel, "
                       "or configure with -DPALMIER_ENABLE_VAAPI=OFF). "
                       "Configuration continues; encoding falls back to the software path.")
    endif()
else()
    message(STATUS "Palmier: VAAPI hardware codec path DISABLED — PALMIER_ENABLE_VAAPI=OFF.")
endif()
set(PALMIER_VAAPI_AVAILABLE ${_palmier_vaapi_available} CACHE INTERNAL
    "VAAPI hardware codec path is enabled AND its SDK (libva) was found")

# --- QSV (Intel Quick Sync via oneVPL, or the legacy Media SDK) -------------
set(_palmier_qsv_available OFF)
if(PALMIER_ENABLE_QSV)
    # Intel Quick Sync via oneVPL (new) or legacy Media SDK (mfx).
    pkg_check_modules(LIBVPL IMPORTED_TARGET vpl)
    if(NOT LIBVPL_FOUND)
        pkg_check_modules(LIBVPL IMPORTED_TARGET libmfx)
    endif()
    if(LIBVPL_FOUND)
        set(_palmier_qsv_available ON)
        message(STATUS "Palmier: QSV hardware codec path ENABLED — oneVPL/Media SDK ${LIBVPL_VERSION} found.")
    else()
        message(STATUS "Palmier: QSV hardware codec path DISABLED — oneVPL (vpl) / Intel Media SDK (libmfx) "
                       "not found (install: apt install libvpl-dev | dnf install oneVPL-devel, "
                       "or configure with -DPALMIER_ENABLE_QSV=OFF). "
                       "Configuration continues; encoding falls back to the software path.")
    endif()
else()
    message(STATUS "Palmier: QSV hardware codec path DISABLED — PALMIER_ENABLE_QSV=OFF.")
endif()
set(PALMIER_QSV_AVAILABLE ${_palmier_qsv_available} CACHE INTERNAL
    "QSV hardware codec path is enabled AND its SDK (oneVPL/libmfx) was found")

# --- NVENC / NVDEC (NVIDIA codec headers consumed by FFmpeg) ----------------
set(_palmier_nvenc_available OFF)
if(PALMIER_ENABLE_NVENC)
    # NVIDIA codec headers (ffnvcodec) expose NVDEC/NVENC to FFmpeg.
    pkg_check_modules(FFNVCODEC IMPORTED_TARGET ffnvcodec)
    if(FFNVCODEC_FOUND)
        set(_palmier_nvenc_available ON)
        message(STATUS "Palmier: NVENC/NVDEC hardware codec path ENABLED — ffnvcodec ${FFNVCODEC_VERSION} found.")
    else()
        message(STATUS "Palmier: NVENC/NVDEC hardware codec path DISABLED — ffnvcodec headers not found "
                       "(install: apt install nv-codec-headers, or build nv-codec-headers from source, "
                       "or configure with -DPALMIER_ENABLE_NVENC=OFF). "
                       "Configuration continues; encoding falls back to the software path.")
    endif()
else()
    message(STATUS "Palmier: NVENC/NVDEC hardware codec path DISABLED — PALMIER_ENABLE_NVENC=OFF.")
endif()
set(PALMIER_NVENC_AVAILABLE ${_palmier_nvenc_available} CACHE INTERNAL
    "NVENC/NVDEC hardware codec path is enabled AND its SDK (ffnvcodec) was found")

# ---------------------------------------------------------------------------
# OpenSSL 3.x — the optional TLS transport for the remote MCP endpoint
# (Requirement 10.6; design.md D4). Optional in exactly the same sense as the
# vendor codec SDKs: a miss records PALMIER_OPENSSL_AVAILABLE=OFF and a status
# message, and configuration succeeds with the TLS path compiled out. Code behind
# the absent PALMIER_HAVE_OPENSSL guard degrades to a working fallback —
# configuring TLS becomes an unmet prerequisite and the gate binds loopback — so
# a host with no OpenSSL configures, builds and tests exactly as before.
#
# OpenSSL is Apache-2.0 from 3.0 onward, which is compatible with GPLv3; the 1.x
# series is not, so 3.0 is the floor rather than a preference.
# ---------------------------------------------------------------------------
set(_palmier_openssl_available OFF)
if(PALMIER_ENABLE_OPENSSL)
    pkg_check_modules(OPENSSL IMPORTED_TARGET "openssl >= 3.0")
    if(OPENSSL_FOUND)
        set(_palmier_openssl_available ON)
        message(STATUS "Palmier: TLS transport ENABLED — OpenSSL ${OPENSSL_VERSION} found.")
    else()
        message(STATUS "Palmier: TLS transport DISABLED — OpenSSL 3.x not found "
                       "(install: apt install libssl-dev | dnf install openssl-devel, "
                       "or configure with -DPALMIER_ENABLE_OPENSSL=OFF). "
                       "Configuration continues; the MCP endpoint serves plaintext and "
                       "configured TLS material becomes an unmet prerequisite.")
    endif()
else()
    message(STATUS "Palmier: TLS transport DISABLED — PALMIER_ENABLE_OPENSSL=OFF.")
endif()
set(PALMIER_OPENSSL_AVAILABLE ${_palmier_openssl_available} CACHE INTERNAL
    "The TLS transport is enabled AND OpenSSL 3.x was found")

# ---------------------------------------------------------------------------
# Audio output backends — OPTIONAL (task 8.6; Requirements 6.2, 6.7).
#
# The audio sink is selected at startup in the order PipeWire -> ALSA ->
# NullAudioSink (design.md D7). Both real backends follow exactly the same
# "ENABLE AND FOUND" contract as the vendor codec SDKs above:
#
#   PALMIER_PIPEWIRE_AVAILABLE  ON | OFF
#   PALMIER_ALSA_AVAILABLE      ON | OFF
#
#   ON if and only if the corresponding PALMIER_ENABLE_* option is ON *and* the
#   library was located at configure time. A miss is never fatal: it records OFF
#   plus a status message, that sink is compiled out, and selection falls through
#   to the next candidate and ultimately to the always-compiled NullAudioSink, so
#   a host with neither library configures, builds and tests exactly as before
#   (Requirement 6.7).
#
#   Both are stored as CACHE INTERNAL so they are readable from any directory or
#   function scope and are recomputed on every configure run, and — as with the
#   vendor SDKs — the pkg_check_modules lookups are performed at this file's
#   TOP-LEVEL scope rather than inside a helper function, so LIBPIPEWIRE_FOUND and
#   LIBASOUND_FOUND (and the imported targets PkgConfig::LIBPIPEWIRE /
#   PkgConfig::LIBASOUND) are visible to the src/* subdirectories. A
#   function-scoped lookup would leave <prefix>_FOUND invisible to callers, which
#   is the exact defect stage 0 of this feature existed to remove.
#
#   The ALSA pkg-config prefix is deliberately LIBASOUND, not ALSA: CMake ships a
#   FindALSA module that publishes ALSA_FOUND with different semantics, and
#   colliding with it would make the gate below mean something other than what it
#   says.
# ---------------------------------------------------------------------------

# --- PipeWire (the primary sink; MIT) ---------------------------------------
set(_palmier_pipewire_available OFF)
if(PALMIER_ENABLE_PIPEWIRE)
    pkg_check_modules(LIBPIPEWIRE IMPORTED_TARGET libpipewire-0.3)
    if(LIBPIPEWIRE_FOUND)
        set(_palmier_pipewire_available ON)
        message(STATUS "Palmier: PipeWire audio sink ENABLED — libpipewire-0.3 ${LIBPIPEWIRE_VERSION} found.")
    else()
        message(STATUS "Palmier: PipeWire audio sink DISABLED — libpipewire-0.3 not found "
                       "(install: apt install libpipewire-0.3-dev | dnf install pipewire-devel, "
                       "or configure with -DPALMIER_ENABLE_PIPEWIRE=OFF). "
                       "Configuration continues; audio output selection falls through to ALSA "
                       "and then to the null sink.")
    endif()
else()
    message(STATUS "Palmier: PipeWire audio sink DISABLED — PALMIER_ENABLE_PIPEWIRE=OFF.")
endif()
set(PALMIER_PIPEWIRE_AVAILABLE ${_palmier_pipewire_available} CACHE INTERNAL
    "The PipeWire audio sink is enabled AND libpipewire-0.3 was found")

# --- ALSA (the fallback sink; LGPL-2.1-or-later) -----------------------------
set(_palmier_alsa_available OFF)
if(PALMIER_ENABLE_ALSA)
    pkg_check_modules(LIBASOUND IMPORTED_TARGET alsa)
    if(LIBASOUND_FOUND)
        set(_palmier_alsa_available ON)
        message(STATUS "Palmier: ALSA audio sink ENABLED — libasound2 ${LIBASOUND_VERSION} found.")
    else()
        message(STATUS "Palmier: ALSA audio sink DISABLED — libasound2 (pkg-config: alsa) not found "
                       "(install: apt install libasound2-dev | dnf install alsa-lib-devel, "
                       "or configure with -DPALMIER_ENABLE_ALSA=OFF). "
                       "Configuration continues; audio output selection falls through to the "
                       "null sink.")
    endif()
else()
    message(STATUS "Palmier: ALSA audio sink DISABLED — PALMIER_ENABLE_ALSA=OFF.")
endif()
set(PALMIER_ALSA_AVAILABLE ${_palmier_alsa_available} CACHE INTERNAL
    "The ALSA audio sink is enabled AND libasound2 was found")

# ---------------------------------------------------------------------------
# Report all missing dependencies at once and fail configuration clearly.
# ---------------------------------------------------------------------------
if(_PALMIER_MISSING_DEPS)
    list(JOIN _PALMIER_MISSING_DEPS "\n" _missing_str)
    message(FATAL_ERROR
        "\n"
        "==================================================================\n"
        " Palmier Pro Linux: cannot configure — missing required dependencies\n"
        "==================================================================\n"
        "${_missing_str}\n"
        "------------------------------------------------------------------\n"
        "Install the packages listed above and re-run CMake. Only the software\n"
        "(FFmpeg/CPU) media path is required — a missing vendor hardware codec\n"
        "SDK (libva / oneVPL / ffnvcodec) never appears here and never blocks\n"
        "configuration; that path is simply compiled out.\n"
        "==================================================================\n")
endif()

message(STATUS "Palmier Pro Linux: all required dependencies located.")
message(STATUS "Palmier Pro Linux: TLS transport (OpenSSL) — "
               "OPENSSL=${PALMIER_OPENSSL_AVAILABLE}")
message(STATUS "Palmier Pro Linux: audio output sinks — "
               "PIPEWIRE=${PALMIER_PIPEWIRE_AVAILABLE} "
               "ALSA=${PALMIER_ALSA_AVAILABLE} "
               "(NullAudioSink is always available)")
message(STATUS "Palmier Pro Linux: hardware codec paths — "
               "VAAPI=${PALMIER_VAAPI_AVAILABLE} "
               "QSV=${PALMIER_QSV_AVAILABLE} "
               "NVENC=${PALMIER_NVENC_AVAILABLE}")
