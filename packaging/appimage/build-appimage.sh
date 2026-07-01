#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Palmier Pro for Linux -- AppImage build script (linuxdeploy + Qt plugin).
# Copyright (C) 2024 Palmier, Inc. and contributors
#
# Alternative to the appimage-builder recipe: stages the installed tree into an
# AppDir and lets linuxdeploy bundle the runtime dependencies (Qt 6, FFmpeg,
# Vulkan loader, shaderc, libva, lcms2, libsecret) plus their transitive libs.
#
# Prerequisites (fetched to the working dir if missing):
#   - linuxdeploy-x86_64.AppImage
#   - linuxdeploy-plugin-qt-x86_64.AppImage
#
# Usage:
#   ./packaging/appimage/build-appimage.sh /path/to/cmake-build-dir

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${1:-${REPO_ROOT}/build}"
APPDIR="${REPO_ROOT}/AppDir"
SHARED="${REPO_ROOT}/packaging/shared"

echo ">> Installing build into AppDir: ${APPDIR}"
rm -rf "${APPDIR}"
cmake --install "${BUILD_DIR}" --prefix "${APPDIR}/usr"

echo ">> Staging desktop entry, icon, AppStream metadata, and MIME type"
install -Dm644 "${SHARED}/io.palmier.PalmierPro.desktop" \
    "${APPDIR}/usr/share/applications/io.palmier.PalmierPro.desktop"
install -Dm644 "${SHARED}/io.palmier.PalmierPro.svg" \
    "${APPDIR}/usr/share/icons/hicolor/scalable/apps/io.palmier.PalmierPro.svg"
install -Dm644 "${SHARED}/io.palmier.PalmierPro.metainfo.xml" \
    "${APPDIR}/usr/share/metainfo/io.palmier.PalmierPro.metainfo.xml"
install -Dm644 "${SHARED}/io.palmier.PalmierPro.xml" \
    "${APPDIR}/usr/share/mime/packages/io.palmier.PalmierPro.xml"

# Ship the GPLv3 license text alongside the bundled binaries (source offer).
install -Dm644 "${REPO_ROOT}/LICENSE" \
    "${APPDIR}/usr/share/doc/palmier-pro/LICENSE"

echo ">> Running linuxdeploy with the Qt plugin"
export QML_SOURCES_PATHS="${REPO_ROOT}/src/ui"
./linuxdeploy-x86_64.AppImage \
    --appdir "${APPDIR}" \
    --plugin qt \
    --desktop-file "${APPDIR}/usr/share/applications/io.palmier.PalmierPro.desktop" \
    --icon-file "${SHARED}/io.palmier.PalmierPro.svg" \
    --output appimage

echo ">> Done: Palmier_Pro-*-x86_64.AppImage"
