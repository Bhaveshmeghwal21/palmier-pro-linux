<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Hardware encoding

This is the Requirement 16.5 document: for each of NVENC, Quick Sync and VAAPI, the driver and
runtime prerequisites, the CMake option that compiles that encode path in, and a command that
confirms the device is usable on the current host — plus the NVIDIA L4 validation procedure with its
fixture, its command and the values it records.

Every statement below is taken from `src/media/EncoderSelector.{hpp,cpp}`, `src/gpu/CodecBridge.cpp`,
`src/app/ApplicationComposition.cpp` (`codecBackendReport()`), `cmake/PalmierOptions.cmake` and
`tests/support/HardwareSkip.hpp`.

**Hardware encoding is never required.** Software encoding is the path every other path degrades to,
so a host with no GPU, no vendor SDK and no vendor driver exports correctly — only slower. Nothing
about hardware encode can block startup, configuration or an export.

## The three vendor paths

| Path | CMake option (default `ON`) | Compile guard | Detected via | Encoder-name suffix |
|---|---|---|---|---|
| NVENC / NVDEC (NVIDIA) | `PALMIER_ENABLE_NVENC` | `PALMIER_HAVE_NVENC` | the `ffnvcodec` headers (`nv-codec-headers`) | `_nvenc` |
| VAAPI (Intel / AMD) | `PALMIER_ENABLE_VAAPI` | `PALMIER_HAVE_VAAPI` | `libva` + `libva-drm` | `_vaapi` |
| Quick Sync (Intel) | `PALMIER_ENABLE_QSV` | `PALMIER_HAVE_QSV` | oneVPL (`vpl`), falling back to `libmfx` | `_qsv` |
| FFmpeg software | *(none — always compiled in)* | — | — | *(named directly, see below)* |

A hardware encoder name is assembled from the codec prefix (`h264`, `hevc`, `vp9`, `av1`) and the
backend suffix above, giving names such as `h264_nvenc` and `hevc_vaapi`. A codec/backend pair with no
prefix or no suffix yields an **empty** name, and an empty name selects software instead of guessing.
The names are composed mechanically; whether libavcodec on a given host actually carries a particular
pair is a host question, answered by the verification commands below.

The software encoder names are fixed per codec: `libx264` (H.264), `libx265` (HEVC), `libvpx-vp9`
(VP9), `libsvtav1` (AV1), `mpeg2video` (MPEG-2). Of these, `timeline.export` offers only H.264, HEVC
and VP9 — the three codecs in its `codec` enum.

An option being `ON` is a **request**, not a claim. The path is compiled in only when the option is
`ON` *and* the SDK was found at configure time; a missing SDK is never a configuration failure. The
option/SDK matrix, the native package names per distribution and the configuration summary that
reports the three states of each path are in [`BUILD.md`](BUILD.md).

### Driver and runtime prerequisites

These are *runtime* prerequisites, separate from the build-time SDKs above. The build never checks
them and their absence is not an error — it is a software fallback with a recorded reason.

| Path | Needs at run time |
|---|---|
| NVENC | the proprietary NVIDIA driver and its kernel modules, i.e. `/dev/nvidia*` present and `nvidia-smi` working. NVENC is a driver feature; the `ffnvcodec` headers alone encode nothing |
| VAAPI | a DRM render node (`/dev/dri/renderD128`) and the vendor VA driver — Mesa `iHD`/`i965` for Intel, Mesa `radeonsi` for AMD — reachable through `libva` |
| Quick Sync | the same render node plus the Intel media stack (`intel-media-va-driver-non-free` or the oneVPL GPU runtime); on Linux the QSV route runs over VAAPI, so a host where VAAPI does not work will not do QSV either |

`libvulkan.so.1` is a separate concern: it is required at launch for GPU compositing and is in the
launch gate, whereas the vendor codec libraries are deliberately **not** in the launch gate.

## Confirming the host can really encode

Three levels, cheapest first.

**1. Is the device there, from the vendor's own tools?**

```sh
nvidia-smi                                    # NVENC: driver loaded, device listed
ls -l /dev/dri/renderD*                       # VAAPI and QSV: a render node exists
vainfo | grep -i enc                          # VAAPI and QSV: VAEntrypointEncSlice entries
vulkaninfo --summary                          # the device Palmier itself will select
```

`vainfo` is in `libva-utils` (Debian/Ubuntu `vainfo`, Fedora `libva-utils`, Arch `libva-utils`) and
`vulkaninfo` is in the `vulkan-tools` package already listed in [`BUILD.md`](BUILD.md).

**2. Does libavcodec on this host actually carry the encoders?** This is the check people skip, and
on a from-source FFmpeg it is usually the one that fails:

```sh
ffmpeg -hide_banner -encoders | grep -E 'nvenc|vaapi|qsv|libx264|libx265|libvpx-vp9'
```

