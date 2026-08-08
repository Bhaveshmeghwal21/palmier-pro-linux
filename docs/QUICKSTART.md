<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Quickstart

This is the Requirement 16.6 document: the launch → import → add tracks → place clip → play → save →
re-open → export walkthrough with the observable result of each step, plus remediation for no
compatible GPU, unavailable audio output and a refused MCP connection.

It describes **what the tree does today**, verified against the sources, not what the specification
plans. Where a step is not yet drivable, it says so instead of inventing a gesture. Two facts shape
the whole document:

- **The five-panel editor shell is not implemented yet.** `ui::MainWindow` is a shell with a menu bar
  (File → Quit, and an empty Edit menu) and a central placeholder label. There is no timeline,
  preview, inspector, media browser or agent-chat panel to click, so the walkthrough below is driven
  through the **tool surface** — the same `tools/call` path the GUI is intended to route through.
- **A headless build produces no executable.** `palmier-pro` is added only when `PALMIER_BUILD_UI=ON`
  *and* Qt 6 Widgets is found. `build-nogui` builds and tests every library, but it has no binary to
  launch, so **launching the editor requires Qt 6**.

## 0. Build

The headless tree — the primary verification tree, and the one to use on a host without Qt:

```sh
PKG_CONFIG_PATH=/usr/local/lib/pkgconfig \
  cmake -S . -B build-nogui -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPALMIER_BUILD_UI=OFF
cmake --build build-nogui -j"$(nproc)"
ctest --test-dir build-nogui --output-on-failure
```

`PKG_CONFIG_PATH=/usr/local/lib/pkgconfig` is **required for a fresh configure** on this project's
own hosts: FFmpeg is built from source into `/usr/local`, and it is discovered purely through
`pkg-config`, which does not search `/usr/local/lib/pkgconfig` by default. Without it the configure
either fails on the FFmpeg probe or silently compiles the FFmpeg paths as stubs. It is needed on the
*configure* step only — `cmake --build` and `ctest` do not re-run the probes.

The UI tree, which is the one that produces the launchable binary and therefore needs Qt 6:

```sh
PKG_CONFIG_PATH=/usr/local/lib/pkgconfig \
  cmake -S . -B build-ui -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPALMIER_BUILD_UI=ON
cmake --build build-ui -j"$(nproc)"
xvfb-run -a ctest --test-dir build-ui --output-on-failure
```

If the configure summary reports the UI as off, or `build-ui/bin/palmier-pro` does not appear, Qt 6
was not found — and the usual cause is missing OpenGL/GLX development packages rather than a missing
Qt. [`BUILD.md`](BUILD.md) has the diagnosis, the package names per distribution and the rest of the
environment traps.

## 1. Launch

```sh
./build-ui/bin/palmier-pro
```

**Observable result.** The platform gate runs first and silently on a supported host; the window then
appears with its menu bar and placeholder. The MCP endpoint is bound during startup, so the editor is
now reachable at `http://127.0.0.1:19789/mcp`. Nothing is printed on a successful start.

On an unsupported host — wrong architecture, glibc older than 2.31, or an unloadable required library
— the gate names **every** unmet item on stderr and in a dialog, and **no window opens**. That is the
correct behaviour, not a crash.

`palmier-pro` accepts no command-line arguments today: the settings resolver that understands
`--mcp-port` and the `PALMIER_*` environment variables exists but is not consumed by the entry point,
so the endpoint is always `127.0.0.1:19789`.

## 2. Connect

Complete the MCP handshake — `initialize`, then `notifications/initialized` with the returned
`Mcp-Session-Id`, then `tools/list`. The `curl` form and the per-client configuration entries are in
[`MCP_CLIENTS.md`](MCP_CLIENTS.md); every step below is a `tools/call` on that session.

**Observable result.** `tools/list` returns 22 tools. Every argument and result field is in
[`TOOLS.md`](TOOLS.md).

Each call below is the `params` of a `tools/call` request:

```json
{"jsonrpc":"2.0","id":2,"method":"tools/call",
 "params":{"name":"project.create","arguments":{ ... }}}
```

## 3. Create a project

`project.create` with `{"name":"Quickstart","fps":30,"width":1920,"height":1080}`.

**Observable result.** `projectId`, the echoed `name`, `fps` as the exact rational
`{"numerator":30,"denominator":1}`, `canvas`, `colorSpace` of `Rec.709` (the default),
`modified: false` and `documentPath: null` — a project that has never been written.

Until this succeeds, every other tool except `project.open` refuses with `no project is open`, having
changed nothing.

## 4. Import media

`media.import` with `{"path":"/absolute/path/to/clip.mp4"}`.

**Observable result.** `assetId`, `sourcePath`, `containerFormat`, `durationMs`, `hasVideo`,
`hasAudio`, `duplicate: false`. `width`, `height` and `fps` appear **only** if the file carries a
decodable video stream — their absence means "audio only", not "unknown". Importing the same file
again succeeds with `duplicate: true` rather than registering a second asset.

## 5. Add tracks

`timeline.add_track` with `{"kind":"video"}`, then again with `{"kind":"audio"}`.

**Observable result.** Each call returns `trackId`, the echoed `kind`, the `index` the track was
inserted at, the new `trackCount` and `"status":"applied"`. A track is an ordinary undoable edit:
`edit.undo` removes it.

## 6. Place a clip

`timeline.add_clip` with the video `trackId`, the `assetId` from step 4, `timelineStartNs: 0` and a
`sourceOutNs` inside the asset's duration — for two seconds, `2000000000`.

