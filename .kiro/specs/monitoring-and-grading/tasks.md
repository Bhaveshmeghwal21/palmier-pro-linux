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

- [x] 3A. Audio playback transport wiring (Requirement 3A) — **S/M**

  Added mid-flight, after Task 2, when wiring Task 3 showed its prerequisite was missing: nothing in
  production ever calls `AudioEngine::start()` or `pump()`, so audio has never been audible and
  Requirement 1's meter reads zero in a real build. Sequenced before task 3 because task 3 depends on it.

  - [x] 3A.1 Add a Qt-free component that observes (transport playing, transport position, engine
        running, mix position, presentation position, scrubbing) and returns an intent: start, stop,
        restart, or pump N quanta.
  - [x] 3A.2 Compute the quanta to pump from the lead the engine has left, bounded per cycle so a slow
        decoder cannot stall the UI thread, and yielding zero when the engine is already far enough ahead.
  - [x] 3A.3 Drive it from the Editor_Shell on a timer, applying the intents to the one
        `media::AudioEngine`, and stand off entirely while scrub audio owns the engine.
  - [x] 3A.4 Keep the missing-device case on the engine's existing null-sink path: the transport still
        runs and audio is suppressed, rather than the wiring refusing to start.
  - [x] 3A.5 Tests: entering and leaving play starts and stops the engine; a seek while playing restarts
        rather than continuing; the pump yields zero when sufficiently ahead and is bounded when far
        behind; scrubbing suppresses the playback wiring and releasing restores it; no output device
        still starts.

Landed across two commits, the first of which failed to compile — recorded as incident 2 below.

| Commit | What landed | Suite |
|---|---|---|
| `22c6986` (run `33504146671`) | `ui::AudioTransportSync`, `ui::AudioPlaybackDriver`, the shell wiring | **failed to compile** |
| `2c2deab` (run `33504812396`) | the one-line include fix | **1506/1506** (+23) |

The +23 is 20 `AudioTransportSync.*` cases plus 3 `ShellUnitTest.*` cases
(`TheShellDrivesTheAudioEngineFromTheTransport`, `SuspendingTheDriverStopsTheEngineAndItsCadence`,
`AScrubGestureTakesTheEngineFromThePlaybackDriver`), all confirmed by name in the run log.

The decisions worth keeping:

- **The decision is a pure function of an observation, and the I/O is a separate 40-line object.**
  `AudioTransportSync::decide()` takes (transport playing, transport position, engine running, mix
  position, presentation position, scrubbing) and returns an action plus a quantum count. It touches no
  engine, no Qt and no clock, which is why the whole of Requirement 3A.5 is testable without an audio
  device, a running event loop or any sleeping — the cadence is simulated by calling `decide()` with
  successive observations. `AudioPlaybackDriver` is then only a `QTimer` that applies what it is told.
- **A seek is inferred, never announced.** Nothing in `PreviewController` signals "the user sought"; there
  is only a playhead that can change between two observations. So the sync compares the transport
  position against where the engine's mix has actually reached and calls it a seek when they have diverged
  beyond a tolerance. This is what makes `HealthyPlaybackIsNeverMistakenForASeek` and
  `ManyConsecutiveHealthyCyclesProduceNoRestartsAtAll` the two most important tests in the file: an
  inference that fires spuriously restarts the engine on every cycle and produces continuous stuttering
  rather than an obvious failure. A backward jump is tested separately from a forward one because a naive
  signed comparison handles only one of them.
- **The pump count is derived from the lead the engine has left, and is bounded both ways.** Zero when far
  enough ahead (so the timer costs nothing during steady playback), one quantum for a small shortfall
  rather than none (so the lead is actually recovered instead of decaying), and a hard per-cycle cap so a
  slow decoder cannot turn one timer tick into an unbounded decode loop on the UI thread — Requirement
  3A.2 is that cap. A negative lead, which is what a stalled sink looks like arithmetically, is treated as
  an empty lead rather than trusted.
- **Scrub suppression is total, including stop.** While a scrub gesture owns the engine the sync returns
  no action at all, rather than "don't start". Two owners issuing `start`/`stop` at the engine on
  different timers is the classic way to get audio that plays only sometimes, and it would be a race
  rather than a reproducible bug. Releasing the scrub restores normal control on the following cycle, and
  that handover is tested in both directions.
- **The missing-device case is deliberately invisible here.** `AudioEngine::start()` already installs a
  null sink and returns success when no device can be opened, so the sync has nothing to decide;
  `TheDecisionDoesNotDependOnWhetherADeviceExists` pins that as an intended property rather than leaving
  it as an accident of the engine's implementation. Requirement 3A.4 is satisfied by *not* adding a
  special case.
