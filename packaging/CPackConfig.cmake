# SPDX-License-Identifier: GPL-3.0-or-later
#
# Palmier Pro for Linux -- standalone CPack configuration (.deb generator).
# Copyright (C) 2024 Palmier, Inc. and contributors
#
# This file is intentionally standalone and is NOT wired into the root
# CMakeLists.txt, so it does not modify or depend on any existing build target.
# It offers a convenience path to produce a .deb directly from an installed
# build tree without the full Debian toolchain:
#
#   cmake --install build --prefix _pkgroot/usr
#   cpack --config packaging/CPackConfig.cmake
#
# The canonical, policy-compliant Debian package is still produced from
# packaging/deb/debian/ via dpkg-buildpackage; this is a lightweight fallback.

set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_NAME "palmier-pro")
set(CPACK_PACKAGE_VERSION "0.1.0")
set(CPACK_PACKAGE_CONTACT "Palmier, Inc. and contributors <maintainers@palmier.io>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "AI-native multi-track video editor with GPU acceleration")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/palmier-io/palmier-pro")

set(CPACK_DEBIAN_PACKAGE_SECTION "video")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")

# Runtime dependencies bundled/declared per Requirement 1.2. Qt 6, FFmpeg
# (libav*), the Vulkan loader, shaderc, libva, lcms2, and libsecret.
set(CPACK_DEBIAN_PACKAGE_DEPENDS
    "libqt6core6, libqt6gui6, libqt6widgets6, libqt6quick6, libqt6qml6, \
libavcodec58 | libavcodec59 | libavcodec60, \
libavformat58 | libavformat59 | libavformat60, \
libavutil56 | libavutil57 | libavutil58, \
libswscale5 | libswscale6 | libswscale7, \
libswresample3 | libswresample4 | libswresample5, \
libvulkan1, libshaderc1, libva2, libva-drm2, liblcms2-2, libsecret-1-0")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "mesa-vulkan-drivers, va-driver-all")

# Enable shared-library dependency detection to complement the explicit list.
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)

# The staged install tree (usr/...) produced by `cmake --install`.
set(CPACK_INSTALLED_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/../_pkgroot;/")
