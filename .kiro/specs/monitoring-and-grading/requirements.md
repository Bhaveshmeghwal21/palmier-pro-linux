# Requirements Document

## Introduction

The `usable-editor` spec is complete: all 17 of its tasks are CI-green, the editor installs from a
Debian package, launches, renders, and completes a real import → edit → export workflow against four
codecs (`h264`, `hevc`, `av1`, `vp9`) with 1370 of 1371 tests passing (run `33401126985`).

A readiness review of that finished tree identified three things standing between "a working editor"
and "an editor someone would choose for real work". One of the three — hardware-encode verification —
cannot be closed from this environment, because no runner here has an NVENC/VAAPI/QSV device. This
spec covers **the other two**, both of which are ordinary engineering work with no external
dependency:

> **Blocker A — you cannot hear or see what you are mixing.** There is no level meter, no audio
> waveform on any clip, and no scrub audio. The audio pipeline mixes and outputs correctly, but it
> is entirely un-monitorable: an editor cannot tell whether a mix is clipping, where a beat lands,
> or what a cut sounds like while dragging.
>
> **Blocker B — you cannot grade.** One `color_grade` effect exists (per-channel gain, scalar lift,
> saturation). There is no tone curve, no lift/gamma/gain wheel surface, no scope of any kind, and no
> LUT support — so colour can be nudged but never shaped, and never judged against anything but
> eyesight.

Both are scored in `docs/UPSTREAM_PARITY.md` today: `audio scrub and metering` is `absent` (`should`)
and `color` is `partial` (`should`). Closing this spec should move the first to at least `partial`
and the second toward `present`.

## Scope decision

Three phases, ordered so that each lands something a user can immediately see:

| Phase | Theme | Why this order |
|---|---|---|
| 1 | Audio monitoring — metering, waveforms, scrub audio | Blocker A. Metering alone is cheap and removes the single worst omission: silent, invisible audio |
| 2 | Colour grading — primary wheels, curves, scopes, LUTs | Blocker B. Depends on nothing in Phase 1; sequenced second only because Phase 1 is cheaper per unit of value |
| 3 | Parity, determinism and documentation | Closes the two parity rows and keeps every checked document true, as every prior spec has required |

Phase 1's Requirement 1 (metering) is deliberately first and smallest: it needs no new decode path,
no cache, and no GPU work — only a peak/RMS computation on samples the mixer already produces.

**Explicitly out of scope**, recorded so the boundary is a decision rather than an omission:

- Hardware-encode verification (the third blocker) — needs physical hardware this environment lacks.
- Audio effects of any kind (EQ, compression, limiting). This spec makes audio *observable*, not
  *processable*. A limiter is a different requirement and a different subsystem.
- Secondary/qualified colour correction (masks, keys, power windows). Primary grading only.
- Colour-managed display transforms beyond the existing `core::ColorSpace` values.

## Codebase audit findings (verified against source at commit `1495a6d` on `main`)

Every absence below was confirmed by pattern search across `src/`, and every present component by
reading the named file. Each row is re-checkable rather than asserted.

### What already exists and must be built on rather than replaced