- **The defaults are checked against the engine's own geometry rather than restated.** A test asserts the
  driver's cadence and lead target are consistent with `kOutputSampleRate` and
  `kDefaultQuantumFrames`, so changing the engine's quantum size cannot silently leave the driver pumping
  at the wrong rate.

Two things this does not establish. Nothing here has been heard: CI has no audio device, so every test
runs against the null-sink path, and the assertions are about which engine calls are made and with what
arguments. And the *content* of the audio is the engine's existing, already-tested responsibility — this
task supplies only the missing cadence.

### CI incident 2 — a lost newline glued two `#include` directives into one

- **Incident 2 — `TimelinePanel.cpp` and its moc TU failed to compile (run `33504146671`, commit
  `22c6986`).** `PreviewController has not been declared`, then nine consequential errors about
  `transport_`. The cause was mechanical rather than conceptual: an edit intended only to delete a blank
  line also removed the newline after `#include "ui/GuiToolGateway.hpp"`, leaving that include and the
  `PreviewController` one on a single line. The preprocessor reads that as one directive, so
  `PreviewController.hpp` was never included at all. Fixed at `2c2deab` (run `33504812396`,
  **1506/1506**) by restoring the newline; nothing else changed.

- **Why the pre-push check did not catch it.** The structural checker run before every push looks for
  newline loss with `;X` and `}XX` — a statement or a block glued to whatever follows. Neither matches a
  *string* glued to a *directive*, and gluing two includes changes no bracket count, so the brace/paren
  balance check passed as well. The rule is now explicit: a preprocessor directive must begin its own
  line.

- **The checker's own first version was wrong in a more instructive way.** It used `"\s*#\s*include`,
  which reported 700+ hits across the tree — every ordinary pair of consecutive includes — because `\s`
  spans newlines. Restricted to horizontal whitespace (`[^\S\n]`) it reports 4 hits across 335 files, all
  of them `#include` text inside string literals in `repository_hygiene_property_test.cpp` and
  `suite_hygiene_property_test.cpp`. That sweep is what established the `TimelinePanel` line was the
  *only* genuine instance, so no other edit in this spec's work had done the same thing silently.

- **Standing lesson.** A checker reporting implausibly many hits is reporting its own bug, and a clean
  result from an unvalidated pattern is worth nothing. This is the same discipline already applied to the
  brace checker, which is only trusted because its output is diffed against `git show HEAD:file` so
  pre-existing false positives cancel and only the delta is read.

- [x] 3. Scrub audio (Requirement 3) — **S/M**
  - [x] 3.1 Play programme audio at the dragged position while the playhead is dragged in
        `ui::TimelineGraphView`, stopping within 200 ms of the drag ending and restoring the transport's
        prior playing/stopped state.
  - [x] 3.2 Make scrub audio suppressible by a user-visible setting and suppress it automatically when no
        output device is available, in neither case slowing the drag.
  - [x] 3.3 Drop audio rather than delay the drag when the decoder cannot keep up, keeping the dragged
        playhead visually responsive.
  - [x] 3.4 Tests: a drag produces audio and stops on release; the transport state is restored exactly;
        the setting and the no-device case both suppress it; the project, the undo history and the
        committed playhead are unchanged beyond the drag's own seek.

Landed across three commits (plus the controller, green earlier at `355de83`):

| Commit | What landed | Suite |
|---|---|---|
| `355de83` (run `33502760883`) | `ui::ScrubAudioController` — the Qt-free decision logic | **1483/1483** (+19) |
| `68fc43a` (run `33505681581`) | the playhead drag gesture on the ruler | **1509/1509** (+3) |
| `f4467d2` (run `33506638866`) | the panel/MainWindow wiring and the menu setting | **1509/1513** — 4 failed |
| `82eaa92` (run `33507392966`) | the test fix for incident 3 | **1514/1514** (+5) |

The decisions worth keeping:

- **The gesture had to be built before the audio could be attached.** Requirement 3 says audio plays while
  the playhead "is dragged across the timeline", but the ruler only supported a click that jumped to one
  point — there was no gesture at all. `DragKind` gained a `Playhead` member and the ruler now emits
  `playheadDragBegan()` / `playheadDragEnded()` around the `seekRequested` stream it already produced.
- **Two signals, one job each.** `seekRequested(ms)` keeps moving the playhead; the begin/end pair only
  delimits the gesture and carries **no position**. The position scrub audio must play at is the clamped,
  frame-snapped one `movePlayheadToMs()` computes, so emitting a raw coordinate alongside it would invite
  the audio to sit a fraction of a frame away from the picture. A test asserts the started position equals
  the transport's actual playhead rather than the pointer's 500 ms.
- **Dragging is confined to the ruler.** A press on an empty lane also selects that lane as the placement
  target, so treating that as a scrub would start and stop audio on an ordinary selection.
