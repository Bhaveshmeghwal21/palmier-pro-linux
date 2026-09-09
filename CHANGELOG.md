# Changelog

All notable changes to Palmier Pro for Linux are recorded here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this
project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

This file starts at `v0.1.0`. Development before that tag is recorded in the commit
history and in the per-task records under `.kiro/specs/*/tasks.md`, which note each
task's completion commit, its CI run and the suite size at that point.

## [Unreleased]

### Added

- **Structured, levelled logging** (`core/Logging.hpp`). Six severities plus `off`, a
  free-form subsystem category, key/value fields, and two rendered formats — one
  human-readable line or newline-delimited JSON — selected by `PALMIER_LOG_FORMAT`.
  `PALMIER_LOG_LEVEL` sets the threshold. Records are data and both formatters are pure
  functions of one, so a test states a whole expected line in closed form; the sink and
  the clock are both injectable. Replaces the twelve `std::cerr` writes that were the
  entire diagnostic surface, all of them in the startup path. See
  [`docs/OBSERVABILITY.md`](docs/OBSERVABILITY.md).
- **A line-coverage gate in CI.** A dedicated job builds instrumented, runs the whole
  suite, and fails below a stated floor. First measurement: 80.1% of lines (15570 of
  19443); the floor is 78%. Cobertura XML, an lcov `coverage.info` and drill-down HTML
  are published as an artifact.
- **A lint job in CI**, gating `clang-format` over the files a commit touches, `ruff`
  and `ruff format` over the Python tooling, and a new repository-consistency check
  that asserts `.env.example` matches the variables the code actually reads, that every
  relative link in the root documents resolves, and that the Makefile's documented
  targets and `.PHONY` list agree.
- **Reproducible setup**: a `Makefile` with 27 documented targets, a `Dockerfile` pinned
  to `ubuntu:24.04` by digest, `docker-compose.yml`, a devcontainer, and
  `packaging/apt-packages.txt` as the single native dependency manifest that the
  Makefile, the container and CI all install from.
- **Programme output level metering.** Per-channel peak and RMS measured in the audio
  graph immediately before submission, so the meter cannot disagree with what was
  heard. Peaks above full scale latch for at least a second and decay no faster than
  20 dB/s. Mounted in the timeline's transport bar.
- **Clip audio waveforms in the timeline.** Peak envelopes extracted off the decode
  thread into a bounded cache, stored as min/max pairs in source time so one envelope
  serves every clip that references the asset. An asset with no audio caches an empty
  envelope as a successful answer rather than an error, and lookups never block or
  decode.
- **Scrub audio.** Dragging the playhead on the timeline ruler plays audio at the
  scrubbed position, with a user-visible setting to disable it.
- **A full primary grade.** `color_grade` extended from a single lift scalar to
  per-channel lift, gamma and gain. Existing projects are byte-identical: the six new
  parameters resolve to defaults that reproduce the previous behaviour exactly, and a
  unity gamma skips the step rather than computing `pow(x, 1)`.
- **Tone curves** on master, red, green and blue channels, editable directly in the
  Inspector and through the `timeline.edit_curve_point` tool. The curve is baked to a
  256-entry table, so in an RGBA8 pipeline the table *is* the transfer function and the
  GPU kernel and software reference agree exactly rather than within a tolerance.
- **Video scopes.** Histogram, luma waveform and vectorscope in a new Scopes dock, fed
  from the very frame the Preview uploaded rather than from a re-render. Bounded to a
  share of the preview frame budget with a 10 Hz floor, and not computed at all while
  hidden.
- **3D LUT support.** `.cube` parsing with trilinear sampling, applied through a new
  `EffectType::Lut`. An unreadable or missing LUT is a cached answer, not an error: the
  render loop carries on with no table.
- **`timeline.set_effect_resource`** and **`timeline.edit_curve_point`** on the agent and
  MCP tool surface.

### Changed

- Project schema `1.4` → `1.5`, adding `Effect::resourcePath` for the LUT file. The key
  is omitted entirely when empty, so a project with no LUT is byte-identical to the
  previous format apart from the version string.
- The audio engine is now driven from the transport. Before this, `AudioEngine::pump()`
  and `start()` were called from no production code, which meant audio was never
  audible in a real build and the new level meter would have read zero.
- Export and Preview are now asserted to render identically at different sizes, rather
  than that being inferred from their sharing a compositor.

### Fixed

- A race in the export coordinator's cancellation test, where a hook documented as
  parking a worker only signalled it.

## [0.1.0] — 2026-08-31

First tagged release: the ground-up C++20 Linux port of Palmier Pro, built on Qt 6,
FFmpeg and Vulkan.

### Added

- **Multi-track timeline editor** with the domain core, undo/redo, project
  save/load and validation.
- **Media engine** on FFmpeg: import with validation, decode, encode, export, and
  hardware codec paths for VAAPI, NVENC and Quick Sync with graceful degradation to
  CPU processing when no device is present.
- **GPU compositing** through Vulkan, with a software reference implementation that
  every effect kernel is checked against.
- **MCP server** over loopback HTTP, exposing the editor's tool surface to external AI
  agents, with opt-in token-gated remote access behind TLS and an Origin allow-list.
- **Agent chat** driving the editor through the same tool surface as the MCP server.
- **Localization** and a platform compatibility check.
- **Packaging** for Debian (`.deb`), Flatpak and AppImage.
- **Verification suite** of GoogleTest unit tests and RapidCheck property tests, run
  headlessly in CI on every push, publishing a per-test summary with every recorded
  skip reason.

[Unreleased]: https://github.com/Bhaveshmeghwal21/palmier-pro-linux/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/Bhaveshmeghwal21/palmier-pro-linux/releases/tag/v0.1.0
