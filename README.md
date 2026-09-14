# Palmier Pro for Linux

[![CI](https://github.com/Bhaveshmeghwal21/palmier-pro-linux/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/Bhaveshmeghwal21/palmier-pro-linux/actions/workflows/ci.yml)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![Qt 6](https://img.shields.io/badge/Qt-6-41cd52.svg)](https://www.qt.io/)
[![Platform: Linux](https://img.shields.io/badge/platform-Linux-lightgrey.svg)](docs/BUILD.md)

A multi-track video editor for Linux, ported from Palmier Pro to C++20, Qt 6, FFmpeg and
Vulkan. GPU compositing runs on NVIDIA, AMD and Intel; a CPU fallback is a first-class path,
not a degraded one.

The editor's entire edit surface — clips, tracks, effects, grading, transitions, export — is
published as tools over a local [Model Context Protocol](docs/MCP_CLIENTS.md) server. Claude
Code, Codex, Cursor, or any other MCP client can drive the timeline directly. The in-app agent
chat calls the same tools, and a property test checks that both paths produce identical edits.

[Quick start](#quick-start) ·
[Driving it from an agent](#driving-it-from-an-agent) ·
[Testing](#testing) ·
[Documentation](#documentation) ·
[Project status](#project-status)

## Example

![A 39-second aerial survey reel cut by an AI agent driving Palmier Pro for Linux: an animated title, a split-screen grade comparison, an animated picture-in-picture inset, and a four-up grid](docs/media/showreel.gif)

This reel was cut with no manual timeline editing: an agent issued 150 tool calls across 19
distinct tools — import, cut, layout, grade, keyframe, caption, export — with zero failures,
and the result was encoded to 1080p30 H.264 via NVENC on an NVIDIA L4.

| Moment | Subsystem |
|---|---|
| Opening title, drifting and scaling | Shared text renderer (preview and export); keyframe animation on scale and position |
| `UNGRADED` vs. `GRADED` split | Primary grade, tone curve, 3D `.cube` LUT, composited via `apply_layout` |
| Night inset crossing the frame | `apply_layout picture_in_picture` with keyframed centre and scale |
| Four angles at once | `apply_layout grid`, four tracks, independent grades per source |

The same edit runs clips at 0.5× and 2.0×, applies wipe/slide/dip-to-colour transitions, burns
in captions with an `.srt` sidecar, and keyframes the audio bed's gain. Every tool used is
documented in [`docs/TOOLS.md`](docs/TOOLS.md).

## Quick start

Three ways to get a build. Each is self-contained.

### Docker

```bash
git clone https://github.com/Bhaveshmeghwal21/palmier-pro-linux.git
cd palmier-pro-linux
make docker-test
```

or with Compose:

```bash
docker compose run --rm test        # build and run the full suite
docker compose up editor            # launch the editor on your X display
docker compose run --rm shell       # a shell with the full toolchain
```

The image is `ubuntu:24.04`, pinned by digest, installing the packages listed in
[`packaging/apt-packages.txt`](packaging/apt-packages.txt) — the same file CI installs from.

### Ubuntu 24.04 (or another Debian derivative)

```bash
git clone https://github.com/Bhaveshmeghwal21/palmier-pro-linux.git
cd palmier-pro-linux

make deps        # Qt 6, FFmpeg, Vulkan, vendor codec SDKs (uses sudo)
make build       # CMake + Ninja configure and compile
make test        # full suite, headless under Xvfb
make run         # launch the editor
```

Run `make` with no target for the full list.

### CMake directly

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

Same flags CI uses in [`.github/workflows/ci.yml`](.github/workflows/ci.yml). Other
distributions, package names, every `PALMIER_*` option, and minimum host requirements are in
[`docs/BUILD.md`](docs/BUILD.md).

### No GPU, no audio device, no display

All three are supported configurations, and CI runs on a bare runner that has none of them:

- **No Vulkan device** — status bar reports "No compatible GPU was found; using CPU
  processing"; compositing runs on the CPU.
- **No audio output** — sink order is PipeWire → ALSA → null. With none available, audio is
  suppressed; video keeps playing.
- **No display** — the test suite runs under `xvfb-run`; the editor binary itself requires a
  display to launch.

## Driving it from an agent

Point any MCP client at `127.0.0.1:19789`. A grade, end to end:

```jsonc
// tools/call — edge-preserving denoise, then warm the highlights
{ "name": "timeline.add_effect",
  "arguments": { "clipId": "…", "type": "denoise",
                 "parameters": { "strength": 0.35, "radius": 2 } } }

{ "name": "timeline.add_effect",
  "arguments": { "clipId": "…", "type": "color_grade",
                 "parameters": { "highlightsHue": 40, "highlightsAmount": 0.5 } } }
```

Every tool, argument and result field is in [`docs/TOOLS.md`](docs/TOOLS.md); a
documentation-consistency test fails the build if a tool ships undocumented. Client setup for
Claude Code, Codex and Cursor is in [`docs/MCP_CLIENTS.md`](docs/MCP_CLIENTS.md). The endpoint
binds to loopback only unless explicitly reconfigured — see
[`docs/REMOTE_ACCESS.md`](docs/REMOTE_ACCESS.md).

## Testing

```bash
make test              # full suite, headless
make test-verbose      # per-test output, JUnit XML, generated summary
make coverage          # instrumented build, gcovr line-coverage report
```

The suite holds more than 1900 GoogleTest and RapidCheck cases across 90 test executables, run
by CI on every
push and pull request against `main`. Beyond unit tests: property tests over generated
timelines, round-trip tests over the project file format, agent/UI edit-equivalence tests, and
documentation-consistency tests that fail the build when a CMake option or tool is added
without being documented.

Line coverage is around 81%, gated at a floor of 78% by a dedicated CI job that publishes
Cobertura XML, an lcov `coverage.info`, and drill-down HTML. Branch coverage (around 46%) is
not gated: most unhit branches are vendor hardware-codec and Vulkan paths that no hosted
runner can exercise, and gating them would penalize the absence of a GPU rather than the
quality of the tests.

CI also launches the built binary on a virtual display, confirms it maps a window and paints a
UI, and publishes the screenshot as an artifact — so "the editor starts" is a measured result,
not an inference from widget tests.

Examples of what specific tests pin:

- Denoise must preserve an edge that a blur destroys — the same step edge is filtered both
  ways and compared.
- A secondary grade with a qualifier that selects everything must produce byte-identical
  pixels to the primary grade with the same correction.
- A colour wheel at amount zero must resolve bit-for-bit to what the project resolved to
  before wheels existed, so no saved project's rendered output can silently change.

### Developer tooling

```bash
make format            # clang-format (config: .clang-format)
make format-check      # fail if changed files are misformatted
make tidy              # clang-tidy the changed files (config: .clang-tidy)
make py-lint           # ruff + mypy over the Python CI helpers
make py-test           # unit tests for those helpers
make lint              # format-check + py-lint
make ci                # build, test, lint, py-test — what CI runs
```

A [devcontainer](.devcontainer/devcontainer.json) reuses the same Dockerfile, so VS Code or
Codespaces opens ready to build.

## Project layout

```
src/core/        Domain model: timeline, clips, effects, undo/redo, tone curves, logging
src/media/       FFmpeg: decode, encode, export, audio graph, peak envelopes
src/gpu/         Vulkan compositing, effect kernels, scopes, LUTs, codec bridge
src/services/    Project store, MCP server, tool registry, export coordination
src/ui/          Qt 6 widgets and view models (view models are Qt-free where possible)
src/app/         Entry point, settings, platform checks
tests/           90 test executables: unit, property, integration, e2e, docs
docs/            Operational documentation
packaging/       Debian, Flatpak and AppImage packaging, apt manifest
scripts/         CI report generators (stdlib-only Python) and their tests
```

Dependency direction is layered: `core` has no dependency on Qt, FFmpeg or Vulkan, and most UI
logic lives in view models testable without a display. Errors are a typed `Result<T>`, not
exceptions.

Every effect renders twice — once on the GPU, once on a host-memory software reference — and a
parity property checks per-channel agreement. A machine with no Vulkan device produces the
same output as one with an NVIDIA L4.

## Documentation

| Document | Contents |
|---|---|
| [`docs/QUICKSTART.md`](docs/QUICKSTART.md) | Build, launch and drive the editor end to end, plus remediation for a missing GPU, missing audio, or a refused MCP connection |
| [`docs/BUILD.md`](docs/BUILD.md) | Full build pipeline, native packages per distribution, every `PALMIER_*` option, minimum host spec, environment pitfalls |
| [`docs/MCP_CLIENTS.md`](docs/MCP_CLIENTS.md) | Connecting Claude Code, Codex and Cursor to `127.0.0.1:19789` |
| [`docs/TOOLS.md`](docs/TOOLS.md) | Every published tool, argument and result field |
| [`docs/HARDWARE_ENCODE.md`](docs/HARDWARE_ENCODE.md) | NVENC, Quick Sync and VAAPI prerequisites, software fallback, host verification, L4 validation |
| [`docs/REMOTE_ACCESS.md`](docs/REMOTE_ACCESS.md) | Opt-in non-loopback access: token, TLS, origin allow-list, session/timeout limits |
| [`docs/OBSERVABILITY.md`](docs/OBSERVABILITY.md) | Log levels/formats, `PALMIER_LOG_LEVEL` / `PALMIER_LOG_FORMAT`, category vocabulary, `GET /health`, CI recording |
| [`docs/UPSTREAM_PARITY.md`](docs/UPSTREAM_PARITY.md) · [`docs/PORT_BACKLOG.md`](docs/PORT_BACKLOG.md) | Upstream parity report and deferred-port backlog |
| [`docs/CLASSIFICATION.md`](docs/CLASSIFICATION.md) | What kind of project this is: a desktop application with no Infrastructure-as-Code layer, and where the engineering evidence lives instead |
| [`CHANGELOG.md`](CHANGELOG.md) · [`SECURITY.md`](SECURITY.md) | Release history; security model and reporting |
| [`.env.example`](.env.example) | Every environment variable the editor reads, in precedence order |

## Project status

**Working, covered by CI:** multi-track editing with undo/redo; media import and validation;
software and hardware codec paths; GPU and CPU compositing; export; audio playback and
metering; timeline waveforms; scrub audio; primary grade with tone curves and three colour
wheels; qualifier-masked secondary grading; edge-preserving denoise; video scopes; 3D LUTs;
keyframe animation; clip speed; project save/load; MCP server and agent chat; localization.

**Packaging** is verified at three depths, since only one format builds on a hosted runner.
The `.deb` is built by CI on every `v*` tag and inspected with `dpkg-deb` for contents,
declared dependencies and metadata. Flatpak and AppImage recipes are fed as source input to
`flatpak-builder` and `appimage-builder`; their manifests are cross-checked against the shared
freedesktop files on every run, so a renamed app id, executable, licence, or missing asset
fails the build. The images themselves are not built in CI and are the least exercised part of
the repository.

**Hardware encode**, verified on real hardware: the `l4-validation` job runs on a self-hosted
NVIDIA L4 and exports a 300-frame 1920x1080/30fps fixture:

```
PALMIER_L4_ENCODER_NAME=h264_nvenc      PALMIER_L4_USED_HARDWARE_ENCODE=true
PALMIER_L4_FRAMES_ENCODED=300           PALMIER_L4_SOFTWARE_FALLBACK=false
PALMIER_L4_ELAPSED_MS=30115             PALMIER_L4_OUTPUT_BYTES=622870
```

The same job checks that the hardware and software exports of that fixture agree on frame
count and duration. Re-running at a later commit reproduced the encoder, frame count and byte
count exactly (622870 bytes both runs); `PALMIER_L4_ELAPSED_MS` moved from 11863 to 30115 on
the same fixture and hardware, since the runner is shared. Treat the byte count as
reproducible and the wall-clock figure as order-of-magnitude only. Details in
[`docs/HARDWARE_ENCODE.md`](docs/HARDWARE_ENCODE.md).

**Compiled but not runtime-verified:** VAAPI, Quick Sync, and the Vulkan compositor. No hosted
runner has an AMD or Intel encode device, so CI confirms these build and that the software
fallback runs correctly when no device is present.

**Upstream parity:** tracked in [`docs/UPSTREAM_PARITY.md`](docs/UPSTREAM_PARITY.md) as 34
scored entries, 17 complete. Remaining gaps: multicam angle switching, a shape layer, media
bins and search, and an SSE stream on the MCP endpoint.

Three items are marked as intentionally out of scope rather than pending: speech-to-text
captions (no recognizer is bundled, and none will ship proprietary); telemetry (a GPL editor
collecting usage data is the wrong default); in-app auto-update (Linux delivery is the package
manager's job).

## Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md). Two things before your first commit:

1. Every source file requires a GPLv3 header — templates in
   [`docs/LICENSE-HEADER.txt`](docs/LICENSE-HEADER.txt); a test fails the build if one is
   missing.
2. `make lint && make test` is exactly what CI checks — the Makefile and the workflow run the
   same commands.

## License

GPLv3-or-later for the editor, with a closed hosted service for generative AI. Full terms in
[`LICENSE`](LICENSE) and [`NOTICE`](NOTICE).

**Open source (GPLv3-or-later):** the multi-track timeline editor, domain core, media engine,
GPU abstraction layer, export engine, localization and Qt 6 UI; the local MCP server; the
in-app agent chat. All fully functional with no network connection to the generative service.

**Closed service (account-gated):** the generative-AI capability is a hosted, proprietary
service requiring an authenticated account or BYOK credentials. Its backend and models are not
distributed under GPLv3 and are not part of this repository beyond the GPLv3 client that
contacts it. The open-source editor works without degradation when the service is
unreachable or unauthenticated.
