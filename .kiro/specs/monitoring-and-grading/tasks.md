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

- [x] 1. Programme output level metering (Requirement 1) — **S**
  - [x] 1.1 Add a pure, Qt-free level computation over an `media::AudioBuffer` returning per-channel
        peak and RMS in normalised units, with no dependency on a sink or a device.
  - [x] 1.2 Report the levels on the existing `media::AudioQuantumReport` (extending it rather than
        adding a second observation channel), computed from the exact buffer `AudioEngine::pump()`
        submits, so the measurement cannot diverge from what was heard.
  - [x] 1.3 Report zero levels for a `suppressed` quantum, keeping "no device" distinguishable from
        "silent timeline" by the existing flag rather than by the levels.
  - [x] 1.4 Add a Qt level-meter widget to the Editor_Shell showing per-channel peak and RMS, with a
        distinct at-or-above-full-scale indication held for at least 1 second, a peak-hold decaying no
        faster than 20 dB/s, and a fall to zero when the transport stops.
  - [x] 1.5 Tests: peak and RMS of hand-computable buffers (silence, full-scale DC, a known sine,
        asymmetric channels); a suppressed quantum reports zero; metering changes neither the submitted
        sample values nor the submitted frame count; the clip indication persists across repaints; the
        meter zeroes on stop.

Completed at `dbadeeb` (run `33419535167`, **1397/1397**, +26 cases over the 1371 baseline), with **zero
CI incidents of its own** — it went green first try. The one incident of this phase belonged to the
spec commit that preceded it and is recorded below.

What shipped, and the two decisions that shaped it:

- `media::measureLevels()` (new) reduces an `AudioBuffer` to the new `media::AudioLevels` — per-channel
  peak as the maximum absolute sample, RMS as the root mean square. It sits in `AudioGraph`'s existing
  pure buffer-math section beside `mix()`, because audit finding 1 held: the mixer already produces
  exactly the samples a meter needs, so no new decode, no second pass and no separate tap were required.
  It is read-only and total — a degenerate buffer measures **empty rather than erroring**, which is what
  lets Requirement 1.3 fall out with no special case, since a suppressed quantum is zero-filled silence
  and therefore measures zero naturally. `AudioQuantumReport::suppressed`, not the levels, remains the
  discriminator between "no output device" and "the timeline is genuinely silent here".
- **The meter's timing rules live in a Qt-free view model, and time is an argument.**
  `ui::AudioMeterViewModel::update()` is *told* the instant its levels belong to rather than reading a
  clock. Requirement 1.5 (a ≥ 1 s clip indication) and Requirement 1.6 (a ≤ 20 dB/s hold decay) are both
  stated in real time, and this is the only reason they are asserted by driving simulated time instead of
  by sleeping — the suite gains 14 timing cases that run in microseconds. Because the decay depends only
  on elapsed time, a caller that repaints twice as often cannot decay twice as fast; that is asserted
  directly (ten 100 ms updates decay exactly as much as one 1000 ms update).
- **The meter went into the existing transport bar, not a fifth dock.** `shell_unit_test.cpp` asserts the
  shell has exactly four docks, and that assertion is correct: a level meter is not a panel. This was
  found *before* pushing, by checking the assertion rather than discovering it in CI, and the new shell
  cases re-assert the dock count alongside the meter's presence so the choice stays deliberate.
- `ui::AudioMeterWidget` takes its data through two `std::function` seams rather than a reference to the
  composition root, so it depends on neither `ApplicationComposition` nor `AudioEngine`; `MainWindow` —
  the only class that can reach both the engine and the transport — installs the closures. Thread
  affinity was checked rather than assumed: `AudioEngine` documents single-thread affinity for its mixing
  calls and spawns no threads, and the pump that drives it runs on the GUI thread, as does the meter's
  timer, so the read is same-thread.

### CI incident 1 — a latent race in an unrelated test, surfaced by a documentation-only commit

- **Incident 1 — `ExportCoordinatorTest.ADestroyedCoordinatorCancelsAndLeavesNoPartialFile` (run
  `33417375130`, commit `f03f269`, 1 failure).** The failing commit added *only* this specification —
  three files under `.kiro/specs/`, no source, no CMake, no test — so it could not have caused a test of
  export cancellation to fail. The test carried a latent race and lost the coin toss. Its own comment
  claimed it "parks the worker in the first frame, then destroys the coordinator", but the hook merely
  set a `std::promise` and returned immediately, which parks nothing: the worker was free to encode all
  four frames and call `guard.commit()` before the main thread reached `reset()`. A committed
  `OutputGuard` deliberately keeps its file, so the closing `EXPECT_FALSE(exists(out))` failed — the
  export was no longer in flight, which is the single precondition the test needs. Fixed at `2f19717`
  (run `33418699369`, 1371/1371) by making the hook genuinely **block**: it signals `reached`, then waits
  — bounded, so a mistake fails the test rather than hanging the suite — until released. `reset()` now
  runs on a helper thread so the release can come from outside the destructor's `join()`, and because
  `~ExportCoordinator` does `cancel()` then `joinWorker()`, the cancel flag is already set when the
  worker is released, so it observes cancellation at its next frame boundary and the uncommitted guard
  removes the partial file. Pushed on its own, ahead of any Task 1 code, so the metering work started
  from a confirmed-green 1371 baseline instead of an ambiguous one.
- **The lesson, added to the standing pre-push doctrine:** a test whose comment says it *parks* or
  *blocks* a worker must actually block it. A hook that only signals converts a deterministic ordering
  into a race that passes for as long as the scheduler is kind, and then fails against an unrelated
  commit — which is the worst time to debug it, because every instinct points at the innocent change.

