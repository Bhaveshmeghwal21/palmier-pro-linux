# Changelog

All notable changes to Palmier Pro for Linux are recorded here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this
project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

This file starts at `v0.1.0`. Development before that tag is recorded in the commit
history and in the per-task records under `.kiro/specs/*/tasks.md`, which note each
task's completion commit, its CI run and the suite size at that point.

## [Unreleased]

### Added

- **`export.tool_budget_seconds`, because a real edit could not be exported at all.** The
  tool surface awaits its own export under a bounded budget so an overrun reports a clean
  timeout and removes the partial file. That budget was fixed at 55 seconds and reachable
  only by an embedder, and a multi-track timeline renders at only a few frames per second
  on a modest host - measured here at **6.7 fps** for a single 1080p layer and **7.6 fps**
  at a 720p canvas, so decode dominates and shrinking the canvas buys almost nothing. A
  ten-second five-track edit therefore could not be exported by any operator of the
  shipped binary, on any hardware, with no way to raise the limit.

  One knob moves everything that has to move: the executor's per-invocation limit and the
  protocol layer's budget are derived from it with headroom, so the export still reports
  its own timeout rather than being killed first. Never shorter than the previous
  defaults, so raising nothing changes nothing. Found by editing real footage end to end
  rather than by a test.

### Fixed

- **The MCP endpoint served no protocol layer, so no external agent could connect.** This
  is the repository's headline claim, and it was broken in the shipped binary:
  `ApplicationComposition` never called `setProtocolDelegate`, so
  `McpServer::dispatchWithContext` - the path the socket transport uses - fell through to
  the tool executor. `initialize` answered `request is missing the tool 'name'` and
  `tools/list` did not exist, while [`docs/MCP_CLIENTS.md`](docs/MCP_CLIENTS.md) documented
  the full handshake and instructed the reader to point Claude Code, Codex or Cursor at the
  endpoint. All three would have failed on their first call.

  Everything needed was already implemented and tested - `McpProtocolHandler`,
  `McpSessionRegistry`, `protocolDelegateFor`, and an `AppConfig::mainThreadInvoker` field
  whose documentation already said `McpProtocolHandler` consumed it. Only the lines
  connecting them were missing, and the session limits configurable through
  `--remote-max-sessions` and `--remote-idle-timeout-seconds` now reach the registry that
  enforces them.

  Two tests were added over the composed application, because the absence of exactly this
  test is what let the gap survive: the previous MCP composition test used `dispatch()`,
  which keeps the executor envelope and cannot see the difference.


- **Two export containers the tool surface advertises could never work.** Both were found
  by exporting real footage through the MCP surface rather than by a test, and both failed
  after the encoder had already opened, so the whole export was lost:
  - `mkv` was passed to FFmpeg unchanged, but FFmpeg's muxer is named `matroska`, not
    `mkv` (`Requested output format 'mkv' is not known`), which surfaced as the far less
    helpful *"could not allocate an output context"*. The other three accepted containers
    happen to match their muxer names, which is why this survived. `ffmpegMuxerName` now
    performs the mapping.
  - `webm` always got an AAC audio track, and WebM carries only Opus or Vorbis, so the
    muxer refused the file when the header was written even though libvpx-vp9 had opened
    successfully. `audioCodecForContainer` now picks a codec the container can carry.

  Both are pure functions with tests, so the mapping is asserted without FFmpeg. Verified
  by re-exporting on the L4 box: `mkv` now produces H.264 through `h264_nvenc` and through
  `libx264`, and `webm` produces VP9 with an honest software-fallback reason (that GPU has
  no VP9 encoder).

### Added