- **The motionless case is the one that leaks.** The clip-drag path treats a zero delta as "a click, not a
  drag" and returns early; a scrub taking that shortcut would never be closed, so audio started on press
  would run forever. The playhead branch therefore returns *before* that check, and a plain ruler click is
  asserted to be a closed begin/end pair with exactly the one seek it always produced.
- **Positions are offered to the controller from inside `movePlayheadToMs()`, guarded on `isDragging()`.**
  All five playhead-moving gestures converge there, so the guard — rather than connecting only to the drag
  paths — is what stops a keyboard step or a timecode edit being mistaken for a scrub, and it makes the
  controller's own state the authority on whether a gesture is running.
- **The applier is handed only the decision.** Pausing on drag start and resuming after `StopAndResume`
  stay in the panel, because the panel owns the transport and an applier able to call `play()` could change
  playback state as a side effect of touching audio. The interruption is deliberately independent of
  audibility: a suppressed drag still moves the playhead, so it still stops and resumes playback, and
  `endDrag()` reports `StopAndResume` even when nothing was heard.
- **`Start` and `Restart` are the same two engine calls.** Repositioning scrub audio *is* stopping and
  starting it — the engine begins mixing from a given position and has no reposition operation, and
  pretending otherwise would leave the old position's audio queued ahead of the new one.
- **Suppression is routed through the panel, not set on the controller.** Either input can change part-way
  through a drag, and both then return a decision that must be performed at once: a user who switches
  scrub audio off wants silence *now*, not at the next mouse move. Only the panel can perform a decision,
  so only the panel is asked to change the inputs.
- **The shell tests assert the conservation law, not the ratio.** A flood of 30 positions must be fully
  accounted for as served-or-dropped; the *proportion* served depends on how long 30 synthetic mouse
  events take on the runner, which is exactly the wall-clock dependency that makes a test pass until it
  does not. The ratio is pinned deterministically in the controller's own simulated-time case, where 100
  positions across a second yield exactly 16 repositions and 84 drops.

### CI incident 3 — a real capability check invalidated tests that predated it

- **Incident 3 — 4 tests failed (run `33506638866`, commit `f4467d2`): 1509 of 1513 passed.** One root
  cause. The new wiring reads `ApplicationComposition::audioOutputAvailable()` and auto-suppresses scrub
  audio when the host has no output device, which is Requirement 3.3 working exactly as specified. CI
  runners have no sound card, so scrub audio is suppressed there, `isScrubbing()` never becomes true and no
  decisions are produced — and four tests asserted the opposite, including
  `ShellUnitTest.AScrubGestureTakesTheEngineFromThePlaybackDriver`, which had been **green since task 3A
  precisely because nothing yet asked whether audio output existed**.

- **The product behaviour was right and the tests were wrong.** Each now calls
  `setScrubAudioOutputAvailable(true)` before driving a gesture, with a comment saying why, so it tests the
  wiring *above* device availability instead of accidentally testing device availability itself. Fixed at
  `82eaa92` (run `33507392966`, **1514/1514**).

- **The behaviour that broke them is now a test of its own.**
  `ScrubAudioFollowsTheHostsActualOutputAvailability` asserts the controller *agrees with*
  `audioOutputAvailable()` rather than asserting either specific value, so it holds on a runner with no
  device and on a developer machine with one. It also pins the part that would be a genuine bug:
  auto-suppression must not flip the user's setting, because a menu item silently unchecked by a missing
  device would leave scrub audio off once the device appeared, with no indication of why and a double
  toggle needed to recover.

- **Standing lesson.** Wiring a real capability check into the shell can invalidate tests that were passing
  only because the capability was never consulted. Expect a capability check to break tests that predate
  it, and prefer asserting *agreement with the host* over asserting a value the host decides.

Also fixed in passing: `MainWindow.hpp` had two pairs of members glued onto single lines by
newline-losing edits (`audioDriver_`/`inspectorPanel_`, and `undoAction_`/`redoAction_` when the same
mistake was repeated while fixing the first). This is incident 2's damage class, but it **compiles**, so CI
could never have caught it. The checker gained a pattern for it, validated against both real damaged lines
*and* against legitimate one-liners — a broad version reported 55 hits that were all deliberate idioms
(`case X: …; break;`, packed struct fields, one-line accessors), which is worth no more than incident 2's
700-hit sweep.

## Phase 2 — Colour grading

Blocker B. Every task here adds a GPU kernel **and** its byte-mirrored software reference, and each is
bound by property P5's per-channel tolerance of 1 on a 0-255 scale.