| # | Finding | Evidence |
|---|---|---|
| 1 | **The audio mixer already produces exactly the samples a meter needs.** `media::AudioGraph::mix()` combines every contributing source into one interleaved `AudioBuffer` per quantum, and `media::AudioEngine::pump()` hands that buffer to the sink. A meter needs no new decode, no second pass and no separate tap — only a reduction over a buffer that already exists. | `src/media/AudioGraph.cpp` (`mix`); `src/media/AudioEngine.cpp` (`pump`), `src/media/AudioSink.hpp`. |
| 2 | **Per-quantum reporting already exists and is already observed.** `media::AudioQuantumReport` carries the window's range, frame count, submitted/suppressed flags and a per-clip `AudioContribution` list. It is the established channel for making audio behaviour inspectable rather than merely audible. | `src/media/AudioEngine.hpp:143-183`. |
| 3 | **`ColorGrade` is a real primary grade already, not a stub.** Its GLSL kernel applies per-channel gain, then a scalar lift, then a Rec.601 luma-mix saturation; `gpu::applyColorGrade` is the byte-exact software mirror. | `src/gpu/EffectKernels.cpp:123-140` (`kColorGradeSrc`); `src/gpu/Compositor.cpp:237` (`applyColorGrade`). |
| 4 | **Adding an effect type is an established seven-site pattern**, already walked twice (`CropTransform`, `InvertColors`). Every new effect must land in all seven or it is unreachable, unserialisable or undocumented. | `core::EffectType` (`src/core/Effect.hpp:25`); `gpu::EffectKernel` + GLSL + registry (`src/gpu/EffectKernels.{hpp,cpp}`); `gpu::applyEffectSoftware` (`src/gpu/Compositor.cpp:284`); `ProjectStore` kind strings (`src/services/ProjectStore.cpp:473,485`); `ToolRegistry` name map **and** the closed value set (`src/services/ToolRegistry.cpp:166,180,190`); `GuiToolGateway` name map (`src/ui/GuiToolGateway.cpp:32`); `InspectorPanel` display name (`src/ui/InspectorPanel.cpp:42`). |
| 5 | **GPU/CPU parity is enforced with a hard, numeric bound.** Property P5 asserts per-channel `abs(gpu - cpu) <= 1` on a 0-255 scale over a generated image set. Every kernel this spec adds is subject to it. | `tests/gpu_gpu_cpu_parity_property_test.cpp:70` (`kParityTolerance = 1`). |
| 6 | **The preview and the export share one `gpu::Compositor` instance**, established by usable-editor tasks 12–14. Any scope reading "what the viewer shows" must read that same compositor output, not a second render path. | `src/app/ApplicationComposition.cpp`; `src/app/main.cpp` (one `Compositor`, one `QtTextRasterizer`, one `QtImageEncoder`). |

### The absences this spec closes

Each confirmed by a word-boundary search over every `.cpp`/`.hpp` under `src/`, returning zero
matches:

| # | Absent component | Search that returned nothing |
|---|---|---|
| 7 | Output level meter (peak or RMS) | `LevelMeter`, `AudioMeter`, `MeterWidget`, `peakDb`, `rmsDb` |
| 8 | Audio waveform / peak envelope | `AudioWaveform`, `WaveformView`, `PeakEnvelope`, `waveform` |
| 9 | Scrub audio | `ScrubAudio`, `scrubAudio` |
| 10 | Tone curve | `ToneCurve`, `CurveEffect`, `curvePoints` |
| 11 | LUT support | `LUT`, `.cube`, `CubeLut`, `LutEffect` |
| 12 | Any scope | `histogram`, `vectorscope`, `VideoScope`, `ScopeView`, `ScopePanel` |
| 13 | A `gamma` parameter anywhere | `gamma` |

### Two structural findings that constrain the design

These are the reason this spec carries design constraints rather than only behaviour:

| # | Finding | Consequence |
|---|---|---|
| 14 | **`core::Effect::parameters` is `std::map<std::string, double>` — scalars only.** There is nowhere on an `Effect` to put a LUT's file path. | A LUT cannot be expressed as parameters alone. `Effect` needs one new optional string field (schema bump), which Requirement 7 specifies. Routing a LUT through `core::MediaAssetRef` instead is rejected on purpose: a LUT would then appear in the Media_Browser library and in `media.list` output alongside footage, which misdescribes it — it has no duration, decodes nothing, and is never placed on a track. |
| 15 | **`ColorGrade` has per-channel gain but only a *scalar* lift, and no gamma at all.** Conventional lift/gamma/gain wheels are per-channel in all three bands. | Requirement 4 extends the existing effect rather than adding a competing one, and is bound by a hard backward-compatibility criterion: the extended kernel must render an existing project **byte-identically** to the current one, which pins the new parameters' defaults (`liftR/G/B` = the old scalar `lift`, `gammaR/G/B` = 1). |

