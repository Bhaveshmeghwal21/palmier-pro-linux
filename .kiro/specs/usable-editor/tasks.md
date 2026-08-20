# Implementation Plan

Derived from `requirements.md` in this directory. Phases are ordered by blocking severity, with the
one deliberate exception argued in the requirements' sequencing note: Phase 2 (the generative
transport) is placed ahead of the larger editing-quality and content phases because its
value-to-effort ratio is the highest in the document and nothing else blocks it.

Effort labels are relative, not calendar estimates: **XS** = wiring existing tested code, **S** = one
new well-bounded class or a few gestures, **M** = a new subsystem with tests, **L** = a new subsystem
plus renderer or platform work.

---

## Phase 1 — Make GUI editing possible at all

Everything in this phase wires up code that already exists and already passes tests. No new domain
logic. Until this lands, no other phase is observable by a user.

- [x] 1. Wire timeline selection to the Inspector (Requirement 1) — **XS**
  - [x] 1.1 Connect `TimelinePanel`'s `QTreeView` selection to `InspectorViewModel::selectClip()`,
        clearing the Selection when a track row (rather than a clip row) is chosen. This single
        connection is what unblocks Delete, Split, trim, opacity, gain and effects — all of which are
        already implemented, gateway-routed and unit-tested.
  - [x] 1.2 Clear the Selection when the selected clip disappears from the project by any surface, so
        the Inspector never points at an absent clip.
  - [x] 1.3 Replace the after-the-fact "No clip is selected" status message on `Edit > Delete Clip`
        and `Edit > Split at Playhead` with proper enable/disable state driven by the Selection.
  - [x] 1.4 Tests: selecting a clip row populates the Inspector; selecting a track row clears it;
        removing the selected clip clears it; both Edit actions are disabled with no Selection and
        enabled with one.

- [x] 2. Add track creation to the GUI (Requirement 2) — **XS**
  - [x] 2.1 Add an `addTrack` gesture to `TimelineViewModel` routed through `GuiToolGateway::addTrack`
        (the gateway method already exists; the view model has only `canAddTrack()`).
  - [x] 2.2 Add "Add Video Track" and "Add Audio Track" actions to the menu bar, disabled when
        `canAddTrack()` is false.
  - [x] 2.3 Tests: each action appends after the last track of its kind; both are disabled at the
        track ceiling; each is undoable in exactly one Undo.

- [x] 3. Add clip placement to the GUI (Requirement 3) — **S**
  - [x] 3.1 Add a "Place at Playhead" gesture that takes the Media_Browser's selected asset and the
        Timeline_Panel's selected track and calls the existing `GuiToolGateway::addClip`.
  - [x] 3.2 Disable it when no asset is selected or no track exists; surface an overlap refusal as a
        status indication that names the overlap, leaving the project unchanged.
  - [ ] 3.3 Implement drag-and-drop from the Media_Browser list onto a Timeline_Panel track row as an
        equivalent route (the UI layer currently has no drag-and-drop code of any kind). **NOT DONE**
        — the menu-driven route (3.1/3.2) is complete and tested; drag-and-drop is deferred (see
        Progress note below for why, and Phase 3's graphical-timeline task is the natural place to
        add it alongside drag-move/drag-trim on the same rewritten widget).
  - [x] 3.4 Tests: placement produces project state equal to invoking `timeline.add_clip` through the
        MCP endpoint with the same arguments (both routes call the identical `GuiToolGateway::addClip`
        → `timeline.add_clip` path, so this is true by construction and is exercised directly); overlap
        is refused with no change (covered by the existing `AddClipCommand` invariant test at the
        command level; not re-asserted at the shell level in this pass); each placement is one Undo.

- [x] 4. Add playhead positioning (Requirement 4) — **S**
  - [x] 4.1 Add a scrubbable playhead control and an editable timecode field to the Timeline_Panel,
        both driving the existing `PreviewView::seekSeconds`. (Implemented via `PreviewController::seek`
        directly, which `seekSeconds` itself wraps — same effect, one fewer indirection.)
  - [x] 4.2 Add frame-step-back and frame-step-forward actions using the project frame rate's frame
        interval.
  - [x] 4.3 Snap every requested position to the nearest frame boundary; clamp out-of-range requests to
        the nearer bound.
  - [x] 4.4 Tests: the displayed timecode agrees with the playhead to the frame; stepping moves exactly
        one frame interval; negative and beyond-duration requests clamp rather than error.

