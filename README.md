# Palmier Pro for Linux

[![CI](https://github.com/Bhaveshmeghwal21/palmier-pro-linux/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/Bhaveshmeghwal21/palmier-pro-linux/actions/workflows/ci.yml)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![Qt 6](https://img.shields.io/badge/Qt-6-41cd52.svg)](https://www.qt.io/)
[![Platform: Linux](https://img.shields.io/badge/platform-Linux-lightgrey.svg)](docs/BUILD.md)

An AI-native, multi-track video editor for Linux with first-class GPU acceleration
(NVIDIA, AMD, Intel) and graceful CPU fallback. This is the ground-up C++20 Linux port
of Palmier Pro, built with Qt 6 (UI), FFmpeg (media/codecs), and Vulkan (GPU
compositing and hardware-codec bridge).

What makes it *AI-native* is that the editor exposes its entire edit surface as tools
over a local [Model Context Protocol](docs/MCP_CLIENTS.md) server, so an external agent
— Claude Code, Codex, Cursor — drives the same operations the buttons do. The in-app
agent chat uses that identical tool surface, and a property test asserts the two paths
produce equivalent edits.

---

## Quick start

Three paths. Pick one; each is complete on its own.

### 1. With only Docker installed

Nothing to install but Docker. This builds the whole toolchain and runs the entire
suite:

```bash
git clone https://github.com/Bhaveshmeghwal21/palmier-pro-linux.git
cd palmier-pro-linux
make docker-test
```

or with Compose:

```bash
docker compose run --rm test        # build + run the full suite
docker compose up editor            # launch the editor on your X display
docker compose run --rm shell       # a shell with the whole toolchain
```

The image is `ubuntu:24.04` pinned by digest, installing exactly the packages in
[`packaging/apt-packages.txt`](packaging/apt-packages.txt) — the same file CI installs
from, so the container and CI cannot drift apart.

### 2. On Ubuntu 24.04 (or another Debian derivative)

```bash
git clone https://github.com/Bhaveshmeghwal21/palmier-pro-linux.git
cd palmier-pro-linux

make deps        # install Qt 6, FFmpeg, Vulkan, vendor codec SDKs (uses sudo)
make build       # configure with CMake + Ninja, then compile
make test        # run the full test suite headlessly under Xvfb
make run         # launch the editor
```

`make` on its own lists every target with a one-line description.

### 3. Explicit CMake, if you would rather not use the Makefile

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DPALMIER_BUILD_TESTS=ON \
  -DPALMIER_BUILD_UI=ON \
  -DPALMIER_ENABLE_VAAPI=ON \
  -DPALMIER_ENABLE_NVENC=ON \
  -DPALMIER_ENABLE_QSV=ON

cmake --build build
xvfb-run -a ctest --test-dir build --output-on-failure
./build/bin/palmier-pro
```

These are the same flags and the same commands
[`.github/workflows/ci.yml`](.github/workflows/ci.yml) runs. Other distributions,
per-distribution package names, every `PALMIER_*` option and the minimum host
specification are in [`docs/BUILD.md`](docs/BUILD.md).

### Running with no GPU, no sound card or no display

All three are supported states rather than failure modes, which is why the suite passes
on a bare CI runner:

- **No Vulkan device** → the status bar reports *"No compatible GPU was found; using CPU
  processing"* and compositing runs on the CPU.
- **No audio output** → the sink order is PipeWire → ALSA → null. With none available,
  audio is suppressed, video keeps playing and a notice is raised.
- **No display** → every test runs under `xvfb-run`; the editor itself needs a display.

---

## Testing

```bash
make test              # the whole suite, headless
make test-verbose      # per-test output, JUnit XML, and a generated summary
make coverage          # instrumented build, then a gcovr line-coverage report
```

The suite is **more than 1800 GoogleTest and RapidCheck cases across 90 test
executables**, and CI runs all of them on every push to `main` and on every pull request.
It is not only unit tests: there are property tests over generated timelines, round-trip
tests over the project format, agent/UI edit-equivalence tests, and
documentation-consistency tests that fail the build when a CMake option or an agent tool is
added without documenting it.

**Line coverage is around 81%**, measured by a dedicated CI job that fails the build below
a floor of 78% and publishes Cobertura XML, an lcov `coverage.info` and drill-down HTML as
an artifact. Branch coverage is around 46% and is
deliberately not gated: most of the unhit branches are the vendor hardware-codec and
Vulkan paths no hosted runner can execute, so gating them would gate on the absence of
a GPU rather than on the quality of the tests.

CI additionally launches the built binary on a virtual display, asserts it maps its main
window and paints a real UI, and publishes the screenshot as an artifact — so *"the
editor starts"* is evidence rather than an inference from widget tests.

### Developer tooling

```bash
make format            # clang-format this repository's C++ (config: .clang-format)
make format-check      # fail if changed files are misformatted
make tidy              # clang-tidy the changed files (config: .clang-tidy)
make py-lint           # ruff + mypy over the Python CI helpers
make py-test           # the Python helpers' own unit tests
make lint              # format-check + py-lint
make ci                # build, test, lint, py-test — what CI checks
```

A [devcontainer](.devcontainer/devcontainer.json) is provided and reuses the same
Dockerfile, so VS Code or Codespaces opens ready to build.

---

## Project layout

```
src/core/        Domain model: timeline, clips, effects, undo/redo, tone curves, logging
src/media/       FFmpeg: decode, encode, export, audio graph, peak envelopes
src/gpu/         Vulkan compositing, effect kernels, scopes, LUTs, codec bridge
src/services/    Project store, MCP server, tool registry, export coordination
src/ui/          Qt 6 widgets and view models (view models are Qt-free where possible)
src/app/         Entry point, settings, platform checks
tests/           90 test executables: unit, property, integration, e2e, docs
docs/            All operational documentation
packaging/       Debian, Flatpak and AppImage packaging, and the apt manifest
scripts/         CI report generators (stdlib-only Python) and their tests
```

Layering is enforced by dependency direction: `core` knows nothing of Qt, FFmpeg or
Vulkan, and most UI logic lives in view models that can be tested without a display.
Errors are returned as a typed `Result<T>` rather than thrown.

---

## Documentation

Everything operational lives in [`docs/`](docs/), so each option, tool and
procedure is documented in exactly one place.

| Document | What it covers |
|---|---|
| [`docs/QUICKSTART.md`](docs/QUICKSTART.md) | Build, launch and drive the editor end to end, with remediation for a missing GPU, missing audio output and a refused MCP connection |
| [`docs/BUILD.md`](docs/BUILD.md) | Configure → build → test → launch, native packages per distribution, every `PALMIER_*` option, the minimum host specification and the environment traps |
| [`docs/MCP_CLIENTS.md`](docs/MCP_CLIENTS.md) | Connecting Claude Code, Codex and Cursor to the loopback endpoint at `127.0.0.1:19789` |
| [`docs/TOOLS.md`](docs/TOOLS.md) | Every tool the agent surface publishes, with each argument and result field |
| [`docs/HARDWARE_ENCODE.md`](docs/HARDWARE_ENCODE.md) | NVENC, Quick Sync and VAAPI prerequisites, the software fallback, host verification and the L4 validation procedure |
| [`docs/REMOTE_ACCESS.md`](docs/REMOTE_ACCESS.md) | Opt-in non-loopback access: token, TLS, Origin allow-list, session and timeout limits |
| [`docs/OBSERVABILITY.md`](docs/OBSERVABILITY.md) | Log levels and formats, `PALMIER_LOG_LEVEL` / `PALMIER_LOG_FORMAT`, the category vocabulary, the `GET /health` endpoint and its error counters, and what each CI run records |
| [`docs/UPSTREAM_PARITY.md`](docs/UPSTREAM_PARITY.md) · [`docs/PORT_BACKLOG.md`](docs/PORT_BACKLOG.md) | Upstream parity report and the deferred-port backlog |
| [`CHANGELOG.md`](CHANGELOG.md) · [`SECURITY.md`](SECURITY.md) | Release history, and the security model and reporting process |
| [`.env.example`](.env.example) | Every environment variable the editor reads, with its precedence |

---

## Project status

Honest about the boundary, because it matters more than a claim of completeness.

**Working and covered by CI:** multi-track editing with undo/redo, media import and
validation, software and (compiled) hardware codec paths, GPU and CPU compositing,
export, audio playback and metering, timeline waveforms, scrub audio, a full primary
grade with tone curves, video scopes, 3D LUTs, project save/load, the MCP server and
agent chat, and localization.

**Packaging is verified to three different depths**, because only one of the three formats
can be built on a hosted runner. The `.deb` is built by CI on every `v*` tag and then
inspected with `dpkg-deb` — contents, declared dependencies, and metadata. The Flatpak and
AppImage recipes are provided as source inputs to `flatpak-builder` and `appimage-builder`;
their manifests are cross-checked on every run against the shared freedesktop files, so a
renamed application id, executable, licence, or an installed asset that does not exist
fails the build — but the images themselves are not built in CI, and are the least
exercised part of this repository.

**Verified on real NVIDIA hardware:** H.264 hardware encode. The `l4-validation` job runs
on a self-hosted NVIDIA L4 runner and exports a 300-frame 1920x1080/30 fps fixture,
recording what actually happened:

```
PALMIER_L4_ENCODER_NAME=h264_nvenc      PALMIER_L4_USED_HARDWARE_ENCODE=true
PALMIER_L4_FRAMES_ENCODED=300           PALMIER_L4_SOFTWARE_FALLBACK=false
PALMIER_L4_ELAPSED_MS=11863             PALMIER_L4_OUTPUT_BYTES=622870
```

The same job asserts the hardware and software exports of that fixture agree on frame
count and duration, so "NVENC works" is a measurement rather than a claim. See
[`docs/HARDWARE_ENCODE.md`](docs/HARDWARE_ENCODE.md).

**Compiled and type-checked but not executed:** the VAAPI and Quick Sync paths, and the
Vulkan compositor. No hosted runner has an AMD or Intel encode device, so CI proves these
*build* and that the software fallback is what actually runs when no device is present.

**Not yet at parity with the upstream product.** The gaps are enumerated and prioritised
in [`docs/UPSTREAM_PARITY.md`](docs/UPSTREAM_PARITY.md) rather than left implicit; the
largest remaining ones are colour wheels and secondary/qualifier grading, denoise, and
parts of the captions and multicam surfaces.

---

## Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md). Two things to know before your first commit:

1. Every source file carries a GPLv3 per-file header — templates are in
   [`docs/LICENSE-HEADER.txt`](docs/LICENSE-HEADER.txt), and a property test in the
   suite fails the build if one is missing.
2. `make lint && make test` locally means the same thing CI means, because the Makefile
   runs the same commands the workflow does.

---

## Licensing

Palmier Pro for Linux uses a deliberate open-source / closed-service split.
Read the [`LICENSE`](LICENSE) (full, unmodified GNU GPLv3 text) and
[`NOTICE`](NOTICE) files for the authoritative details.

### Open source — GPLv3-or-later

The editor and its tooling are free software under the **GNU General Public
License, version 3 or later**. This covers:

- **Timeline Editor** — the multi-track timeline editor, domain core, media
  engine, GPU abstraction layer, export engine, localization, and Qt 6 UI.
- **MCP Server** — the local Model Context Protocol server exposing editor
  tools to external AI agents over loopback HTTP.
- **Agent Chat** — the in-app assistant that drives the editor through the same
  tool surface as the MCP server.

These components are fully functional with **no network connection** to the
generative service.

### Closed service — account-gated

The **generative-AI capability** is a proprietary, hosted service that requires
a valid authenticated account (active subscription or Bring-Your-Own-Key
credentials). Its backend and models are **not** distributed under GPLv3 and are
**not** part of this repository beyond the GPLv3 client that contacts it. When
the service is unreachable or unauthenticated, the open-source editor continues
to work without degradation.
