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

# --- Optional TLS transport for the MCP endpoint ----------------------------
# Serves HTTPS on an opt-in remote MCP binding (Requirement 10.6) through
# OpenSSL 3.x. Detection is OPTIONAL: with the option ON but OpenSSL absent,
# configuration still succeeds, PALMIER_HAVE_OPENSSL stays undefined, and
# configuring TLS material becomes an unmet prerequisite that falls the endpoint
# back to a loopback bind. The endpoint itself is unaffected either way — it is
# loopback-only unless remote access is explicitly enabled.
option(PALMIER_ENABLE_OPENSSL
    "Enable the OpenSSL-backed TLS transport for the remote MCP endpoint" ON)

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


# --- Optional audio output backends (PipeWire / ALSA) -----------------------
# The audio engine's output sink is chosen at startup in the order
# PipeWire -> ALSA -> NullAudioSink (design.md D7). Both real backends are
# OPTIONAL in exactly the same sense as the vendor codec SDKs and OpenSSL: with
# the option ON but the library absent, configuration still succeeds,
# PALMIER_HAVE_PIPEWIRE / PALMIER_HAVE_ALSA stay undefined, and that sink reports
# "not compiled in" during selection so the order simply continues to the next
# candidate. With no backend available at all the always-compiled NullAudioSink is
# selected, audio is suppressed, video keeps running and a notice is raised
# (Requirement 6.7) — so a host with neither library builds, tests and runs.
#
# Licences: libpipewire-0.3 is MIT; libasound2 is LGPL-2.1-or-later. Both are
# compatible with this project's GPLv3.
option(PALMIER_ENABLE_PIPEWIRE
    "Enable the PipeWire audio output sink (libpipewire-0.3)" ON)

option(PALMIER_ENABLE_ALSA
    "Enable the ALSA audio output sink (libasound2)" ON)
