<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Building Palmier Pro for Linux

This is the Requirement 16.1 build document: the complete configure → build → test → launch
sequence from a clean checkout, the native package names per supported distribution family, every
`PALMIER_*` CMake cache option with its default and effect, and the minimum host specification.

It is a **checked** document. Task 12.7's consistency checker emits the live `PALMIER_*` cache
variables at configure time and compares them two-way against the option tables below
(Requirements 16.7, 16.8), so an option name here that the build system does not define — or an
option the build system defines that is missing here — fails the Verification_Suite. The option
names below were read out of `cmake/PalmierOptions.cmake`, `cmake/PalmierDependencies.cmake` and
`tests/CMakeLists.txt` rather than transcribed from prose.

## Minimum host specification (Requirement 16.1)

| Item | Minimum | Where it comes from |
|---|---|---|
| Processor | x86-64 or AArch64, 4 cores | `app::checkPlatformCompatibility` accepts only `x86_64` and `aarch64`; the core count is a build-throughput recommendation, not an enforced gate |
| Memory | 8 GB RAM (4 GB for a `PALMIER_BUILD_UI=OFF` build) | recommendation; nothing in the tree measures it |
| GPU and driver | any device with a Vulkan 1.1 driver reachable through `libvulkan.so.1` | `gpu::GpuContext` requests `VK_API_VERSION_1_1`. **No GPU is strictly required**: an absent or incompatible device yields the software fallback plus the `gpuUnavailableNotice` in the status bar (Requirement 1.6), and the editor still opens all five panels |
| C library | glibc 2.31 or later | `CompatibilityRequirements::minGlibc = {2, 31}`; an undetectable glibc fails the gate conservatively |
| Distribution | Ubuntu 22.04+, Debian 12+, Fedora 38+, RHEL/Alma/Rocky 9+, or Arch (rolling) | glibc ≥ 2.31 and Qt ≥ 6.2 together. CI runs `ubuntu-24.04` |
| Toolchain | CMake ≥ 3.22, a C++20 compiler (GCC 11+ or Clang 14+), `pkg-config` | `cmake_minimum_required(VERSION 3.22)`, `CMAKE_CXX_STANDARD 20`, `find_package(PkgConfig REQUIRED)` |
| Runtime shared libraries | `libQt6Core.so.6`, `libQt6Gui.so.6`, `libQt6Widgets.so.6`, `libavcodec.so`, `libavformat.so`, `libavutil.so`, `libvulkan.so.1`, `liblcms2.so.2`, `libsecret-1.so.0` | the launch gate `dlopen`s each of these and refuses to construct the editor while any is unloadable, naming each missing item |

The memory and core-count figures are the only rows above that are **not** derived from code or
from the requirements: nothing in the tree asserts them. Treat them as a recommendation and correct
them if a measured figure is ever established.

Vendor hardware-codec libraries are deliberately **not** in the launch gate — their absence must
never block startup, because encoding degrades to the software path.

## Native packages per distribution family (Requirement 16.1)

Required for every build. The Debian/Ubuntu column is the set CI installs, so it is the one with
direct evidence behind it.