If the `ffmpeg` CLI is not installed — a `--disable-programs` build has none — the equivalent
question is answered by level 3, which asks the library directly.

**3. Ask Palmier.** The project has its own answer, and it is the authoritative one because it is the
same code the export uses. Run the hardware/software comparison test and read its verdict:

```sh
ctest --test-dir build-nogui -R palmier_services_export_hw_sw_comparison_tests -V
```

On a capable host the comparison runs. On an incapable host it is reported **skipped with a recorded
reason naming what is absent** (Requirement 15.5), and the reason distinguishes the two independent
causes:

- no vendor path compiled in — the reason names the undefined guards, e.g. *no vendor hardware encode
  path is compiled in (`PALMIER_HAVE_NVENC`, `PALMIER_HAVE_VAAPI`, `PALMIER_HAVE_QSV` all
  undefined)*;
- a path is compiled in but the selected device cannot — the reason names the device and the codec,
  e.g. *no VAAPI-capable device reported for H.264 encode on "Software (CPU)"*;
- no **software** encoder to compare against — the reason names the missing FFmpeg encoder, e.g.
  *libavcodec on this host carries no software H.264 encoder ("libx264")*.

When both halves are missing the recorded reason names both, labelled separately, so it never implies
the hardware was the only problem.

Inside a running application the same question is answered by
`ApplicationComposition::codecBackendReport()`, which returns four entries — `vaapi`,
`nvenc-nvdec`, `qsv` and `ffmpeg-software` — each with `compiledIn`, `usableOnHost` and a `detail`
string. It reads the build-time guards and the capabilities the single `GpuContext` already probed at
construction; it starts no probe of its own, changes no encoder selection and touches no export
state. `ffmpeg-software` is reported as compiled in **and** usable unconditionally: a build in which
it were not could not export at all.

There is currently **no command-line flag that prints this report** — `palmier-pro` accepts no
arguments — so it is reachable from application code and tests only.

## What actually happens when an export asks for hardware

`timeline.export` takes `preferHardware` (default `true`). That is a request; `media::EncoderSelector`
decides, exactly once per export, and the decision is reported back in the tool result
(`encoderName`, `usedHardwareEncode`, `usedSoftwareFallback`, `fallbackReason` — see
[`TOOLS.md`](TOOLS.md)).

1. **Software immediately, with no probe at all**, when hardware was not requested, when no hardware
   encode path is compiled in for the selected device's vendor, or when the device does not list the
   codec as encodable. A host with no GPU therefore never pays the probe cost.
2. Otherwise the **capability probe**, run on a *detached* thread and awaited with a 3000 ms deadline.
   The thread is detached deliberately: a wedged vendor driver must not be able to block an export.
   A timeout is treated as "no compatible device" and selects software.
3. On a positive probe, the vendor encoder — and if hardware initialisation then fails before the
   first frame, initialisation is retried **exactly once** before selecting the software encoder for
   the same codec, with the requested resolution, frame rate and bit rate **unchanged**.

One export produces exactly one encoder. `usedHardwareEncode` and `usedSoftwareFallback` can never
both be true: the two are set by the only two constructors of the selection value, so a contradictory
result is unconstructible rather than merely unwritten. A software selection that was what the caller
asked for (`preferHardware: false`) is **not** a fallback and carries no reason.

## The software fallback is not a free pass

The fallback selects the FFmpeg software encoder for the codec — `libx264`, `libx265` or
`libvpx-vp9`. Those are *external* libraries that FFmpeg must have been built with. If libavcodec on
the host carries none of them, there is nothing to fall back **to**: opening the software encode route
fails, the export is refused with the libavcodec error, and the partially written file is removed
rather than left behind.

**This is the situation on the sandbox and CI host this project is developed on.** libavcodec
60.31.102 is built from source into `/usr/local` with no `libx264`, `libx265`, `libvpx` or `libaom`,
and there is no vendor encoder either — so the host has **no H.264, HEVC or VP9 encoder at all**,
hardware or software. Decoding works; encoding does not. That is precisely why
`palmier_services_export_hw_sw_comparison_tests` **skips** here, for both reasons at once, and why the
assertions of Requirement 8.6 are written but **unverified**: they must be exercised on a host with a
real encoder stack before that task counts as verified. A skip here is the correct, required outcome —
not a failure, and not a pass.

To make the comparison runnable, rebuild FFmpeg with the external encoders (at minimum `--enable-gpl
--enable-libx264`) and reconfigure the build tree from scratch, since only a fresh configure re-runs
the `pkg-config` probes.

## The NVIDIA L4 validation procedure (Requirement 8.5)

Hardware encode on real silicon cannot be validated on the CI runners, which have no GPU. It is a
separate lane, run on a self-hosted NVIDIA L4 host or by an operator.