- **A health and error-tracking surface.** `GET /health` on the MCP endpoint answers with
  the process state, version, uptime, one entry per component, and counts of what the log
  has carried including a per-category breakdown of failures and the most recent one. A
  separate path rather than a tool call, so a container runtime or a person with `curl`
  can ask without speaking JSON-RPC or holding a session.
  - `degraded` is served as **200**, only `unavailable` as 503: no GPU and no audio
    device are supported ways to run this editor, and restarting the process for either
    would destroy a usable one.
  - Error tracking **decorates the existing log sink** rather than adding a parallel
    reporting path, so anything logged is counted by construction and a failure cannot be
    recorded that the log does not also carry. The sink is installed before any component
    is built, because the failures raised during composition are the interesting ones.
  - When remote access is enabled the endpoint is behind the **same admission gate** as
    `/mcp`; an unauthenticated remote peer gets 401 and the report is never built. A test
    asserts the served key set is exactly the five documented ones, so no credential,
    path or host can be added to it without that decision being made again.
  - 40 tests, including a probe that throws, a clock that goes backwards, concurrent
    counting from eight threads, and the refusal path. See
    [`docs/OBSERVABILITY.md`](docs/OBSERVABILITY.md).

### Changed

- **No production translation unit exceeds 1000 lines any more.** The last two did:
  `services/ProjectStore.cpp` (1082) and `services/ExportCoordinator.cpp` (1080). Each is
  now one unit per subject, none over 500 lines:
  - the project store splits into the JSON machinery
    ([`ProjectStoreJson.hpp`](src/services/ProjectStoreJson.hpp)), the format vocabulary,
    the writers, the readers, and the public save/load surface that remains
    `ProjectStore.cpp`;
  - export coordination splits into the worker, request validation, progress reporting and
    the agent tool surface.

  The JSON parser and writer are deliberately kept in a header of their own: nothing in
  them knows about projects, clips or tracks.

### Fixed

- Two faults in the tooling used to perform these splits, both caught before they reached
  the tree. Naive brace counting placed the anonymous namespace 700 lines early in
  `ProjectStore.cpp`, because a JSON parser contains `case '{':` — which would have made
  every linkage decision wrong; and a definition's extent was measured from the first
  brace at signature depth, which truncated any constructor with a member-initialiser list.

## [0.3.0] — 2026-09-10

Hardware encode works. That is the headline, and it was not a driver, SDK or build-flag
problem: two lines that were never written meant no GPU could ever have been selected.
Proven on real NVIDIA L4 hardware through CI, not asserted.

### Added

- **A measured NVENC result.** The `l4-validation` job ran for the first time in this
  repository's history, on a self-hosted NVIDIA L4 (driver 580.173.02), and passed all
  ten steps: `h264_nvenc`, 300 frames of 1920x1080, no software fallback. See
  [`docs/HARDWARE_ENCODE.md`](docs/HARDWARE_ENCODE.md).
- Seven tests over the Vulkan-extension-to-codec mapping, runnable without a GPU.

### Fixed

- **Hardware encode could not be selected on any GPU.** `GpuCaps::encodeCodecs` and
  `decodeCodecs` were declared and read in four places that gate hardware selection —
  `EncoderSelector` requires `hwEncode && encodeCodecs` to contain the codec — but nothing
  ever populated them. Empty on every device, so the export silently used libx264 no
  matter what the hardware supported. They are now filled from the Vulkan video extensions
  the device advertises, gated by the encode/decode queue bits.
- **The hardware/software comparison test could not have passed either.** Its fixture
  hard-coded `GpuContext::softwareFallback()` while the skip above it gated on the live
  device, so on a capable host it proceeded and then measured a software export.
- **A `PALMIER_BUILD_UI=OFF` build did not compile**, and the `l4-validation` job's CTest
  filter matched a test only when the build was broken. Both found by running that job's
  own sequence on real hardware for the first time.
- **The 0.2.0 release was packaged as 0.1.0.** The git tag has no relationship to
  `PROJECT_VERSION`, and four files carried the version independently with nothing
  comparing them. All four now agree and a repository-consistency check enforces it. The
  `v0.2.0` tag remains in history with that defect; 0.3.0 supersedes it.

### Changed

- **`core/EditCommands.cpp` is one translation unit per subject.** It held all 24 command
  implementations and their shared helpers in 1583 lines; it is now six subject files, the
  largest 411 lines, plus a private `EditCommandSupport.hpp` for the six helpers more than
  one subject uses. The commands are declared in the public `core/EditCommands.hpp`, which
  is unchanged, so their member definitions already had external linkage and moved
  untouched.