---

## Requirement 1: Output Level Metering

**User Story:** As an editor, I want to see the programme output's level while it plays, so that I can
tell whether my mix is clipping before I export it — today the mix is completely invisible.

1. THE Audio_Engine SHALL compute, for every mixed quantum, the per-channel peak sample magnitude and
   the per-channel RMS magnitude of the buffer it submits to the sink, and SHALL report both on the
   existing `media::AudioQuantumReport` rather than through a new observation channel.
2. THE reported peak SHALL be the maximum absolute sample value in the quantum for that channel, and
   THE reported RMS SHALL be the root mean square over the same samples, both in normalised units
   where 1.0 is full scale.
3. WHEN a quantum is suppressed because no output device is available (the existing
   `suppressed` case, Requirement 6.7 of the predecessor spec), THE reported peak and RMS SHALL both
   be zero, and SHALL be distinguishable from a genuinely silent timeline by that existing flag
   rather than by the levels alone.
4. THE Editor_Shell SHALL present a level meter showing the current per-channel peak and RMS of the
   programme output, updating at least 10 times per second while playing.
5. THE meter SHALL indicate a peak at or above full scale distinctly from any level below it, and
   THE indication SHALL persist for at least 1 second after the peak that caused it, so a single
   over-scale sample cannot be missed between two repaints.
6. THE meter SHALL show a peak-hold indication that decays no faster than 20 dB per second.
7. WHEN playback stops, THE meter SHALL fall to zero rather than freezing at its last value, so a
   stopped transport is never mistaken for a silent one.
8. Metering SHALL NOT alter the samples submitted to the sink, SHALL NOT change the number of frames
   submitted, and SHALL NOT introduce an additional buffering stage: the measurement is a read-only
   reduction over the buffer the mixer already produced.
9. THE metering computation SHALL be pure and separately testable, taking a buffer and returning
   levels, with no dependency on Qt, on a sink, or on a device being present.

## Requirement 2: Clip Audio Waveforms In The Timeline

**User Story:** As an editor, I want to see each clip's audio as a waveform on the timeline, so that I
can cut on a transient or a word boundary without scrubbing blindly.

1. THE system SHALL compute, for an audio-carrying asset, a peak envelope: a sequence of
   min/max sample pairs over fixed-width source-time buckets, at a resolution sufficient to draw the
   asset at the timeline's current zoom.
2. THE envelope computation SHALL run off the thread that owns the project and the UI, and THE
   Editor_Shell SHALL remain responsive while it runs.
3. THE Timeline_Panel SHALL draw the envelope inside each clip rectangle on an audio track, aligned so
   that a given horizontal position in the clip corresponds to the source time actually played at that
   timeline position — that is, honouring the clip's `sourceIn` and any trim, not merely the clip's
   width.
4. WHEN a clip is trimmed, moved, or split, THE drawn envelope SHALL follow the clip's new source range
   within 200 milliseconds without recomputing the asset's envelope from the file again.
5. THE computed envelope SHALL be cached per asset and reused by every clip referencing that asset, and
   THE cache SHALL be bounded, evicting least-recently-used entries rather than growing without limit.
6. IF an asset carries no audio stream, THEN THE Timeline_Panel SHALL draw no envelope for it and SHALL
   report no error, matching the existing "silent, not failed" distinction the Audio_Engine already
   draws.
7. IF envelope computation fails for an asset, THEN THE failure SHALL be reported once, THE clip SHALL
   still draw and remain fully editable, and THE failure SHALL NOT be retried on every repaint.
8. THE envelope SHALL be derived from the same decoder path playback uses, so that what is drawn and
   what is heard cannot disagree about the asset's content.

## Requirement 3: Scrub Audio

**User Story:** As an editor, I want to hear the audio while I drag the playhead, so that I can find a
cut point by ear rather than by counting frames.

1. WHEN a user drags the playhead across the timeline, THE system SHALL play the programme audio at the
   dragged position.