| Dependency | Debian / Ubuntu | Fedora / RHEL | Arch |
|---|---|---|---|
| Toolchain | `build-essential cmake ninja-build pkg-config` | `gcc-c++ cmake ninja-build pkgconf-pkg-config` | `base-devel cmake ninja` |
| Qt 6 (Core, Gui, Quick, Qml, Widgets) | `qt6-base-dev qt6-base-dev-tools qt6-declarative-dev qt6-tools-dev qt6-tools-dev-tools` | `qt6-qtbase-devel qt6-qtdeclarative-devel qt6-qttools-devel` | `qt6-base qt6-declarative qt6-tools` |
| QML runtime modules (UI build) | `qml6-module-qtquick qml6-module-qtquick-controls qml6-module-qtquick-window qml6-module-qtqml-workerscript` | `qt6-qtdeclarative` | `qt6-declarative` |
| FFmpeg | `libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev` | `ffmpeg-devel` (RPM Fusion) | `ffmpeg` |
| Vulkan | `libvulkan-dev vulkan-tools` | `vulkan-loader-devel vulkan-headers vulkan-tools` | `vulkan-icd-loader vulkan-headers vulkan-tools` |
| shaderc | `libshaderc-dev` | `libshaderc-devel` | `shaderc` |
| LittleCMS | `liblcms2-dev` | `lcms2-devel` | `lcms2` |
| libsecret | `libsecret-1-dev` | `libsecret-devel` | `libsecret` |
| OpenGL / GLX headers (needed for `find_package(Qt6)` — see the traps below) | `libgl1-mesa-dev libglu1-mesa-dev` | `mesa-libGL-devel mesa-libEGL-devel libglvnd-devel` | `mesa libglvnd` |

Optional, each behind its own `PALMIER_ENABLE_*` option. A missing one is never a configuration
failure; the corresponding path is compiled out and a documented fallback takes over.

| Optional dependency | Debian / Ubuntu | Fedora / RHEL | Arch |
|---|---|---|---|
| OpenSSL 3.x (TLS for remote MCP) | `libssl-dev` | `openssl-devel` | `openssl` |
| PipeWire (primary audio sink) | `libpipewire-0.3-dev` | `pipewire-devel` | `libpipewire` |
| ALSA (fallback audio sink) | `libasound2-dev` | `alsa-lib-devel` | `alsa-lib` |
| VAAPI (Intel/AMD encode) | `libva-dev` | `libva-devel` | `libva` |
| NVENC/NVDEC headers | `nv-codec-headers` | `nv-codec-headers` (RPM Fusion) | `ffnvcodec-headers` |
| Intel Quick Sync (oneVPL) | `libvpl-dev` | `oneVPL-devel` | `vpl-gpu-rt libvpl` |

Test-only: a virtual X server is needed to run the Qt-dependent tests headlessly —
`xvfb` (Debian/Ubuntu), `xorg-x11-server-Xvfb` (Fedora/RHEL), `xorg-server-xvfb` (Arch).

## Configure → build → test → launch, from a clean checkout

Two build trees are conventional in this project, and both are gitignored local artifacts:
`build-nogui` is the primary headless verification tree, and `build-ui` adds `src/ui` and the
`palmier-pro` executable.

```sh
git clone https://github.com/palmier-io/palmier-pro-linux.git
cd palmier-pro-linux

# 1. Configure. Add PKG_CONFIG_PATH=... if any dependency is a /usr/local install.
cmake -S . -B build-ui -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPALMIER_BUILD_UI=ON

# 2. Build.
cmake --build build-ui -j"$(nproc)"

# 3. Test. xvfb-run is required because the Qt shell and end-to-end suites need a display.
xvfb-run -a ctest --test-dir build-ui --output-on-failure

# 4. Launch.
./build-ui/bin/palmier-pro
```

Read the configuration summary that CMake prints last: it reports the build type, whether the UI
and tests are on, and — in three states each (`enabled (SDK found)`, `disabled (SDK not found)`,
`disabled (option OFF)`) — every vendor codec path, the TLS transport and both audio sinks. That
summary is the fastest way to confirm the tree is the tree you meant to configure.

The headless tree, for a host without Qt or for CI-style verification of everything except the
window:

```sh
cmake -S . -B build-nogui -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPALMIER_BUILD_UI=OFF
cmake --build build-nogui -j"$(nproc)"
ctest --test-dir build-nogui --output-on-failure
```

Both trees register the **same** set of tests — `build-ui` adds only the `palmier-pro` executable,
which registers no test — so a divergence in test count between the two trees is a build
configuration problem, not a UI-only test.