- [x] 5. **Phase 1 checkpoint** — the usability threshold's first half
  - [ ] 5.1 An end-to-end GUI test that, driving only widgets: creates a track, imports a fixture
        asset, places it, places a second clip, selects one, splits it at a set playhead, deletes a
        piece, then exports — asserting the exported file probes and decodes with the expected duration.
        **NOT DONE AS SPECIFIED** — see Progress note: the tests actually added
        (`ShellSelectionTest`, `ShellPlacementTest`, `ShellPlayheadTest`, the two `AddTrackAction`
        tests) cover the same wiring end to end but register the placed asset directly on
        `MediaManager` rather than through a real FFmpeg-probed `media.import` call, and do not
        chain all the way through an actual export. A real-fixture version of this exact test is the
        first item of unstarted work below.
  - [x] 5.2 Extend the `ci.yml` launch smoke test to drive this sequence with `xdotool` and capture a
        screenshot of the populated timeline, so the artifact shows a real edit rather than an empty
        project. (Drives Edit > Add Video Track specifically, not the full create→place→split→delete
        chain 5.1 describes; see Progress note.)
  - [x] 5.3 Update `docs/UPSTREAM_PARITY.md`'s `timeline editing` row, which is the report's only
        `must`-priority gap. (Also re-scored six further rows that were stale for the same reason.)

---

## Phase 2 — Make the generative capability live

One class behind an already-defined seam with six existing test doubles. OpenSSL is already a linked
dependency and the repository already contains TLS code for the MCP server to model this on.

- [ ] 6. Implement an HTTPS generative transport (Requirement 11) — **S/M**
  - [ ] 6.1 Implement `GenerativeHttpTransport` over OpenSSL: connect, verify the certificate by
        default, POST the request the backend built with its headers intact, read status and body.
  - [ ] 6.2 Apply a request timeout and map timeout, connection failure and TLS verification failure
        onto the distinct error codes the existing backends already branch on.
  - [ ] 6.3 Preserve the plaintext refusal: an `http://` endpoint is refused without sending bytes.
  - [ ] 6.4 Guarantee no credential value is ever logged, matching the discipline
        `RemoteAccessGate` already holds itself to.
  - [ ] 6.5 Install it in `ApplicationComposition` by default, falling back to the unavailable
        transport only where the build excludes TLS.
  - [ ] 6.6 Stand up a local HTTPS test endpoint and verify submit/poll/fetch end to end against it, so
        verification does not depend on the closed hosted service.
  - [ ] 6.7 Remove Property 66's remaining reliance on an injected transport where it now has a real
        one, and assert that a BYOK generation completes and lands as exactly one undoable edit.
  - [ ] 6.8 Re-score the `generate` row in `docs/UPSTREAM_PARITY.md`, which currently reads `absent`
        solely because of this missing class.

- [ ] 7. Land the generative backlog entries (Requirement 12) — **M**
  - [ ] 7.1 Upscale, audio generation and catalogue-driven model selection, each against the acceptance
        check its `docs/PORT_BACKLOG.md` entry already declares.
  - [ ] 7.2 Move each landed entry to `complete` in the same change, keeping the backlog-consistency
        test green.

---

## Phase 3 — Make editing tolerable rather than merely possible

- [ ] 8. Ripple editing and gap management (Requirement 5) — **M**
  - [ ] 8.1 `RippleDeleteCommand` and `RippleTrimCommand` in the domain core, preserving the ordered,
        non-overlapping track invariant the engine already enforces.
  - [ ] 8.2 Publish both on the Tool_Surface, and a close-gap operation.
  - [ ] 8.3 Expose all three in the Editor_Shell, enabled only with a Selection.
  - [ ] 8.4 Tests: each is one Undo; the invariant holds after every operation; the deferred PR 397
        backlog entry's check passes.

- [ ] 9. Effect lifecycle management (Requirement 6) — **M**
  - [ ] 9.1 Remove, reorder and re-parameterise operations in the domain core and on the Tool_Surface.
  - [ ] 9.2 Wire them into the Inspector's effect list.
  - [ ] 9.3 Tests: reordering changes rendered output on both the GPU and software paths; a parameter
        change re-renders the preview; naming an absent effect is refused with no change.