**Observable result.** `clipId` and `"status":"applied"`. Confirm with `timeline.read`, which returns
the whole project: the track now carries a clip whose `durationNs` is `sourceOutNs - sourceInNs`, and
the project `durationNs` has grown to cover it.

`sourceOutNs` at or below `sourceInNs` is refused outright, and a placement that overlaps an existing
clip outside that clip's explicit transition region is applied, found to break the timeline invariants
and **rolled back**, so the timeline is left exactly as it was either way.

## 7. Play

**Not drivable today.** The playback transport (`ui::PreviewController`, the decode pool and the
audio graph) is wired into the composition root, but there is no playback tool on the tool surface and
no preview panel in the window, so neither an agent nor a user can currently start playback. The
transport is exercised by its own unit and property tests; the end-to-end walkthrough that plays 24
consecutive frames under a controlled clock is task 12.10 and is **not yet written** — there is no
`tests/e2e/` directory in the tree.

Everything before and after this step works; only this one has no operator-visible path yet.

## 8. Save

`project.save` with `{"path":"/absolute/path/to/quickstart.palmier"}`.

**Observable result.** `documentPath` — the path actually written — `bytesWritten` greater than zero,
and `modified: false`. After this, `project.save` with **no** arguments rewrites the same path; called
with no argument on a project that was never saved, it refuses and tells you `path` is required rather
than guessing a destination.

## 9. Re-open

`project.open` with the same path.

**Observable result.** `projectId`, `name`, `trackCount: 2`, a `clipCount` matching what you placed,
the `documentPath` and `modified: false`. Compare against the step-6 `timeline.read` output — the
round trip must agree.

A missing, unreadable or malformed document is reported with the path and the reason, and the project
that was current **stays** current with its document path, modified flag and undo history intact.

## 10. Export

`timeline.export` with `{"outputPath":"/absolute/path/out.mp4","format":"mp4"}`. Width, height, frame
rate, codec and bit rate all default from the project and the container, so those four are the only
arguments needed.

**Observable result.** `outputPath`, `framesEncoded` equal to `plannedFrames`, the `encoderName` that
actually ran, `usedHardwareEncode`, `usedSoftwareFallback` (never both true), `containsAudio`,
`durationNs` and `projectModified: false`.

Replacing an existing file is never implied: without `"overwrite": true` an existing destination is
preserved and the request refused.

**If the export fails to open an encoder,** the host has no encoder for the codec. That is the state
of this project's own sandbox and CI host — libavcodec built with no `libx264`, `libx265` or
`libvpx`, and no vendor encoder — so no H.264, HEVC or VP9 export can produce bytes there at all.
A partially written file is removed rather than left behind. [`HARDWARE_ENCODE.md`](HARDWARE_ENCODE.md)
explains how to confirm this and what to install.

## Remediation

### No compatible GPU

Nothing to do — this is a supported configuration, not a fault. `gpu::GpuContext` degrades to CPU
processing and records a non-blocking notice, one of:

- *No compatible GPU was found; using CPU processing.*
- *GPU detection exceeded the time budget; using CPU processing.*
- *Selected GPU is not usable for compute; using CPU processing.*

Editing, playback, saving, opening and exporting all continue; only compositing and hardware encode
are affected. The notice is available to the shell through `gpuUnavailableNotice()` and is intended
for the status bar; since the status bar is part of the unimplemented shell, today the notice is
visible to code and tests rather than on screen.

`libvulkan.so.1` is different: it is a **required** runtime library in the launch gate. A missing
loader stops startup with the library named, and installing the Vulkan loader package for your
distribution is the fix — see [`BUILD.md`](BUILD.md).

### Audio output unavailable

Also a supported configuration. The sink is selected at startup in the order PipeWire → ALSA → null
sink. When neither real backend is compiled in or neither can open a device, the always-available null
sink is selected: **audio is suppressed, video keeps running**, and a notice is raised that names
every candidate and its reason, beginning *Audio output is unavailable; playback continues without
sound.*

To get real audio: install the runtime library (`libpipewire-0.3` or `libasound2`), confirm the
matching option is `ON` and the SDK was found in the configure summary, then reconfigure the tree from
scratch — a stale cache will keep reporting the old detection result.

### The MCP connection is refused

| Symptom | Cause and remedy |
|---|---|
| Connection refused | The editor is not running, or its endpoint never bound. A port conflict on 19789 prints a warning at startup and shows a non-blocking dialog, and the editor keeps running **without** the endpoint — so a refused connection with a live window is almost always the port. Free port 19789 (`ss -ltnp \| grep 19789`) and restart; the port is not configurable from the running binary today. |
| Connection refused, and no window | The launch gate stopped startup. Read the stderr message: it names every unmet item. |
| 404 on every request | The only served path is `/mcp`. |
| 405 | The client issued `GET`, usually to open an event stream. There is no `GET /mcp`; use the stdio bridge form in [`MCP_CLIENTS.md`](MCP_CLIENTS.md). |
| Handshake stalls after `initialize` | The client rejected the zero-byte HTTP 202 answer to `notifications/initialized`, or is not echoing `Mcp-Session-Id`. |
| Every call answers `no project is open` | Expected until `project.create` or `project.open` succeeds. |

Exposing the endpoint beyond loopback is a separate, opt-in operation with its own prerequisites:
[`REMOTE_ACCESS.md`](REMOTE_ACCESS.md).

## Next

- Full build reference: [`BUILD.md`](BUILD.md)
- Client configuration: [`MCP_CLIENTS.md`](MCP_CLIENTS.md)
- Every tool, argument and result field: [`TOOLS.md`](TOOLS.md)
- Hardware encode and its validation: [`HARDWARE_ENCODE.md`](HARDWARE_ENCODE.md)