2. THE scrub audio SHALL stop within 200 milliseconds of the drag ending, and SHALL leave the transport
   in the same state (playing or stopped) it was in before the drag began.
3. Scrub audio SHALL be suppressible by a user-visible setting, and SHALL be suppressed automatically
   when no output device is available, in both cases without blocking or slowing the drag itself.
4. Scrub audio SHALL NOT modify the project, THE undo history, or the playhead's committed position
   beyond the seek the drag itself performs.
5. IF the decoder cannot supply audio fast enough to keep up with the drag, THEN THE system SHALL drop
   audio rather than delay the drag, and THE dragged playhead SHALL remain visually responsive.

## Requirement 4: Lift / Gamma / Gain Primary Grade

**User Story:** As a colourist, I want per-channel control of shadows, midtones and highlights, so that
I can balance an image rather than only brighten or saturate it.

1. THE `color_grade` effect SHALL accept per-channel lift, per-channel gamma and per-channel gain, in
   addition to the saturation it already accepts.
2. THE extended effect SHALL render an existing project **byte-identically** to the current
   implementation when the new parameters hold their defaults, and THE defaults SHALL be chosen to make
   this true (each channel's lift defaulting to the previous scalar `lift`, each gamma defaulting to
   1.0, each gain defaulting to the existing per-channel gain).
3. A project saved before this change SHALL open without error and SHALL render identically after it,
   with no user action and no migration step.
4. THE GPU kernel and THE software reference SHALL agree within the existing per-channel tolerance of 1
   on a 0-255 scale (property P5), for every combination of the new parameters.
5. THE order of operations SHALL be documented in the kernel and mirrored exactly by the software path,
   so the two cannot drift: gain, then lift, then gamma, then saturation.
6. THE Tool_Surface SHALL accept every new parameter through the existing
   `timeline.set_effect_parameter`, and each change SHALL be one undoable edit.
7. THE Inspector SHALL present the nine grade values as three grouped controls (lift, gamma, gain),
   each with a per-channel component, plus saturation, and SHALL show the current value of each.
8. Every new parameter SHALL round-trip through save and open, preserving its exact value.

## Requirement 5: Tone Curves

**User Story:** As a colourist, I want a curve I can shape, so that I can lift shadows without washing
out highlights — the one operation no combination of lift/gamma/gain can express.

1. THE system SHALL provide a tone-curve effect that maps input to output intensity through a set of
   user-placed control points.
2. THE curve SHALL support a master (luma) channel at minimum, and SHALL support independent red,
   green and blue curves.
3. THE curve SHALL be evaluated identically by THE GPU kernel and THE software reference, within
   property P5's per-channel tolerance of 1 on a 0-255 scale.
4. THE curve's interpolation between control points SHALL be documented and deterministic: the same
   control points SHALL always produce the same transfer function, on both paths, on every host.
5. A curve with no control points, or with a single point, SHALL be the identity transform rather than
   an error.
6. THE effect SHALL clamp its output to the representable range without wrapping, so an aggressive
   curve cannot produce a hue-shifted artefact from overflow.
7. THE Tool_Surface SHALL expose adding, moving and removing a control point, and each SHALL be one
   undoable edit.
8. THE control points SHALL round-trip through save and open, preserving both their coordinates and
   their order.
9. THE Inspector SHALL present the curve as a directly editable control showing the current transfer
   function, not merely as a numeric list.

## Requirement 6: Video Scopes

**User Story:** As a colourist, I want a histogram, a waveform monitor and a vectorscope, so that I can
judge exposure and colour against a measurement instead of against an uncalibrated screen.

1. THE Editor_Shell SHALL present a histogram, a luma waveform monitor and a vectorscope of the frame
   currently shown in the Preview.
2. Every scope SHALL be computed from the output of the same `gpu::Compositor` instance the Preview
   displays, so a scope can never disagree with the picture beside it.
3. Every scope SHALL therefore reflect all applied effects, text and caption burn-in, exactly as the
   viewer does.