- [x] 4. Lift / gamma / gain primary grade (Requirement 4) — **M**
  - [x] 4.1 Extend `color_grade` to per-channel lift, per-channel gamma and per-channel gain alongside
        the existing saturation, choosing defaults that reproduce today's output exactly.
  - [x] 4.2 Fix and document the operation order — gain, lift, gamma, saturation — in the kernel, and
        mirror it exactly in `gpu::applyColorGrade` so the two cannot drift.
  - [x] 4.3 Accept every new parameter through the existing `timeline.set_effect_parameter` as one
        undoable edit; round-trip each through save/open.
  - [x] 4.4 Present lift, gamma and gain in the Inspector as three grouped per-channel controls plus
        saturation, each showing its current value.
  - [x] 4.5 Tests: a project saved before the change renders byte-identically after it (the hard
        backward-compatibility criterion); GPU/CPU parity across the new parameter space; each parameter
        round-trips; operation order is asserted rather than assumed.

Two commits, both green first try, no CI incidents:

| Commit | What landed | Suite |
|---|---|---|
| `9cb9b11` (run `33509745453`) | the kernel, the software reference, the parity model | **1520/1520** (+6) |
| `a5f2cb5` (run `33510740057`) | the defaults moved to core, the Inspector, tool/round-trip tests | **1524/1524** (+4) |

**No schema bump was needed, which is worth stating because the spec anticipated one.**
`core::Effect::parameters` is already a `std::map<std::string, double>` that `ProjectStore` serialises
by iterating, so the new names round-trip with no serializer change; and
`timeline.set_effect_parameter` already takes a free-form parameter name and is already undoable with
an exact inverse, including the "had no prior value" case. Requirements 4.6 and 4.8 were therefore
satisfied by existing code, and the work for them was to *prove* it rather than to build it.

The decisions worth keeping:

- **Byte-identity is carried by six defaults, not by a compatibility branch.** Each per-channel lift
  falls back to the LEGACY SCALAR `lift` and each gamma to 1.0, so a project saved before the change
  renders through the identical arithmetic. There is no migration step, no version check and nothing
  for the user to do — which is what Requirement 4.3 asks for, and what
  `ALegacyColorGradeIsLoadedWithoutMigratingItsParameters` pins from the other side: loading such a
  project must add *nothing*. An open that helpfully materialised `liftR/G/B` would be the forbidden
  migration, and would turn a later edit of the legacy scalar into a silent no-op because the
  per-channel names shadow it.
- **A unity gamma SKIPS the step rather than computing `pow(x, 1)`.** This is the genuinely subtle
  part. Gamma is meaningless on a negative value and `pow` is undefined there, so the step clamps its
  input up to 0 first — and at an exponent of 1 that clamp is still observable, because a saturation
  below 1 mixes toward a luma computed from the *unclamped* values. Without the guard, an existing
  project with a negative lift would shift. Both lanes guard it, the parity model reproduces the guard
  rather than simplifying it away, and `AUnityGammaIsSkippedRatherThanComputed` asserts that spelling
  gamma out as 1.0 is indistinguishable from omitting it.
- **The byte-identity test carries a FROZEN copy of the previous implementation.** The old arithmetic
  is gone from the tree, so a test comparing the new code to the old one has to contain the old one,
  down to its own copy of `Compositor`'s private `toByte` rounding. Comparing the new code to itself
  would assert nothing, and the temptation to "simplify" it into a call to `applyEffectSoftware` is
  called out in a comment for exactly that reason. The comparison is `==` over an image covering every
  byte value, across ten parameter combinations including a negative lift and a lift past white — not a
  tolerance, because the requirement says byte-identical.
- **Gamma runs in normalised [0,1] space in both lanes, but they reach it differently.** The kernel
  already works in [0,1]; the byte-space software reference has to divide by 255, exponentiate and
  multiply back, because `pow(200, 1/2.2)` is 10.6 rather than a brightened 200. That asymmetry is the
  likeliest place for the two to drift, so the P5 property generates gamma across [0.1, 4.0] and
  per-channel lift across [-0.5, 0.5] — the latter routinely driving values negative and so exercising
  the clamp — instead of testing a couple of example values.
- **`pow(x, 1/gamma)`, not `pow(x, gamma)`.** Above 1 brightens midtones, the direction every colour
  tool's midtone control moves. Asserted on mid-gray, because 0 and 255 are fixed points of `pow` and
  would show nothing whichever way the convention ran; the endpoints are then asserted separately to be
  left alone.
- **The defaults live in `core`, in one place.** They started at the renderer's call site and moved,
  because the Inspector must display "the current value of each" and those values include the legacy
  fallback. Resolved independently in two places, an old project would show a per-channel lift of 0 in
  the panel while the renderer used −0.25 — the panel quietly lying about what is on screen. A pre-push
  check now fails if `Compositor` resolves them itself again.
- **The Inspector presents all ten unconditionally.** Its generic loop can only show parameters
  *present* in the map, so an effect carrying just `lift` offered one spin box and no way to reach the
  other nine. The legacy scalar deliberately gets no control of its own, since it is already shown
  through the three per-channel lift rows and two widgets disagreeing about one number is worse than
  one; anything outside the known ten still falls through to the generic loop, so nothing is hidden.