- [x] 2. Clip audio waveforms in the timeline (Requirement 2) — **M**
  - [x] 2.1 Add a peak-envelope computation producing min/max pairs over fixed-width source-time
        buckets, reading through the same decoder path playback uses.
  - [x] 2.2 Run the computation off the project/UI thread, reusing the existing worker/teardown-queue
        idiom rather than introducing a new threading model, and keep the shell responsive while it runs.
  - [x] 2.3 Add a bounded, LRU-evicting per-asset envelope cache shared by every clip referencing that
        asset.
  - [x] 2.4 Draw the envelope inside each audio clip rectangle in `ui::TimelineGraphView`, mapped
        through the clip's `sourceIn` and trim so a horizontal position corresponds to the source time
        actually played there — not merely to the clip's width.
  - [x] 2.5 Redraw within 200 ms on trim/move/split without re-reading the file; draw nothing and report
        nothing for an asset with no audio stream; report a computation failure once, keep the clip
        editable, and do not retry per repaint.
  - [x] 2.6 Tests: bucket boundaries and min/max values against a synthesised asset with known content;
        a trimmed clip draws the correct source window; the cache is reused across clips and evicts under
        pressure; an audio-less asset yields no envelope and no error; a failing asset is reported once.

Completed across four commits, each green first try — **zero CI incidents for this task**:

| Commit | What landed | Suite |
|---|---|---|
| `f878bcc` (run `33421008474`) | `media::PeakEnvelope`, `PeakEnvelopeBuilder`, `PeakEnvelopeCache` | **1428/1428** (+31) |
| `37fb7f0` (run `33421827252`) | `extractPeakEnvelope` through `MediaDecoder`'s audio surface | **1440/1440** (+12) |
| `576b266` (run `33423157259`) | `PeakEnvelopeService` (worker + dedup), cache entries to `shared_ptr` | **1454/1454** (+14) |
| `08f8800` (run `33424699310`) | `sourceWindowForColumn`, the drawing, and the composition wiring | **1464/1464** (+10) |

It was deliberately split into four verified commits rather than one large one: the arithmetic had to be
right before anything was built on it, and a green baseline after each step meant a later failure could
only have come from the step that introduced it.

The decisions worth keeping:

- **Buckets are min/max pairs in SOURCE time, not magnitudes in clip time.** Both bounds are kept
  because audio is signed and often asymmetric — collapsing a bucket to one magnitude draws a symmetric
  shape that no longer corresponds to the signal and loses DC offset entirely. Positions are source time
  so one envelope serves every clip referencing the asset however each was placed or trimmed
  (Requirement 2.5).
- **Bucket boundaries come from the absolute frame counter, never accumulated per buffer.** A decoder
  hands over ragged blocks whose sizes are an artefact of the container; if boundaries drifted with block
  size the same asset would draw differently on different days. Asserted directly, twice — once on the
  builder with eight ragged chunk sizes, once end-to-end through the extractor with two very different
  block layouts producing identical envelopes.
- **`frameToSourceTime` splits into whole seconds plus a remainder.** The obvious
  `frame * kTicksPerSecond / sampleRate` overflows a signed 64-bit tick count within a day of 48 kHz
  audio and wraps into negative source times.
- **Three outcomes, and only one of them is an error.** An envelope; an EMPTY envelope reported as
  *success* for an asset with no audio (Requirement 2.6); an error only for a file that would not open, a
  stream the build refuses, or a failed decode (Requirement 2.7). Conflating the middle with the last
  would put an error in front of the user for a video with no soundtrack, so the cache stores silence as
  an *answer* rather than as an absence — otherwise the cheapest case would become the most expensive
  one, re-decoding a silent asset on every repaint forever.
- **`lookup()` never blocks and never decodes.** That single rule shapes the whole service interface and
  is how Requirement 2.2 became a precondition rather than an aspiration. It also makes deduplication
  load-bearing rather than an optimisation: a repaint asks about every visible clip many times a second,
  so an asset already in flight is not re-queued and a failure is remembered. Both suppressions are
  counted, so 20 lookups yield 1 scheduled job and 19 suppressions, and 50 repaints of a failed asset
  yield 1 attempt and 1 decoder open.
- **The trim mapping is a named, separately-tested function.** `sourceWindowForColumn` is Requirement 2.3
  as arithmetic. Interpolating over the clip's width alone looks entirely plausible and is wrong — it
  draws the whole asset squeezed into every clip, so two clips cut from different points draw the same
  shape. The tests pin exactly that: a clip trimmed to 40–60 ms starts at 40 ms and not at 0.
- **The read is bounded.** A decode loop that trusts the backend to report end-of-stream hangs forever if
  it never does — a stuck worker in an editor, an expired job in CI. Frame and decode-call limits turn
  that into an ordinary reported failure, and the endless-backend case is tested because without the
  bound that test does not fail, it never returns.
- **Cache entries hold the envelope by `shared_ptr`.** A renderer resolves an asset once per repaint and
  then reads per pixel column; if eviction could free it mid-loop the only safe alternatives would be
  copying the whole envelope per clip per frame or holding the cache's lock for the entire paint.
- **The ready callback is guarded by a liveness token.** The composition, and therefore the worker, can
  outlive the window, so a late completion must not dereference a destroyed view. The token is a
  `MainWindow` member, and members are destroyed before the `QWidget` base destroys its children — so by
  the time the view could dangle, the `weak_ptr` has already expired. This was designed in, not found by
  a crash.
- **Digital silence draws a centre line, not nothing.** "Silent" and "no waveform available yet" must not
  look identical to the user.

Applying the lesson from incident 1: every place a test needed the worker parked mid-flight, the gate
genuinely **blocks** (bounded, so a mistake fails rather than hangs). A gate that only signalled would
let the worker finish and turn every "while in flight" assertion into a coin toss.

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