**Fixture.** The Requirement 8.6/8.5 fixture timeline: **300 frames, 1920×1080, 30 frames per
second** — the geometry and frame count `export_hardware_software_comparison_test.cpp` builds
(`kFixtureFrames = 300`, `Resolution::hd1080()`, `FrameRate::fps30()`), with a deterministic frame
painted for every clip position so both runs feed the encoder an identical pixel sequence.

**Codec.** H.264, so the encoder under test is `h264_nvenc`.

**Command.** On the L4 host, configure with the NVIDIA path on and the `ffnvcodec` headers present,
then run the same test that gates itself on hardware:

```sh
PKG_CONFIG_PATH=/usr/local/lib/pkgconfig \
  cmake -S . -B build-l4 -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DPALMIER_BUILD_TESTS=ON -DPALMIER_BUILD_UI=OFF -DPALMIER_ENABLE_NVENC=ON
cmake --build build-l4 -j"$(nproc)"
ctest --test-dir build-l4 -R palmier_services_export_hw_sw_comparison_tests -V \
  2>&1 | tee ctest-l4.log
python3 scripts/l4_validation_report.py ctest-l4.log --summary l4-summary.md
```

A **skip** on that host is a failed validation, not a pass: it means the NVENC path was not compiled
in, the device was not selected, or there is no software H.264 encoder to compare against. Read the
recorded reason, fix the named cause, and re-run. Note that `ctest` exits **0** for a skip, which is
why the verdict is the report script's and not `ctest`'s: with no measurements recorded the script
exits 2.

**The CI lane.** The `l4-validation` job in [`../.github/workflows/ci.yml`](../.github/workflows/ci.yml)
runs exactly the sequence above. It is gated three ways, because no hosted runner has the device:

- it runs only on a `workflow_dispatch` whose `run_l4_validation` input is ticked, so no push and no
  pull request reaches it;
- it requires a self-hosted runner labelled `nvidia-l4` (`runs-on: [self-hosted, linux, x64,
  nvidia-l4]`), so it cannot land on a hosted runner even by accident;
- nothing `needs:` it, so its absence never holds up the normal graph.

The job installs nothing: the driver, the `ffnvcodec` headers and an FFmpeg carrying `h264_nvenc`
and `libx264` are host state, checked at the start of the job with `nvidia-smi` and
`pkg-config --exists ffnvcodec` so a missing prerequisite is named up front instead of appearing
later as a skip reason. It then asserts the configuration summary reports NVENC as *enabled (SDK
found)* before it measures anything.

**Values recorded.** Requirement 8.5 requires three values as job output. The hardware run of
`export_hardware_software_comparison_test.cpp` prints them, one key per line, between
`--- BEGIN PALMIER L4 MEASUREMENTS ---` and `--- END PALMIER L4 MEASUREMENTS ---`;
`scripts/l4_validation_report.py` lifts that block out of the `ctest -V` log and writes the values to
`$GITHUB_OUTPUT`:

| Value | Emitted as | Job output | Where it comes from |
|---|---|---|---|
| the selected encoder name | `PALMIER_L4_ENCODER_NAME` | `encoder-name` | the `encoderName` field of the export result — must be `h264_nvenc` |
| elapsed wall-clock time in milliseconds | `PALMIER_L4_ELAPSED_MS` | `elapsed-ms` | a `steady_clock` bracket around the hardware export, `begin()` through `awaitCompletion()` |
| output file size in bytes | `PALMIER_L4_OUTPUT_BYTES` | `output-bytes` | `file_size()` of the written file — must be greater than 0 |

Three more are recorded because Requirement 8.10 judges by them: `PALMIER_L4_USED_HARDWARE_ENCODE`
(`used-hardware-encode`), `PALMIER_L4_SOFTWARE_FALLBACK` (`software-fallback`) and
`PALMIER_L4_FALLBACK_REASON` (`fallback-reason`), plus `PALMIER_L4_FRAMES_ENCODED`
(`frames-encoded`) so the 300-frame fixture count is on the record. The job also publishes
`validation-status` (`passed`/`failed`) and `validation-detail`.

The validation **fails** if the selected encoder is anything other than `h264_nvenc`, if the
software-fallback flag is true, or if the output is 0 bytes — and it retains the measurements as job
output in every case, including the failing ones. That ordering is deliberate on both sides: the test
prints the block *before* it asserts anything about the values, and the script writes the job outputs
*before* it applies the verdict, so a failing run cannot lose its measurements. A run that recorded
nothing at all — the test skipped, was filtered out, or the build never got that far — fails too,
with a summary saying nothing was validated.

The uploaded `l4-validation` artifact carries `ctest-l4.log`, `configure-l4.log`, the generated
`l4-summary.md` and `build-l4/Testing/**`; the same summary is rendered on the run's own page.

## Next

- Options, packages and configure recipes: [`BUILD.md`](BUILD.md)
- The `timeline.export` arguments and result fields: [`TOOLS.md`](TOOLS.md)