- **Both "every parameter" tests iterate `kColorGradeParameterNames`.** A hand-written list in a test
  is a list that falls behind the renderer silently. Driving the round-trip and the undo tests from the
  same array the resolver uses means a parameter added without a way to set it, or without persistence,
  cannot pass by being forgotten in a second place.
- **A degenerate gamma is clamped, not divided by.** A gamma of 0 would divide by zero and a negative
  one is meaningless; either would put a NaN into a frame, where `toByte`'s two comparisons are both
  false and the cast is undefined, and it would propagate through every later effect and into the
  encoder. Detected in the test by running twice and comparing, since a NaN is not equal to itself.

Not established: nothing here has been *seen*. The GPU lane is a float model of the shader compared
against the software reference, which is what makes property P5 checkable on a runner with no Vulkan
device; no image produced by an actual SPIR-V dispatch has been compared to anything.


- [x] 5. Tone curves (Requirement 5) — **L**
  - [x] 5.1 Add a tone-curve effect type through all seven sites of audit finding 4, with master plus
        independent R/G/B channels.
  - [x] 5.2 Implement a documented, deterministic interpolation between control points, identical on the
        GPU and software paths; empty and single-point curves are the identity, not an error.
  - [x] 5.3 Clamp output without wrapping, so an aggressive curve cannot produce an overflow hue shift.
  - [x] 5.4 Expose adding, moving and removing a control point on the Tool_Surface, each one undoable;
        round-trip the points' coordinates and order through save/open. (Control points are pairs, so
        Class 2 array-item-shape conformance needs its own check here.)
  - [x] 5.5 Add a directly editable curve control to the Inspector showing the current transfer function.
  - [x] 5.6 Tests: identity cases; a known curve's transfer function on both paths; GPU/CPU parity;
        determinism across repeated evaluation; clamping at both ends; point add/move/remove undo.

**Complete.** Six commits, all green:

| Commit | What landed | Suite |
|---|---|---|
| `7e1ff5b` (run `33512850550`) | `core::ToneCurve` — points, interpolation, baking | **failed to compile** |
| `315928c` (run `33513623198`) | the one-line macro fix | **1542/1542** (+18) |
| `2cc268e` (run `33514956539`) | `EffectType::ToneCurve` through every site, kernel, reference | **1546/1547** — 1 failed |
| `2666480` (run `33516171510`) | the derived registration count | **1547/1547** (+5) |
| `f40327e` (run `33541826614`) | `timeline.edit_curve_point` and its command | **1553/1555** — 2 failed |
| `6be0c13` (run `33542641022`) | the doc fix, and the Qt-free curve editor logic | **1572/1572** (+17) |
| `8ba290f` (run `33543504086`) | the Inspector's curve control | **1577/1577** (+5) |

The decisions worth keeping:

- **THE CURVE IS BAKED TO A 256-ENTRY TABLE, and both render paths do nothing but index it.** This is
  the decision the whole task turns on. The pipeline is RGBA8, so a curve's input has exactly 256
  possible values per channel; a 256-entry table is therefore not an approximation of the transfer
  function but *is* the transfer function, completely. Interpolation happens exactly once, on the host,
  in double precision, so the GPU kernel and the software reference **agree exactly rather than within
  property P5's 1-LSB tolerance** — there is no arithmetic left in either path to diverge. Criterion 5.3
  becomes a structural fact instead of a bounded error, and criterion 5.6's clamping becomes a property
  of the table (every entry came from a clamped evaluation) rather than of a render loop.

  It also avoids a limit that would otherwise have been imposed by a transport rather than by the design:
  push constants are only guaranteed to be 128 bytes, so four channels of variable-length control points
  could not be passed that way, and "up to 16 points shared across all channels" would have been the
  result.

- **Interpolation is piecewise linear, and that is a choice, not an oversight.** Monotone cubic
  (Fritsch–Carlson) is what a dedicated colour tool uses and would look smoother under a coarse set of
  points. Criterion 5.4 asks for interpolation that is *documented and deterministic* and says nothing
  about smoothness; linear is exactly reproducible on any host with no dependence on evaluation order or
  fused multiply-add, and simple enough that a test can state a whole expected transfer function in
  closed form. Worth revisiting if a colourist finds the result too angular — the baking design means the
  interpolation could be replaced in one function without touching either render path.

- **Held flat outside the end points, not extrapolated.** Continuing the user's last segment off the end
  of the range is how an aggressive curve blows a highlight nobody asked for. Asserted directly,
  including that the extrapolated values do *not* appear.

- **Fewer than two points is the identity (5.5).** Zero is obvious; one is the interesting case, since it
  defines no segment. Clamping the range to that point's y would flatten the image to a single value —
  emphatically not what a user placing their first point expects — and an error would make the control
  unusable mid-construction.

