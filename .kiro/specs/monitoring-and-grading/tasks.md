# Implementation Plan

Derived from `requirements.md` in this directory. Phases are ordered so each one lands something a
user can see immediately, cheapest-per-unit-of-value first.

Effort labels are relative, not calendar estimates: **XS** = a pure function plus its tests, **S** =
one new well-bounded class or a few gestures, **M** = a new subsystem with tests, **L** = a new
subsystem plus GPU kernel and software-parity work.

Every task follows this repository's established verification practice: commit, push, confirm CI green
with an explicit `"100% tests passed, 0 tests failed out of N"` line extracted from the run log, and
only then mark the task complete. The standing pre-push checks accumulated across the previous three
specs apply throughout, in particular:

- A new tool needs its entry in `tool_registry_schema_test.cpp`'s `expectedSurface()` (confirm the
  current tool count by grep first — it is **40** as of `1495a6d`), a `drawValidInvocation()` case in
  `mcp_protocol_property_test.cpp` if it needs a real project entity, an `observer.run()` call in
  `documentation_consistency_test.cpp`, and a `docs/TOOLS.md` section.
- A new array-of-primitive or cross-field argument needs its own look at **both** Class 1 (cross-field
  relations) and Class 2 (array item shape) in `tool_schema_conformance_property_test.cpp` — the two
  are independent, and checking one is no evidence about the other (lesson from usable-editor
  incident 25).
- A new `.cpp` reached by `palmier_app_composition_tests`, `palmier_services_offline_mode_tests` or
  `palmier_e2e_tests` must be added to that target's own source list in `tests/CMakeLists.txt`,
  determined by grepping for real call sites rather than assumed.
- Any `docs/UPSTREAM_PARITY.md` re-score needs both fragile-anchor classes checked in
  `report_parser_test.cpp` (numbered build-order anchors and row-content literal anchors) and every
  rationale verified at ≤200 code points against the checked-in file.
- A new `.cmake` file or any file under `src/`, `tests/` or `cmake/` needs the
  `SPDX-License-Identifier: GPL-3.0-or-later` header in its leading comment block
  (`repository_hygiene_property_test.cpp`).

---

## Phase 1 — Audio monitoring

Blocker A. Nothing here needs a GPU or a new render path. Task 1 is deliberately first because it is
a pure reduction over a buffer the mixer already produces.

- [ ] 1. Programme output level metering (Requirement 1) — **S**
  - [ ] 1.1 Add a pure, Qt-free level computation over an `media::AudioBuffer` returning per-channel
        peak and RMS in normalised units, with no dependency on a sink or a device.
  - [ ] 1.2 Report the levels on the existing `media::AudioQuantumReport` (extending it rather than
        adding a second observation channel), computed from the exact buffer `AudioEngine::pump()`
        submits, so the measurement cannot diverge from what was heard.
  - [ ] 1.3 Report zero levels for a `suppressed` quantum, keeping "no device" distinguishable from
        "silent timeline" by the existing flag rather than by the levels.
  - [ ] 1.4 Add a Qt level-meter widget to the Editor_Shell showing per-channel peak and RMS, with a
        distinct at-or-above-full-scale indication held for at least 1 second, a peak-hold decaying no
        faster than 20 dB/s, and a fall to zero when the transport stops.
  - [ ] 1.5 Tests: peak and RMS of hand-computable buffers (silence, full-scale DC, a known sine,
        asymmetric channels); a suppressed quantum reports zero; metering changes neither the submitted
        sample values nor the submitted frame count; the clip indication persists across repaints; the
        meter zeroes on stop.

