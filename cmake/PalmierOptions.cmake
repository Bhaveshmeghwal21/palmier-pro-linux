# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build options for Palmier Pro Linux.
#
# Vendor hardware-codec paths can be independently enabled or disabled so the
# project can be built on machines that lack a given vendor's SDK/driver headers,
# or to produce vendor-specific distribution packages.

include_guard(GLOBAL)

# --- General build options -------------------------------------------------
option(PALMIER_BUILD_TESTS
    "Build the GoogleTest + RapidCheck unit and property tests" ON)

option(PALMIER_BUILD_UI
    "Build the Qt 6 UI presentation layer and application shell" ON)

option(PALMIER_WERROR
    "Treat compiler warnings as errors" OFF)

# --- Vendor hardware codec paths (VAAPI / NVENC / QSV) ---------------------
# These control which hardware-accelerated decode/encode backends are compiled
# in. When all are OFF the build still produces a fully functional editor that
# uses the FFmpeg software (CPU) decode/encode path — matching the design's
# graceful-degradation requirement.
option(PALMIER_ENABLE_VAAPI
    "Enable the VAAPI hardware codec path (Intel / AMD via libva)" ON)

option(PALMIER_ENABLE_NVENC
    "Enable the NVIDIA hardware codec path (NVDEC / NVENC)" ON)

option(PALMIER_ENABLE_QSV
    "Enable the Intel Quick Sync Video hardware codec path (oneVPL/QSV)" ON)

# Convenience: is any vendor HW codec path enabled?
if(PALMIER_ENABLE_VAAPI OR PALMIER_ENABLE_NVENC OR PALMIER_ENABLE_QSV)
    set(PALMIER_ANY_HW_CODEC ON)
else()
    set(PALMIER_ANY_HW_CODEC OFF)
endif()