- **Points live in the existing parameter map under indexed names** (`curveMasterP0X`, `curveRedP2Y`, …),
  so tone curves need **no schema bump and no serializer change**: `ProjectStore` already persists that
  map by iterating it, and both coordinates *and their order* round-trip because the order is carried in
  the name. Two consequences are tested rather than assumed: a half-written point (an X with no Y, which
  is what a partially applied edit leaves behind) is not read as a point at (x, 0); and a gap
  **truncates** rather than being closed up, because a point's index is its identity and promoting p2
  into a missing p1 would make an undo entry naming "point 2" refer to a different point than before.

- **Channel composition is per-channel then master, and the two compose into one table.**
  `combined[c][v] = master[channel_c[v]]`, which is why the kernel needs three tables and no notion of a
  master curve at all. The order is asserted, *and* the test asserts the two orders genuinely differ, so
  it is discriminating rather than vacuous.

- **The shader uses `round()` rather than a truncating cast** to recover the input byte. Truncation would
  read the neighbouring table entry for most values and shift the whole image by one — a subtle,
  plausible-looking wrong answer rather than an obvious failure.

- **A test asserts the kernel source contains the table binding and NO `mix()` call**, so a future change
  that moved curve arithmetic back into the shader fails loudly instead of quietly invalidating the
  exactness argument that criterion 5.3 now rests on.

Three incidents, all caught and all instructive about tooling rather than about the feature:

- **A braced initialiser passed bare to `EXPECT_EQ`** (`EXPECT_EQ(read[0], CurvePoint{0.0, 0.0})`) failed
  to compile: the comma inside the braces is split by the *preprocessor* into a third macro argument,
  because parentheses protect a macro argument and braces do not. The same file passes braced
  initialisers with commas to gtest macros elsewhere and those are fine, because they sit inside a
  function call's own parentheses. The sweep gained a check for a comma at brace depth > 0 while paren
  depth is back at 1, validated against the real failing line *and* against the legitimate ones.

- **The new sweep then reported my own tooling twice.** It ran on past a legitimate string literal
  containing an unmatched parenthesis — `EXPECT_EQ(src.find("mix("), …)` — because it did not blank out
  string literals first; the preprocessor tokenises strings before matching macro parens, so that
  parenthesis is genuinely not one. And the glued-line patterns produced 17 false hits from
  `ProjectStore.cpp`'s legitimate one-line `if (c == ',') { ++pos_; continue; }` bodies until they were
  restricted to lines absent from `HEAD` — the same delta discipline the brace-balance check already
  used, and the third time this session that discipline has been the thing that made a checker useful.

- **A hardcoded `EXPECT_EQ(registered, 6u)`** went stale the instant the enumerator landed — standing
  doctrine item 7, missed because the sweep looked for `allEffectKernels().size()` and not for a bare
  literal. Now computed from `allEffectKernels()` by excluding `Transition` (the only kernel with no
  `EffectType`, since it blends two inputs), so it cannot go stale at the next effect.

**The tool surface (5.7) is ONE tool with an add/move/remove `operation`, not three tools**, and one
`core::EditCurvePointCommand` behind it. `timeline.set_effect_parameter` could not serve: a control point
is a pair, so adding one sets two parameters and would be two history entries — a single Undo would leave
an X with no Y behind, which `curvePoints` deliberately refuses to read as a point, so the user would
watch their point vanish and the curve change shape in a way no single action explains. One tool rather
than three because the three operations share their target and differ only in which coordinates matter,
and because this repository's conformance obligations (registry entry, schema, `TOOLS.md` row,
`drawValidInvocation` case, tool count, the exhaustive schema map) multiply by three otherwise while the
criterion is satisfied either way.

`revert()` restores **the whole channel's prior parameter set** rather than undoing the specific
mutation, which is the design decision worth arguing about. It is not laziness: removing a middle point
renumbers every point after it, so the inverse of "remove p1" is not "add a point at p1" — after the
removal there is no p1 carrying the old p2's coordinates to put back. Capturing the channel wholesale
makes the inverse exact for all three operations with no special case, and the captured set is at most a
few dozen doubles. The capture is selected by **name prefix**, not by walking indices, because a walk
stops at the first gap and would silently fail to restore anything past it.

**Move and remove require an index and refuse without one.** Defaulting a missing index to 0 would
silently move or delete the wrong point, and a plausible wrong answer is worse than a rejection. Add
ignores the index and appends, because rendering sorts by x regardless, so an insertion position would be
a distinction without a difference — while a point's index *is* its identity for a later move or remove.
That makes which arguments are required depend on another argument's *value*, which the `ArgSpec`
vocabulary cannot express; rather than weaken the tool to fit the schema, the gap is declared as **Class
1** in `tool_schema_conformance_property_test.cpp`, which exists for exactly this.