- [ ] 2. Clip audio waveforms in the timeline (Requirement 2) — **M**
  - [ ] 2.1 Add a peak-envelope computation producing min/max pairs over fixed-width source-time
        buckets, reading through the same decoder path playback uses.
  - [ ] 2.2 Run the computation off the project/UI thread, reusing the existing worker/teardown-queue
        idiom rather than introducing a new threading model, and keep the shell responsive while it runs.
  - [ ] 2.3 Add a bounded, LRU-evicting per-asset envelope cache shared by every clip referencing that
        asset.
  - [ ] 2.4 Draw the envelope inside each audio clip rectangle in `ui::TimelineGraphView`, mapped
        through the clip's `sourceIn` and trim so a horizontal position corresponds to the source time
        actually played there — not merely to the clip's width.
  - [ ] 2.5 Redraw within 200 ms on trim/move/split without re-reading the file; draw nothing and report
        nothing for an asset with no audio stream; report a computation failure once, keep the clip
        editable, and do not retry per repaint.
  - [ ] 2.6 Tests: bucket boundaries and min/max values against a synthesised asset with known content;
        a trimmed clip draws the correct source window; the cache is reused across clips and evicts under
        pressure; an audio-less asset yields no envelope and no error; a failing asset is reported once.

- [ ] 3. Scrub audio (Requirement 3) — **S/M**
  - [ ] 3.1 Play programme audio at the dragged position while the playhead is dragged in
        `ui::TimelineGraphView`, stopping within 200 ms of the drag ending and restoring the transport's
        prior playing/stopped state.
  - [ ] 3.2 Make scrub audio suppressible by a user-visible setting and suppress it automatically when no
        output device is available, in neither case slowing the drag.
  - [ ] 3.3 Drop audio rather than delay the drag when the decoder cannot keep up, keeping the dragged
        playhead visually responsive.
  - [ ] 3.4 Tests: a drag produces audio and stops on release; the transport state is restored exactly;
        the setting and the no-device case both suppress it; the project, the undo history and the
        committed playhead are unchanged beyond the drag's own seek.

## Phase 2 — Colour grading

Blocker B. Every task here adds a GPU kernel **and** its byte-mirrored software reference, and each is
bound by property P5's per-channel tolerance of 1 on a 0-255 scale.

- [ ] 4. Lift / gamma / gain primary grade (Requirement 4) — **M**
  - [ ] 4.1 Extend `color_grade` to per-channel lift, per-channel gamma and per-channel gain alongside
        the existing saturation, choosing defaults that reproduce today's output exactly.
  - [ ] 4.2 Fix and document the operation order — gain, lift, gamma, saturation — in the kernel, and
        mirror it exactly in `gpu::applyColorGrade` so the two cannot drift.
  - [ ] 4.3 Accept every new parameter through the existing `timeline.set_effect_parameter` as one
        undoable edit; round-trip each through save/open.
  - [ ] 4.4 Present lift, gamma and gain in the Inspector as three grouped per-channel controls plus
        saturation, each showing its current value.
  - [ ] 4.5 Tests: a project saved before the change renders byte-identically after it (the hard
        backward-compatibility criterion); GPU/CPU parity across the new parameter space; each parameter
        round-trips; operation order is asserted rather than assumed.

- [ ] 5. Tone curves (Requirement 5) — **L**
  - [ ] 5.1 Add a tone-curve effect type through all seven sites of audit finding 4, with master plus
        independent R/G/B channels.
  - [ ] 5.2 Implement a documented, deterministic interpolation between control points, identical on the
        GPU and software paths; empty and single-point curves are the identity, not an error.
  - [ ] 5.3 Clamp output without wrapping, so an aggressive curve cannot produce an overflow hue shift.
  - [ ] 5.4 Expose adding, moving and removing a control point on the Tool_Surface, each one undoable;
        round-trip the points' coordinates and order through save/open. (Control points are pairs, so
        Class 2 array-item-shape conformance needs its own check here.)
  - [ ] 5.5 Add a directly editable curve control to the Inspector showing the current transfer function.
  - [ ] 5.6 Tests: identity cases; a known curve's transfer function on both paths; GPU/CPU parity;
        determinism across repeated evaluation; clamping at both ends; point add/move/remove undo.