On launch you should see: the platform gate pass silently, the MCP endpoint bind
`127.0.0.1:19789/mcp`, and the editor window open with all five panels. A missing dependency at
launch produces a message naming **each** unmet item and no window. A port conflict on 19789
produces a non-blocking warning and the editor still runs — without the agent endpoint.

## CMake options (Requirements 16.1, 16.7)

Every name in the two tables below is a `PALMIER_*` **cache** variable of the configured build
tree, and the tables are exhaustive: 16 entries, of which 10 are user-settable and 6 are derived.
Verify against a configured tree with:

```sh
cmake -LAH build-ui | grep PALMIER_       # user-settable entries
grep '^PALMIER_' build-ui/CMakeCache.txt  # includes the derived INTERNAL entries
```

**Extraction contract for task 12.7.** The checker must take the documented option set from the two
tables between the `palmier-options` markers below and from nowhere else in this file. Names
discussed in prose outside that region — the `PALMIER_HAVE_*` compile guards, the `PALMIER_*`
environment variables in [`REMOTE_ACCESS.md`](REMOTE_ACCESS.md) — are deliberately not cache
variables and must not be read as documented options.

<!-- palmier-options:begin -->

### User-settable options

| Option | Type | Default | Effect |
|---|---|---|---|
| `PALMIER_BUILD_TESTS` | BOOL | `ON` | Builds the GoogleTest + RapidCheck unit and property tests, calls `enable_testing()` and adds `tests/`. `OFF` also removes `PALMIER_PBT_MIN_SUCCESS` from the cache, because that variable is created by `tests/CMakeLists.txt`. |
| `PALMIER_BUILD_UI` | BOOL | `ON` | Builds the Qt 6 presentation layer and the `palmier-pro` application shell. `OFF` skips the `find_package(Qt6 6.2 COMPONENTS Core Gui Quick Qml Widgets)` lookup entirely, so a host without Qt configures and tests. |
| `PALMIER_WERROR` | BOOL | `OFF` | Treats compiler warnings as errors. |
| `PALMIER_ENABLE_OPENSSL` | BOOL | `ON` | Requests the OpenSSL 3.x TLS transport for the remote MCP endpoint. Detection is optional: with the option `ON` and OpenSSL absent, configuration still succeeds and configuring TLS material becomes an unmet prerequisite that falls the endpoint back to loopback. Requires `openssl >= 3.0` — the 1.x series is not GPLv3-compatible and is not accepted. |
| `PALMIER_ENABLE_VAAPI` | BOOL | `ON` | Requests the VAAPI hardware codec path (Intel/AMD via `libva`, `libva-drm`). A missing SDK is never fatal; encode falls back to the FFmpeg software path. |
| `PALMIER_ENABLE_NVENC` | BOOL | `ON` | Requests the NVIDIA NVDEC/NVENC path via the `ffnvcodec` headers. A missing SDK is never fatal. |
| `PALMIER_ENABLE_QSV` | BOOL | `ON` | Requests the Intel Quick Sync path via oneVPL (`vpl`), falling back to the legacy Media SDK (`libmfx`). A missing SDK is never fatal. |
| `PALMIER_ENABLE_PIPEWIRE` | BOOL | `ON` | Requests the PipeWire audio output sink (`libpipewire-0.3`, MIT). Absent, the startup sink selection continues to ALSA and then to the always-compiled null sink. |
| `PALMIER_ENABLE_ALSA` | BOOL | `ON` | Requests the ALSA audio output sink (`libasound2`, pkg-config module `alsa`, LGPL-2.1-or-later). Absent, selection falls through to the null sink: audio is suppressed, video keeps running and a notice is raised (Requirement 6.7). |
| `PALMIER_PBT_MIN_SUCCESS` | STRING | `100` | Minimum successful RapidCheck iterations per property test, injected into every registered test as `RC_PARAMS=max_success=<n>`. CI may raise it; a value below 100 is warned about and forced back to 100, because the floor is Requirement 15.2. Present in the cache only when `PALMIER_BUILD_TESTS=ON`. |