- **CI no longer repeats work.** `push` is restricted to `main` and release tags, so a
  branch with an open pull request no longer runs the whole pipeline twice; a dispatch for
  the L4 lane runs only that job; a tag push runs only `package`; and coverage is skipped
  when a commit changes nothing that could move a coverage figure.
- The format gate checks the **lines** a commit changed rather than whole files, so a fix
  in a file that predates `.clang-format` no longer requires reformatting all of it.

## [0.2.0] — 2026-09-09

Engineering-quality release. No change to what the editor does; substantial change to how
much of it is proven, reproducible and observable.

At a glance: the suite grew from 1652 to 1699 cases, line coverage is measured for the
first time (80.3%) and gated at 78%, CI grew from four jobs to six, the largest source
file shrank from 2767 lines to 160, and the diagnostic surface went from twelve
`std::cerr` writes to a structured logger.

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

- **The editor tool surface is one translation unit per subject.**
  `services/ToolRegistry.cpp` held the registry container, every shared helper and all
  42 tool factories in a single anonymous namespace across 2767 lines; it is now 160
  lines holding just the container and the registration order, alongside ten files of
  which the largest is 417 lines. Symbols were promoted out of the anonymous namespace
  only where more than one of the new files uses them, so internal linkage is kept
  wherever it is still correct. The published surface is unchanged — the same 42 tools
  in the same order with the same schemas and handlers — so `tools/list` output and
  `docs/TOOLS.md` are untouched.
- **The tool surface's source list is named once.** Twenty-two standalone test targets
  compile these sources directly rather than linking `Palmier::services`, deliberately,
  to avoid its Qt/FFmpeg/libsecret/lcms2 dependencies. They now share
  `PALMIER_TOOL_SURFACE_SOURCES`, so adding a translation unit is a one-line change
  instead of twenty-three edits that can silently disagree.
- Project schema `1.4` → `1.5`, adding `Effect::resourcePath` for the LUT file. The key
  is omitted entirely when empty, so a project with no LUT is byte-identical to the
  previous format apart from the version string.
- The audio engine is now driven from the transport. Before this, `AudioEngine::pump()`
  and `start()` were called from no production code, which meant audio was never
  audible in a real build and the new level meter would have read zero.
- Export and Preview are now asserted to render identically at different sizes, rather
  than that being inferred from their sharing a compositor.

### Fixed

- **Hardware encode could not be selected on any GPU.** `GpuCaps::encodeCodecs` and
  `decodeCodecs` were declared and read in four places that gate hardware selection —
  `EncoderSelector` requires `hwEncode && encodeCodecs` contains the codec — but nothing
  ever populated them, so they were empty on every device and the export silently used
  libx264 no matter what the hardware supported. They are now filled from the Vulkan
  video extensions the device advertises (`VK_KHR_video_encode_h264` and friends), gated
  by the encode/decode queue bits. The mapping is a pure function so it can be tested
  without a GPU, which is precisely what nothing did before.
- **The hardware/software comparison test could not have passed either.** Its fixture
  hard-coded `GpuContext::softwareFallback()` while the skip above it gated on the live
  device, so on a host with a hardware encoder the test proceeded and then measured a
  software export. It now creates the context the same way the skip decision and the
  production composition root do.
- **A `PALMIER_BUILD_UI=OFF` build did not compile.** `tests/ui/shell_unit_test.cpp` and
  `tests/ui/shell_layout_property_test.cpp` each keep a no-Qt fallback branch that
  registers one recorded skip, but `#include <gtest/gtest.h>` sat inside the Qt branch,
  so `TEST` was undefined in the fallback. No CI lane built with the UI off, so nothing
  had ever compiled that path. Found by running the `l4-validation` job's own sequence
  on real NVIDIA L4 hardware.
- **The `l4-validation` job's CTest filter matched no test.** It filtered on the target
  name, but `palmier_register_test` registers through `gtest_discover_tests`, so test
  names are GoogleTest's. The target name existed as a test name only via the
  `…_NOT_BUILT` sentinel CMake adds when an executable is missing — so the filter matched
  a test only when the build was broken, and nothing when it was healthy.
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