**The Inspector control (5.9) is split in two, and the Qt-free half carries all the behaviour.**
`ui::CurveEditorViewModel` owns the pixel-to-coordinate mapping, the point hit test and the gesture;
`ui::CurveEditorWidget` only paints and forwards mouse events. Nothing compiles `InspectorPanel.cpp`
outside the shell build, so a panel-resident implementation would have had no behavioural coverage at all;
this way 19 cases run on a host with no display, no Qt and no GPU.

`release()` is the **single commit point**. The first version committed an Add on press and a Move on
release, so clicking on empty space and dragging to place a point — one action as far as the user is
concerned — cost two undo entries and took two presses to undo. Now `press()` and `drag()` only advance
the working copy so the gesture can be drawn, and `release()` returns an Add carrying the *final*
position, a Move when an existing point actually moved, or nothing when a point was grabbed and released
without moving. A motionless click still adds, because requiring movement would make single clicks do
nothing while drags worked.

Three further hazards are pinned by test rather than reasoned about. A zero-sized widget mid-layout must
not divide, since NaN in a control point does not glitch visibly — it silently poisons the baked table.
The **nearest** point wins the hit test, or the earlier-added of two overlapping points would capture the
pointer forever and dragging would quietly move the wrong one. And an external change (an undo, or an
edit over MCP) **abandons a gesture in progress**, because the index being dragged may name a different
point afterwards and continuing would move whichever point now holds it — a wrong edit that looks like a
successful one.

The widget plots `bakeToneCurve`'s own table, so the curve drawn is necessarily the curve applied. The
panel hides the raw control points from its generic parameter loop: thirty-two `curveMasterP3Y` spin
boxes beside the plot would be unreadable and would let the two widgets disagree about the same point.

Two more incidents, both worth keeping:

- **A `Result:` line in `docs/TOOLS.md` wrapped**, leaving `*(command result)*` alone on the following
  line, and that marker is read from the `Result:` line itself — so two documentation-consistency tests
  reported the tool returning an undocumented `status`. The four backticked field names on the first line
  *were* harvested, which is why only one field was reported and the diagnosis was narrow. Checked rather
  than assumed afterwards: 23 `Result:` lines carry the marker, all on the line itself; two others do
  wrap and are green, because they continue with backticked field names, which the observer harvests from
  the whole block. The rule is about the marker's placement, not about wrapping.

- **The newline-eating `strReplace` happened again** — an edit whose only purpose was adding an include
  glued `EditCommands.hpp` to `Effect.hpp` on one line, exactly CI incident 2's damage class. Caught
  pre-push. That is four times this session from the same cause, all from edits touching only whitespace
  or an include, which is why doctrine item 15 exists.

Also moved the channel and operation **name** vocabularies out of `ToolRegistry`'s anonymous namespace
into `core`, beside the types they name, because the Inspector needs them too — the same reasoning that
moved the colour-grade defaults into `core` in task 4. Each parser is derived from its own name function
by iteration, so a name added to one direction cannot be missing from the other.


- [x] 6. Video scopes (Requirement 6) — **M**
  - [x] 6.1 Add pure, Qt-free, GPU-free histogram, luma-waveform and vectorscope computations over an
        RGBA8 frame buffer.
  - [x] 6.2 Feed them from the output of the same `gpu::Compositor` instance the Preview displays, so a
        scope cannot disagree with the picture beside it, and so all effects and burn-in are reflected.
  - [x] 6.3 Add the three scopes to the Editor_Shell, individually hideable with visibility persisted
        across restart, updating at least 10×/second without costing the Preview more than 10% of its
        presented frame rate.
  - [x] 6.4 Present an explicit empty state when no frame is available, rather than a stale reading.
  - [x] 6.5 Tests: fully black, fully white and full-saturation-primary frames each produce the
        documented asserted reading on all three scopes; the empty state is reached with no project; the
        Preview's frame-rate budget is respected.

**Complete.** Four commits; one CI incident.

| Commit | What landed | Suite |
|---|---|---|
| `ea8c8ef` (run `33544033387`) | the three computations | **1596/1596** (+19) |
| `adc8903` (run `33545162274`) | the Qt-free cadence, empty state and visibility | **1612/1612** (+16) |
| `8a152ec` (run `33546003036`) | the Scopes dock and the frame observer | **1616/1617** - 1 failed |
| `76e29a0` (run `33546814172`) | the meter assertion fix | **1617/1617** |