### Derived, read-only entries

These are `CACHE INTERNAL`, recomputed on every configure run, and are **not** settable — setting
one has no effect because detection overwrites it. Each is `ON` if and only if its matching option
above is `ON` *and* the dependency was found, which is what lets consumers gate on one variable
while the configuration summary distinguishes "disabled by option" from "SDK not found" by combining
the two.

| Entry | Type | `ON` when |
|---|---|---|
| `PALMIER_VAAPI_AVAILABLE` | INTERNAL | `PALMIER_ENABLE_VAAPI` and `libva` + `libva-drm` found |
| `PALMIER_QSV_AVAILABLE` | INTERNAL | `PALMIER_ENABLE_QSV` and `vpl` or `libmfx` found |
| `PALMIER_NVENC_AVAILABLE` | INTERNAL | `PALMIER_ENABLE_NVENC` and `ffnvcodec` found |
| `PALMIER_OPENSSL_AVAILABLE` | INTERNAL | `PALMIER_ENABLE_OPENSSL` and `openssl >= 3.0` found |
| `PALMIER_PIPEWIRE_AVAILABLE` | INTERNAL | `PALMIER_ENABLE_PIPEWIRE` and `libpipewire-0.3` found |
| `PALMIER_ALSA_AVAILABLE` | INTERNAL | `PALMIER_ENABLE_ALSA` and `alsa` found |

<!-- palmier-options:end -->

Outside the marked region, and therefore outside the option comparison: the `PALMIER_HAVE_*`
preprocessor definitions (`PALMIER_HAVE_QT`, `PALMIER_HAVE_OPENSSL`, `PALMIER_HAVE_PIPEWIRE`,
`PALMIER_HAVE_ALSA`, `PALMIER_HAVE_VAAPI`, `PALMIER_HAVE_NVENC`, `PALMIER_HAVE_QSV`) are compile
definitions attached to targets, not cache variables — they are how source code sees the result of
the detection above. `PALMIER_TEST_TIMEOUT_SECONDS` (600 s per test, Requirement 15.8),
`PALMIER_SKIP_WITHOUT_HW`, `PALMIER_SOURCE_DIR` and `PALMIER_DOCS_DIR` are likewise plain variables
or compile definitions inside `tests/`.

Failing a required dependency does not abort on the first miss: discovery accumulates **all**
failures and reports one message naming every missing dependency with an install hint. Only the
software media path is mandatory, so no vendor SDK ever appears in that message.

## Installable package (usable-editor Requirement 13)

`cmake/PalmierPackaging.cmake` configures [CPack](https://cmake.org/cmake/help/latest/module/CPack.html)'s
`DEB` generator, producing a self-contained Debian package for the documented Debian/Ubuntu family
above — the family CI itself builds and tests on (`ubuntu-24.04`), so it is also the one with direct
evidence behind the packaged artifact. Build one from an existing `build-ui` configuration:

```sh
cmake --build build-ui
cd build-ui && cpack -G DEB
```

The package's `Depends:` field is generated by `dpkg-shlibdeps` (`CPACK_DEBIAN_PACKAGE_SHLIBDEPS=ON`)
inspecting the built `palmier-pro` binary's actual linked libraries — Qt 6, FFmpeg, Vulkan, shaderc,
LittleCMS and libsecret — rather than a hand-maintained list, so `apt`/`dpkg` refuse to install the
package at all on a host missing one of them. `PlatformCompatibility`'s launch-time gate (Requirement
1.4) remains the second, independent check for a host whose package manager was bypassed. The GPLv3
[`LICENSE`](../LICENSE) text and [`NOTICE`](../NOTICE) are installed into
`/usr/share/doc/palmier-pro/` alongside the executable at `/usr/bin/palmier-pro`, preserving the
licensing split the top-level [`README`](../README.md) documents.

