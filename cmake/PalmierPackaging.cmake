#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# CPack packaging configuration for Palmier Pro Linux.
#
# usable-editor tasks.md task 16.1 (Requirement 13.1): "publish at least one
# self-contained installable artifact for a documented set of distributions,
# bundling or declaring every runtime dependency the platform check requires."
#
# A Debian package (.deb) is the artifact this project produces: CI already
# builds and tests on ubuntu-24.04 (docs/BUILD.md's own "direct evidence"
# column), and docs/BUILD.md documents Ubuntu 22.04+/Debian 12+ as one of the
# supported distribution families this Requirement asks for "a documented set
# of" — not every family at once.
#
# CPACK_DEBIAN_PACKAGE_SHLIBDEPS=ON runs `dpkg-shlibdeps` over the installed
# palmier-pro binary, which inspects its actual ELF NEEDED entries and writes
# the package's Depends: field from the real runtime library packages the
# binary links against (Qt 6, FFmpeg, Vulkan, shaderc, lcms2, libsecret) — the
# same shared libraries PlatformCompatibility's launch gate independently
# dlopen()s and names by hand if missing (task 16.4; already covered by
# tests/app/platform_compatibility_test.cpp's MissingSingleDependencyIsNamed /
# EveryMissingDependencyIsNamedIndividually, predating this spec). Declaring
# them via dpkg's own dependency resolution means apt/dpkg refuses to install
# the package at all when a dependency is absent, and PlatformCompatibility's
# named-by-item message remains the second, independent line of defence for a
# host whose package manager was bypassed (a raw copy of the binary, or a
# runtime library removed after installation).
#
# LICENSE and NOTICE are NOT listed again here: src/app/CMakeLists.txt already
# installs both into share/doc/palmier-pro alongside the executable (task
# 16.3), and CPack packages whatever install() placed in the staging tree.
#
include_guard(GLOBAL)

set(CPACK_PACKAGE_NAME "palmier-pro")
set(CPACK_PACKAGE_VENDOR "Palmier, Inc. and contributors")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
# A plain-ASCII summary, deliberately not PROJECT_DESCRIPTION (which carries an
# em dash): Debian control file fields are conventionally single-line ASCII,
# and dpkg-deb/lintian warn on stray encoding in Description:.
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "AI-native multi-track video editor (Linux port with GPU acceleration)")
set(CPACK_PACKAGE_HOMEPAGE_URL "${PROJECT_HOMEPAGE_URL}")
set(CPACK_PACKAGE_CONTACT "https://github.com/palmier-io/palmier-pro-linux/issues")
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")

set(CPACK_GENERATOR "DEB")
set(CPACK_DEBIAN_PACKAGE_SECTION "video")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_CONTACT}")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${CPACK_PACKAGE_HOMEPAGE_URL}")

# Requirement 13.1's "bundling or declaring every runtime dependency": declare,
# via dpkg's own real dependency resolution against the binary CPack just
# staged, rather than a hand-maintained list that can drift from what the
# binary actually links (docs/BUILD.md's "-dev" package table is for BUILDING,
# not the runtime package names a Depends: field needs, which differ by distro
# release — exactly what shlibdeps looks up instead of guessing).
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)

# Architecture-qualified filename (e.g. palmier-pro_0.1.0_amd64.deb) so the
# launch smoke test's packaging job (ci.yml) can name it deterministically
# without discovering it by glob.
set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")

include(CPack)
