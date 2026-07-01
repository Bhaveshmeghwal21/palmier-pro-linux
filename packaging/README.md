# Packaging

Distributable packaging for Palmier Pro for Linux (Requirement 1.2). Three
formats are provided, all bundling/declaring the runtime dependency set — **Qt
6, FFmpeg (libav\*), the Vulkan loader, shaderc, libva, LittleCMS (lcms2), and
libsecret** — so the editor launches on a supported host (x86-64, glibc >=
2.31). Everything here is GPLv3-or-later, matching the editor and MCP server
(see the repository-root `LICENSE` and `NOTICE`).

## Layout

```
packaging/
├── shared/                              # assets shared across all three formats
│   ├── io.palmier.PalmierPro.desktop        # .desktop entry (Name=Palmier Pro)
│   ├── io.palmier.PalmierPro.metainfo.xml   # AppStream metadata (project_license GPL-3.0-or-later)
│   ├── io.palmier.PalmierPro.xml            # shared-mime-info for *.palmier
│   └── io.palmier.PalmierPro.svg            # scalable application icon
├── flatpak/
│   └── io.palmier.PalmierPro.yaml           # Flatpak manifest (PRIMARY)
├── appimage/
│   ├── AppImageBuilder.yml                  # appimage-builder recipe
│   └── build-appimage.sh                    # linuxdeploy + Qt plugin alternative
├── deb/
│   └── debian/                              # Debian source package (control/rules/…)
└── CPackConfig.cmake                        # standalone CPack .deb fallback
```

## Flatpak (primary)

```sh
flatpak-builder --user --install --force-clean \
    build-flatpak packaging/flatpak/io.palmier.PalmierPro.yaml
flatpak run io.palmier.PalmierPro
```

Uses the KDE runtime (ships Qt 6 + the Vulkan loader) and builds lcms2, libva,
shaderc, and FFmpeg as bundled modules. GPU access is granted via `--device=dri`.

## AppImage

```sh
cmake -S . -B build -DPALMIER_BUILD_UI=ON -DPALMIER_BUILD_TESTS=OFF
cmake --build build
appimage-builder --recipe packaging/appimage/AppImageBuilder.yml
# or, using linuxdeploy:
packaging/appimage/build-appimage.sh build
```

## Debian (.deb)

```sh
# From the repository root, with packaging/deb/debian symlinked or copied to ./debian:
dpkg-buildpackage -us -uc -b
```

The `debian/control` `Depends` field declares the runtime libraries; the build
depends pull in the `-dev` packages. A lightweight fallback that skips the full
Debian toolchain is available via CPack:

```sh
cmake --install build --prefix _pkgroot/usr
cpack --config packaging/CPackConfig.cmake
```

## Note on build tooling

`flatpak-builder`, `appimage-builder`/`appimagetool`, and `dpkg-buildpackage`
are not run here; these recipes are the source inputs to those tools. Manifest
and control syntax is validated (YAML/JSON/XML parsers, `xmllint`, and Debian
field checks) as part of the packaging task.