4. THE scope computations SHALL be pure functions of a frame buffer, independent of Qt and of any GPU
   being present, and SHALL be separately testable against hand-checkable inputs.
5. THE scopes SHALL update at least 10 times per second during playback, and SHALL NOT reduce the
   Preview's own presented frame rate by more than 10 percent while doing so.
6. WHEN no frame is available (no project, or the playhead is past the end), every scope SHALL present
   an explicit empty state rather than a stale or misleading reading.
7. THE scopes SHALL be individually hideable, and their visibility SHALL persist across a restart.
8. A fully black frame, a fully white frame and a full-saturation primary SHALL each produce the
   documented, asserted reading on every scope, so a wrongly scaled axis is caught by test rather than
   by eye.

## Requirement 7: LUT Application

**User Story:** As a colourist, I want to apply a `.cube` LUT, so that I can use a look my client
supplied or a camera vendor published, instead of rebuilding it by hand.

1. THE system SHALL apply a 3D LUT loaded from a `.cube` file to a clip, as an effect in the existing
   effect chain.
2. `core::Effect` SHALL gain one optional string field carrying the LUT's path, and THE project schema
   SHALL be bumped accordingly. A LUT SHALL NOT be modelled as a `core::MediaAssetRef`, for the reason
   recorded in audit finding 14.
3. A project saved before this change SHALL open without error, with the new field absent meaning "no
   resource", exactly as `Clip::captionText` and `MediaAssetRef::tags` already establish.
4. THE `.cube` parser SHALL accept the format's documented essentials — `LUT_3D_SIZE`, the data table,
   comments and blank lines — and SHALL reject a malformed file with an error naming what was wrong,
   rather than applying a partially read table.
5. IF a LUT's declared size and its actual row count disagree, THEN THE file SHALL be rejected as
   malformed.
6. THE LUT SHALL be applied by trilinear interpolation, and THE GPU kernel and THE software reference
   SHALL agree within property P5's per-channel tolerance of 1 on a 0-255 scale.
7. An identity LUT SHALL leave every pixel unchanged within that same tolerance, which is the parser's
   and the interpolator's shared correctness anchor.
8. IF the referenced LUT file is missing or unreadable when a project is opened, THEN THE clip SHALL
   render un-graded, THE effect SHALL remain in the chain, and THE failure SHALL be reported naming the
   path — a missing LUT SHALL NOT fail the open, drop the effect, or block editing.
9. THE Tool_Surface SHALL expose applying a LUT by path, and it SHALL be one undoable edit.

## Requirement 8: Parity, Determinism And Documentation

**User Story:** As anyone reading this repository, I want its documents to describe what this spec
actually built, so that the parity report stays trustworthy.

1. Every effect this spec adds SHALL satisfy property P5's GPU/CPU parity bound, and SHALL be
   registered in the `gpu::EffectKernelRegistry` alongside the existing kernels.
2. Every effect this spec adds SHALL be present in all seven sites audit finding 4 enumerates, and a
   test SHALL fail if any effect type is reachable in the core but absent from the Tool_Surface's closed
   value set.
3. Every new tool and every new tool argument SHALL be documented in `docs/TOOLS.md`, and the existing
   documentation-consistency tests SHALL pass unchanged.
4. Export SHALL produce the same picture as the Preview for every effect this spec adds, since both
   already render through one `gpu::Compositor` — asserted, not assumed.
5. `docs/UPSTREAM_PARITY.md` SHALL be re-scored for the `audio scrub and metering` and `color` rows
   (and the `color and effects` capability area), recording per row why the status changed, and
   `docs/PORT_BACKLOG.md` SHALL be updated in the same change if any entry maps to this work.
6. Every new subsystem SHALL work with no network connection and with no generative-AI account, and the
   existing `OfflineModeAvailability` suite SHALL continue to pass.
7. The whole suite SHALL pass with an explicit CTest count recorded for each landed task, matching the
   verification practice every prior spec in this repository has followed.