**Criterion 2 is satisfied by WHERE the frames come from, not by anything the panel does.**
`ui::PreviewView` gained a `FrameObserver` seam called from `uploadFrame` with the very buffer being
uploaded, and the shell installs the panel's `observeFrame()` as that observer. So a scope is computed
from the same `gpu::Compositor` output, at the same moment, as the picture beside it - which is exactly
what "a scope can never disagree with the picture beside it" demands, and it makes criterion 3 (effects
and burn-in reflected) automatic rather than a separate feature. A second `PreviewFrameSink` could not
have promised as much: `setFrameSink` holds only one sink, so installing another would displace the
view's own and the scopes could end up measuring a frame nobody saw.

**The cadence is where the difficulty is.** Criterion 5 has two halves pulling in opposite directions -
at least 10 updates a second, and never more than ten percent of the Preview's presented frame rate - so
`shouldRecompute()` is a pure function of the last computation's instant, the current instant and the
*measured* cost of the last one. Three decisions, each deliberate:

- **The floor wins over the budget** when they conflict. A panel that stops updating is a broken panel;
  a Preview one frame short is imperceptible, and the floor is only 10 Hz.
- **The first computation is always allowed**, even on an impossible budget, or a host where one
  computation happens to exceed the share would show a permanently empty panel with no explanation.
- **A hidden scope is not computed at all.** The cheapest way to honour the budget is not to spend it.

With no Preview frame rate - paused, which is when a colourist actually reads a scope - any cost fits,
because there is nothing to protect. The ten percent is asserted on *both* sides of the boundary (1666us
fits a 60 fps frame, 1667us does not), because a budget that was really 100 percent or 1 percent would
pass a one-sided test.

**A bug in my own first draft, worth recording because the symptom would have been misleading.**
`observeFrame` called `update()` twice - once to do the work, once to record what it had cost - which
computed every scope TWICE per frame and so spent double the budget the cost exists to protect. The
symptom would have been a Preview that felt slightly heavy while the measurement machinery reported it
was well inside budget. `recordCost()` replaced the second call, and a test asserts `computeCount()`
stays at 1 across it.

**Empty is a value, not an error, and the two sides are both asserted.** A null or zero-sized buffer
produces an empty result rather than failing, and `clear()` drops every reading rather than keeping the
last: a scope showing the previous shot's exposure is worse than one showing nothing, because it looks
authoritative. Equally, a **black frame must not read as empty** - it has every sample in bin 0, which
is a real reading. `clear()` deliberately keeps its cadence bookkeeping, or the view model would report
"nothing computed yet" and spin while no frame exists, spending the budget precisely when there is
nothing to show.

Criterion 8's readings are written as **literals with the arithmetic beside them**, not computed by
calling the same helper under test - which would assert only that a function equals itself and would
pass with every axis inverted. White's luma is exactly 255; red's is 76 because 0.299 x 255 = 76.245;
red's Cr is 255 because it is 255.45 *before* clamping, which is why a full-saturation primary sits ON
the graticule edge exactly as on a real vectorscope. Two further cases pin mistakes a uniform frame
cannot show: a left-dark, right-bright frame must trace dark on the LEFT (an inverted or mirrored column
mapping is the most plausible wrong answer), and a red frame's waveform must read 76 rather than 255
(255 would mean it is plotting the red channel instead of luma).

A waveform's columns are **bucketed and summed, never sampled**. A 3840-wide frame cannot be shown in a
200-wide panel, and choosing which pixels to keep is exactly how a waveform loses the overexposed streak
being looked for; a test walks a 1920x3 frame and asserts all 5760 pixels land in exactly one bucket.

The Scopes dock is **tabbed with Inspector and Agent Chat**, because a colourist reads a scope *instead*
of the Inspector rather than beside it, and a fifth visible column would cost the Preview width at the
1280x720 minimum the layout property test exercises. The three visibility toggles are checkboxes on the
panel rather than menu actions: a new menu would shift the menu bar's indices, which several shell tests
address positionally, and a scope's visibility belongs beside the scope.

### CI incident 5

`8a152ec` failed one test: `ShellUnitTest.TheProgrammeLevelMeterIsMountedInTheTransportBar` carried a
*second* hardcoded dock count (`EXPECT_EQ(findChildren<QDockWidget*>().size(), 4u)`) that the new dock
invalidated. I had checked for exactly this before pushing, and the checker reported one assertion,
correctly updated - because it used `re.search`, which returns only the **first** match, on a pattern
that can occur many times.

**Standing lesson: a pattern that can appear repeatedly must be scanned with `findall`.** Searching for
one instance and reporting "found and correct" is how a check passes while the defect it was written for
sits ten lines further down. This is the same class as the earlier lesson about implausibly *many* hits,
in the opposite direction: an implausibly *small* result is also the checker reporting itself.

The fix did not renumber the count. That test's claim is "the meter is in the transport bar, NOT in a
dock of its own", which a dock count states only incidentally - it went stale the moment an unrelated
dock appeared and said nothing about where the meter is. It now asserts that no dock's widget *is* the
meter, which is the actual claim and cannot go stale when a sixth dock is added for some other reason.

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
