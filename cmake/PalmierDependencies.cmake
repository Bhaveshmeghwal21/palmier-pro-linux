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
# Discovered dependencies:
#   * Qt 6            (Core, Gui, Quick, Qml, Widgets)   — find_package
#   * FFmpeg / libav* (avformat avcodec avutil swscale swresample) — pkg-config
#   * Vulkan          (loader + headers)                 — find_package
#   * shaderc         (GLSL -> SPIR-V compilation)       — pkg-config / find_*
#   * libva           (VAAPI HW codec path)              — pkg-config [optional]
#   * oneVPL / libvpl (Intel QSV HW codec path)          — pkg-config [optional]
#   * ffnvcodec       (NVIDIA NVDEC/NVENC headers)       — pkg-config [optional]
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
# Vendor hardware codec paths (optional, gated by build options).
# ---------------------------------------------------------------------------
if(PALMIER_ENABLE_VAAPI)
    palmier_require_pkgconfig(LIBVA "libva (VAAPI)"
        "apt install libva-dev  |  dnf install libva-devel"
        libva
        libva-drm)
endif()

if(PALMIER_ENABLE_QSV)
    # Intel Quick Sync via oneVPL (new) or legacy Media SDK (mfx).
    pkg_check_modules(LIBVPL IMPORTED_TARGET vpl)
    if(NOT LIBVPL_FOUND)
        pkg_check_modules(LIBVPL IMPORTED_TARGET libmfx)
    endif()
    if(NOT LIBVPL_FOUND)
        list(APPEND _PALMIER_MISSING_DEPS
            "  - oneVPL / Intel Media SDK (QSV) [PALMIER_ENABLE_QSV=ON]\n      Install: apt install libvpl-dev  |  dnf install oneVPL-devel  (or set -DPALMIER_ENABLE_QSV=OFF)")
    endif()
endif()

if(PALMIER_ENABLE_NVENC)
    # NVIDIA codec headers (ffnvcodec) expose NVDEC/NVENC to FFmpeg.
    pkg_check_modules(FFNVCODEC IMPORTED_TARGET ffnvcodec)
    if(NOT FFNVCODEC_FOUND)
        list(APPEND _PALMIER_MISSING_DEPS
            "  - ffnvcodec headers (NVDEC/NVENC) [PALMIER_ENABLE_NVENC=ON]\n      Install: apt install nv-codec-headers  |  build nv-codec-headers from source  (or set -DPALMIER_ENABLE_NVENC=OFF)")
    endif()
endif()

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
        "Install the packages listed above and re-run CMake. Vendor hardware\n"
        "codec paths can be disabled individually if their SDKs are absent:\n"
        "  -DPALMIER_ENABLE_VAAPI=OFF  -DPALMIER_ENABLE_NVENC=OFF  -DPALMIER_ENABLE_QSV=OFF\n"
        "==================================================================\n")
endif()

message(STATUS "Palmier Pro Linux: all required dependencies located.")