- [ ] 10. Mutable project settings (Requirement 7) — **S/M**
  - [ ] 10.1 A settings-change operation on the Tool_Surface accepting `project.create`'s ranges.
  - [ ] 10.2 A settings surface in the Editor_Shell that reads current values and submits changes.
  - [ ] 10.3 Tests: clip positions and source ranges survive a frame-rate change as durations;
        out-of-range values are refused by name; every change is one Undo.

- [ ] 11. A graphical timeline (Requirement 8) — **L**
  - [ ] 11.1 Replace the `QTreeView` with a custom widget: lanes per track, clip rectangles positioned
        and sized by timeline start and duration, a time ruler and a playhead marker.
  - [ ] 11.2 Click-to-seek on the ruler and empty lane areas, honouring Phase 1's frame snapping.
  - [ ] 11.3 Zoom that keeps the playhead visible across a zoom change.
  - [ ] 11.4 Drag a clip to move it and drag its edges to trim, both through the Tool_Surface, with a
        refused drop reverting visually.
  - [ ] 11.5 Render the Selection consistently with Requirement 1.
  - [ ] 11.6 Tests: rectangle geometry corresponds to clip timing across zoom levels; a refused drag
        leaves the project unchanged; drag-move and `timeline.move_clip` produce equal state.

---

## Phase 4 — Content most deliverables need

- [ ] 12. Text and titles (Requirement 9) — **L**
  - [ ] 12.1 A text clip type in the domain core with string, font, size, colour, alignment and
        position, round-tripping through save and open.
  - [ ] 12.2 Tool_Surface operations to create a text clip and change its string and styling.
  - [ ] 12.3 Text rasterisation on both the GPU and software compositor paths, agreeing within the
        tolerance the existing GPU/CPU parity property applies to effects.
  - [ ] 12.4 An entry and styling surface in the Editor_Shell previewing at the current playhead.
  - [ ] 12.5 A documented default-font substitution that reports rather than fails when a requested
        family is absent.
  - [ ] 12.6 Tests: text round-trips; both render paths agree; an export shows the text identically to
        the preview at the corresponding frames.

- [ ] 13. Captions and transcription (Requirement 10) — **L**
  - [ ] 13.1 A caption track kind in `core::TrackKind` and caption cues that round-trip.
  - [ ] 13.2 Add, edit, retime and remove cue operations on the Tool_Surface.
  - [ ] 13.3 Burn-in and sidecar subtitle export.
  - [ ] 13.4 Construct `services::TranscriptionService` in the Composition_Root and publish an
        audio-to-cues operation.
  - [ ] 13.5 Tests: a missing recognizer backend is reported by name and does not block authoring
        captions by hand; burned-in and sidecar outputs agree on cue timing.

- [ ] 14. Still-frame capture — **S**
  - [ ] 14.1 An operation that writes the frame at the playhead to an image file, plus an Editor_Shell
        action for it.
  - [ ] 14.2 Tests: the written image matches the preview frame within the existing parity tolerance.

- [ ] 15. Media organisation — **M**
  - [ ] 15.1 Bins or tags over the currently flat media library, round-tripping through save and open.
  - [ ] 15.2 A filter field in the Media_Browser.
  - [ ] 15.3 Tests: organisation survives save and open; filtering never changes project state.

---

## Phase 5 — Distribution and documentation

- [ ] 16. Installable artifact (Requirement 13) — **M**
  - [ ] 16.1 Package as a self-contained artifact for a documented distribution set, declaring or
        bundling every dependency `PlatformCompatibility` requires.
  - [ ] 16.2 Build it in CI from a tagged commit and verify it launches and renders by reusing the
        existing launch smoke test against the installed artifact.
  - [ ] 16.3 Ship the GPLv3 text and `NOTICE`, preserving the documented licensing split.
  - [ ] 16.4 Tests: a host missing a required dependency is told which one, by name.

- [ ] 17. Re-score the parity report (Requirement 14) — **S**
  - [ ] 17.1 Re-score `docs/UPSTREAM_PARITY.md` against the tree, removing every stale
        "MainWindow is still a placeholder" and "panel is unmounted" rationale.
  - [ ] 17.2 Record, per changed row, why the status changed.
  - [ ] 17.3 Keep the documentation-consistency tests green.