CI's `package` job (`.github/workflows/ci.yml`) builds this package, verifies its `Depends:` field is
non-empty, installs it with `apt`/`dpkg` for real, and reuses the launch smoke test against the
installed `/usr/bin/palmier-pro` — not a build-tree binary — whenever a commit is pushed as a `v*`
tag.

## Environment traps this project actually hit

These cost real time on this project's own hosts. Read them before concluding that a dependency is
missing or that a tree is broken.

**A dependency under `/usr/local` needs `PKG_CONFIG_PATH`.** FFmpeg in particular may have to be
built from source (for example when the distribution package is too old, or when a container image
has none), and it is discovered purely through `pkg-config`. Configure both trees with:

```sh
PKG_CONFIG_PATH=/usr/local/lib/pkgconfig cmake -S . -B build-nogui -DPALMIER_BUILD_UI=OFF
PKG_CONFIG_PATH=/usr/local/lib/pkgconfig cmake -S . -B build-ui   -DPALMIER_BUILD_UI=ON
```

A from-source FFmpeg 6.1.x is enough for the editor, but note that a minimal
`--enable-shared --disable-static --disable-doc --disable-programs --disable-debug` build carries
**no** `libx264`, `libx265`, `libvpx` or `libaom`, so it can decode but cannot encode H.264, HEVC or
VP9. Add the external encoder libraries if you intend to export in those codecs or to run
Requirement 8.6's hardware-versus-software export comparison.

**Qt 6 installed via `aqtinstall` needs an `ldconfig` entry.** When Qt comes from
`aqt install-qt linux desktop 6.8.3 linux_gcc_64` and is copied into `/usr/local`, the libraries
are present but not loadable at runtime until `/usr/local/lib` is on the dynamic loader's path:

```sh
echo /usr/local/lib > /etc/ld.so.conf.d/zz-usr-local.conf && ldconfig
```

**"Qt 6 not found" usually is not a missing Qt — it is missing OpenGL/GLX development packages.**
`find_package(Qt6 6.2 QUIET COMPONENTS ...)` also fails when Qt6Gui's `WrapOpenGL` dependency is
unmet, and `QUIET` suppresses the real reason, so the failure reads as "Qt 6 is absent" no matter
what is actually wrong. This trap cost this project an entire build tree. Before reinstalling Qt,
confirm the diagnosis with a throwaway **non**-`QUIET` `find_package` in a scratch directory:

```sh
mkdir -p /tmp/qtprobe && cd /tmp/qtprobe
printf 'cmake_minimum_required(VERSION 3.22)\nproject(p)\nfind_package(Qt6 6.2 REQUIRED COMPONENTS Core Gui Widgets)\n' > CMakeLists.txt
cmake -S . -B b     # the real error names WrapOpenGL when GL/GLX headers are the problem
```

The fix is the OpenGL/GLX development packages from the table above, not reinstalling Qt.

**After any dependency loss, delete and reconfigure both build trees.** A stale cache will keep
reporting a dependency as found against headers that are no longer on disk; the tree then either
fails to compile or **silently compiles the FFmpeg paths as stubs**, which lowers the registered
test count without failing anything. A test count *below* the expected number is a stub build, not
a regression, and calls for a from-scratch reconfigure rather than acceptance. `cmake --build` on an
existing tree is not sufficient — only a fresh configure re-runs the `pkg-config` probes.

```sh
rm -rf build-nogui build-ui   # after any dependency loss, never reuse the cache
```

Two cheap gates detect the loss before you trust any build tree at all:
`pkg-config --exists libavcodec && echo ffmpeg ok` and `command -v qmake6`.

## Next

- Connect an agent: [`MCP_CLIENTS.md`](MCP_CLIENTS.md)
- Expose the endpoint beyond loopback: [`REMOTE_ACCESS.md`](REMOTE_ACCESS.md)