- [ ] 6. Video scopes (Requirement 6) — **M**
  - [ ] 6.1 Add pure, Qt-free, GPU-free histogram, luma-waveform and vectorscope computations over an
        RGBA8 frame buffer.
  - [ ] 6.2 Feed them from the output of the same `gpu::Compositor` instance the Preview displays, so a
        scope cannot disagree with the picture beside it, and so all effects and burn-in are reflected.
  - [ ] 6.3 Add the three scopes to the Editor_Shell, individually hideable with visibility persisted
        across restart, updating at least 10×/second without costing the Preview more than 10% of its
        presented frame rate.
  - [ ] 6.4 Present an explicit empty state when no frame is available, rather than a stale reading.
  - [ ] 6.5 Tests: fully black, fully white and full-saturation-primary frames each produce the
        documented asserted reading on all three scopes; the empty state is reached with no project; the
        Preview's frame-rate budget is respected.

- [ ] 7. LUT application (Requirement 7) — **L**
  - [ ] 7.1 Add one optional string field to `core::Effect` for a resource path and bump the project
        schema; absent means "no resource", matching the `captionText` / `tags` precedent. Sweep for
        stale hardcoded schema-version literals immediately after the bump.
  - [ ] 7.2 Add a `.cube` parser accepting `LUT_3D_SIZE`, the data table, comments and blank lines, and
        rejecting a malformed file with an error naming the fault — including a declared size that
        disagrees with the actual row count.
  - [ ] 7.3 Add the LUT effect through all seven sites, applying the table by trilinear interpolation on
        both the GPU and software paths within P5's tolerance.
  - [ ] 7.4 Render un-graded and report the path when a referenced LUT is missing at open, without
        failing the open, dropping the effect, or blocking editing.
  - [ ] 7.5 Expose applying a LUT by path on the Tool_Surface as one undoable edit.
  - [ ] 7.6 Tests: an identity LUT is a no-op within tolerance (the shared parser/interpolator anchor); a
        known LUT's output on both paths; every malformed-file rejection names its fault; a pre-schema
        project opens unchanged; a missing LUT degrades as specified.

## Phase 3 — Parity, determinism and documentation

- [ ] 8. Close the parity rows and the documentation (Requirement 8) — **S**
  - [ ] 8.1 Add a test that fails if any `core::EffectType` reachable in the core is absent from the
        Tool_Surface's closed value set, so the seven-site pattern is enforced rather than remembered.
  - [ ] 8.2 Assert export and Preview agree for every effect this spec adds, rather than inferring it
        from the shared compositor.
  - [ ] 8.3 Document every new tool and argument in `docs/TOOLS.md`; keep the documentation-consistency
        suite green.
  - [ ] 8.4 Re-score `docs/UPSTREAM_PARITY.md`: `audio scrub and metering` (capability area) and `color`
            (tool category), plus the `color and effects` capability area, recording per row why the
            status changed; update `docs/PORT_BACKLOG.md` if any entry maps to this work; advance
            `linux-ref`; recompute the build-order counts.
  - [ ] 8.5 Confirm `OfflineModeAvailability` still passes: every new subsystem must work with no network
        and no generative-AI account.

---

## Sequencing notes

**Task 1 before everything.** It is the smallest change with the largest visible effect, and it
establishes the level-computation seam that scrub audio (task 3) and any later audio work read from.

**Task 4 before 5 and 7.** Extending the existing `color_grade` walks the GPU-kernel-plus-software-
mirror path on an effect that already exists and already has parity coverage, which is a much cheaper
place to discover a parity problem than inside a brand-new curve or LUT kernel.

**Task 6 is independent of 4, 5 and 7** and could move earlier if grading turns out to need a
measurement to develop against — scopes read the compositor's output and know nothing about which
effects produced it.

**Task 7 carries the only schema bump** in this spec (1.4 → 1.5). Per the standing lesson from
usable-editor task 15, the whole repository must be swept for hardcoded schema-version literals
immediately after the bump rather than after a CI failure.