---

## Progress

**Phase 1 landed and is CI-verified green on `main` as of commit `a7fd88f` (build `32369055159`,
completed success, 3m10s): 1257 of 1257 tests passing (up from 1239 before this phase), the
`configure-without-vendor-sdks` job green, and the launch smoke test's real runtime output reading
`OK: palmier-pro launched, mapped its main window and painted 5031 distinct colours` followed by
`OK: drove Edit > Add Video Track via the keyboard` — a real keyboard-delivered menu action against
the shipped binary, not a unit test.**

The audit in `requirements.md` was verified against commit `1bc20d1`, at which point CI was green
(run `32362473786`) with 1239 of 1239 tests passing and the launch smoke test capturing a
rendered-but-empty-and-uneditable editor.

**The single most valuable finding, restated because it determined the whole plan:** GUI editing was
blocked by four unwired connections, not by missing features. `InspectorViewModel::selectClip()`,
`GuiToolGateway::addTrack()` and `GuiToolGateway::addClip()` were all implemented and tested but had
no callers, and no drag-and-drop or context-menu code existed anywhere in `src/ui/`. Task 1.1 alone —
a single signal connection — unblocked clip deletion, splitting, trimming, opacity, gain and effects,
all of which were already built.

### What was actually built (commits `65fb3d9`, `6233e9c`, `a7fd88f`; doc commit `85e04bf`)

- **Selection (Task 1).** `TimelinePanel` gained `clipSelected(QString)`/`selectionCleared()` signals
  driven by the tree's own `QItemSelectionModel`, plus `selectedClipId()`/`selectedTrackId()` readers.
  `MainWindow` wires these to `InspectorViewModel::selectClip`/`clearSelection` and to new
  `deleteClipAction_`/`splitAction_` enable/disable state, replacing the old
  `statusBar()->showMessage("No clip is selected", ...)` pattern entirely. A model reset (any engine
  change, from any surface) re-derives the selection, so a clip deleted via the MCP endpoint or the
  agent correctly clears a GUI selection pointed at it.
- **Track creation (Task 2).** `TimelineViewModel::addTrack(TrackKind)` is new: it enforces the
  50-track UI ceiling itself (distinct from `AddTrackCommand`'s own, much higher, 64-per-kind cap —
  confirmed by reading `EditCommands.cpp` directly rather than assuming the existing command would
  reject at 50), then routes through the gateway exactly like every other gesture.
  `TimelineModel::addTrack(QString)` and two new Edit-menu actions ("Add Video Track" / "Add Audio
  Track") expose it, disabled together whenever `canAddTrack()` is false.
- **Placement (Task 3).** `MediaBrowserViewModel` gained a library-asset-selection concept
  (`selectLibraryAsset`/`selectedLibraryAsset`, deliberately distinct from the pre-existing clip
  VERSION selection, which is a different thing entirely) and an `assetDuration()` cache populated
  from `media.import`'s own `durationMs` result field inside `importMediaViaGateway`.
  `MediaBrowserPanel` wires its library `QListWidget`'s `currentRowChanged` to this and emits a new
  `librarySelectionChanged()` signal. A new "Place at Playhead" Edit-menu action, enabled only when
  both a library asset and a timeline track are selected, calls the pre-existing
  `GuiToolGateway::addClip` with the selected asset, the selected track, the cached (or a 1-second
  placeholder, if uncached) duration, and the current playhead as `timelineStart`.
- **Playhead (Task 4).** `TimelinePanel` gained a `QSlider` scrub bar (range = timeline duration in
  ms), two frame-step `QToolButton`s, and an editable `QLineEdit` timecode field (HH:MM:SS.mmm,
  lenient on fewer segments). All four funnel through one `movePlayheadToMs()` that snaps to the
  nearest frame at the PROJECT's edit frame rate (`TimelineModel::timelineFps()`, i.e.
  `Project.timelineFps` — deliberately NOT `PreviewController::previewFrameRate()`, which is clamped
  to >= 24 fps for display smoothness and would snap a lower-fps project to the wrong grid) and
  clamps to `[0, timelineDurationMs()]`.

### Deliberate, disclosed scope reductions from the original task list

- **Task 3.3 (drag-and-drop) was not implemented.** The menu-driven placement route (3.1/3.2) is
  complete, tested and is the ONLY route right now — there is still no drag-and-drop code anywhere in
  `src/ui/`. Rationale for deferring rather than adding it here: `TimelinePanel`'s tree view has no
  natural "drop onto this lane" target shape (a `QTreeView` row is not a time-axis position), so a
  drag-and-drop implementation done properly belongs with Phase 3's graphical-timeline rewrite
  (`design.md`/`tasks.md` task 11), where drag-move and drag-trim need the identical drop-target
  machinery. Building it twice — once against the tree, once against the eventual graphical view —
  was judged not worth doing. This is an honest gap, not a forgotten one.
- **Task 5.1's end-to-end test does not use a real FFmpeg-probed fixture or a real export.** The
  tests actually added (`ShellSelectionTest`, `ShellPlacementTest`, `ShellPlayheadTest`, the two
  `AddTrackAction` tests in `tests/ui/shell_unit_test.cpp`) drive the real `MainWindow` /
  `TimelinePanel` / `MediaBrowserPanel` widgets over a real `ApplicationComposition` and prove the
  full selection→placement→split→delete wiring works, but they register the placed asset directly
  via `MediaManager::importAsset` rather than through a real `media.import` call, and none of them
  chain into `ExportCoordinator`. Reason: a real fixture needs `tests/support/SyntheticMedia.hpp`'s
  FFmpeg encoder negotiation (confirmed by reading that header in full), which is a heavier,
  CMake-target-specific piece of machinery (`tests/e2e/editor_end_to_end_test.cpp`'s
  `PALMIER_E2E_FIXTURE_DIR` generator) that was judged out of scope for a Phase 1 wiring pass. A
  fixture-backed version of exactly this test is the natural next step and is recorded as the first
  item of unstarted work.
- **The CI smoke-test extension (5.2) drives only Add Video Track**, not the full
  create-track→place→split→delete chain 5.1 originally described, for the same reason: no real
  importable fixture is generated for the smoke test to place. It is evidentiary rather than a hard
  gate (wrapped in `set +e`; a keyboard-delivery quirk under a bare Xvfb — confirmed and fixed once
  already, see the CI incident below — must not mask the smoke test's two pre-existing, load-bearing
  assertions).

### CI incidents hit and fixed during this phase (kept for the audit trail)

1. **Run `32368221271` (first push) failed twice over.** `tests/docs/parity_report_property_test.cpp`
   caught two rationale strings over the documented 1-200 character bound (`timeline editing` at 531
   chars, `layout` at 206) — both fixed in commit `a7fd88f`, re-verified programmatically
   (UTF-8-codepoint-counted, matching the parser's own `characterCount()`) before recommitting.
   Separately, the CI smoke-test extension's `xdotool windowactivate "$window"` failed with
   "windowmanager claims not to support _NET_ACTIVE_WINDOW" (there is no window manager under this
   bare Xvfb) and, under `set -uo pipefail`, took the whole smoke-test step down with it — fixed in
   commit `6233e9c` by dropping `windowactivate` entirely (`xdotool key --window` delivers keys via
   XTest without needing window activation) and wrapping the keyboard sequence in `set +e` so it
   cannot mask the step's two hard assertions again.
2. **Run `32369055159` (after both fixes) is fully green** — 1257/1257 tests, both non-l4 jobs
   passing, smoke test's real output confirming both the launch and the keyboard-driven edit
   succeeded (the "XGetInputFocus returned the focused window of 1" lines in that run's log are
   `xdotool`'s own benign diagnostic chatter under a window-manager-less Xvfb, not a failure — the
   step's recorded conclusion is `success` and the edit's own status was 0, so the `set +e` fallback
   was not even needed on this run).

### Unstarted work (tracked here rather than opened as new items, to keep this checklist self-contained)

- A fixture-backed version of Task 5.1's originally-described end-to-end test (real `media.import`
  probe via `tests/support/SyntheticMedia.hpp`, chained into a real export and readback).
- Drag-and-drop placement (Task 3.3), deferred to Phase 3's graphical timeline per the rationale
  above.
- Everything in Phases 2-5 of this spec, none of which has been started.
