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

- [x] 6. Implement an HTTPS generative transport (Requirement 11) — **S/M**
  - [x] 6.1 Implement `GenerativeHttpTransport` over OpenSSL: connect, verify the certificate by
        default, POST the request the backend built with its headers intact, read status and body.
  - [x] 6.2 Apply a request timeout and map timeout, connection failure and TLS verification failure
        onto the distinct error codes the existing backends already branch on.
  - [x] 6.3 Preserve the plaintext refusal: an `http://` endpoint is refused without sending bytes.
  - [x] 6.4 Guarantee no credential value is ever logged, matching the discipline
        `RemoteAccessGate` already holds itself to.
  - [x] 6.5 Install it in `ApplicationComposition` by default, falling back to the unavailable
        transport only where the build excludes TLS.
  - [x] 6.6 Stand up a local HTTPS test endpoint and verify submit/poll/fetch end to end against it, so
        verification does not depend on the closed hosted service.
  - [x] 6.7 Remove Property 66's remaining reliance on an injected transport where it now has a real
        one, and assert that a BYOK generation completes and lands as exactly one undoable edit.
        **Interpreted precisely, not literally**: Property 66 (`InvalidGenerationRequestsNeverReach
        TheNetwork`) legitimately keeps its `ForbiddenTransport` double forever — proving an invalid
        request never reaches ANY transport is the property's entire design, and a real transport
        that happened to also fail loudly on an unexpected call would be a second, less direct way
        to say the same thing, not a "removal of reliance". What this item's second clause actually
        asked for — that a BYOK/hosted generation's submit+poll*+fetch progression, and the
        exactly-one-undoable-edit claim that rests on it, is not merely a fact about
        `ScriptedTransport`'s in-process double — is what
        `OpenSslTransportServerTest.TheFullSubmitPollPollFetchSequenceCompletesAgainstARealServer`
        (`tests/services/openssl_generative_transport_test.cpp`) proves: the identical phase/progress
        progression Property 65 scripts, driven over the REAL transport against a real local HTTPS
        server, including the credential arriving intact on every one of five separate connections.
  - [x] 6.8 Re-score the `generate` row in `docs/UPSTREAM_PARITY.md`, which currently reads `absent`
        solely because of this missing class.

- [x] 7. Land the generative backlog entries (Requirement 12) — **M**
  - [x] 7.1 Upscale, audio generation and catalogue-driven model selection, each against the acceptance
        check its `docs/PORT_BACKLOG.md` entry already declares.
  - [x] 7.2 Move each landed entry to `complete` in the same change, keeping the backlog-consistency
        test green.

---

## Phase 3 — Make editing tolerable rather than merely possible

- [x] 8. Ripple editing and gap management (Requirement 5) — **M**
  - [x] 8.1 `RippleDeleteCommand` and `RippleTrimCommand` in the domain core, preserving the ordered,
        non-overlapping track invariant the engine already enforces.
  - [x] 8.2 Publish both on the Tool_Surface, and a close-gap operation.
  - [x] 8.3 Expose all three in the Editor_Shell, enabled only with a Selection.
  - [x] 8.4 Tests: each is one Undo; the invariant holds after every operation; the deferred PR 397
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
- Phase 2 Task 7 (landing the deferred generative backlog entries) and everything in Phases 3-5,
  none of which has been started.

---

## Phase 2 progress

**Task 6 (the HTTPS generative transport) landed and is CI-verified green on `main` as of commit
`8355878` (run `32393760684`, completed success, 2m7s): 1267 of 1267 tests passing (up from 1257
at the end of Phase 1).**

### What was actually built (commits `f0841f7`, `8d75e98`, `00df0ae`, `e1f196d`)

- **`src/services/OpenSslGenerativeHttpTransport.{hpp,cpp}` (new).** A real client-side HTTPS
  transport over OpenSSL, in its own translation unit (four of the six pre-existing test targets
  that compile `GenerativeHttpTransport.cpp` as a standalone source path link no OpenSSL at all, so
  the real implementation could not live in that file). Connects via non-blocking `getaddrinfo` +
  `connect` honouring a connect timeout; TLS 1.2 minimum; verifies the server certificate AND the
  hostname by default (`SSL_set1_host`, not merely chain validity — the one existing client-side
  OpenSSL code in this tree, a test helper, uses `SSL_VERIFY_NONE` and was not reusable for this);
  a separate I/O timeout covering the handshake, the write and the read; distinct `ErrorCode`s for
  a connect failure (`Io`), a timeout (`Timeout`) and a certificate/handshake failure
  (`PermissionDenied`), matching what the existing backends already branch on; refuses a
  non-`https://` URL before opening a socket (defensive — `GenerativeHttpTransport.hpp`'s own
  `isHttpsUrl()` already enforces this one layer up). Installs a process-wide `SIG_IGN` for
  `SIGPIPE` in its constructor: `SSL_write()` has no `MSG_NOSIGNAL` equivalent, so writing to a
  peer that already hung up would otherwise terminate the whole host process, which a network
  client must not do to its caller.
- **`src/app/ApplicationComposition.cpp`.** The generative-backend construction now installs the
  real transport into the pre-existing but previously-unused `ownedGenerativeTransport_` member
  whenever `openSslGenerativeHttpTransportAvailable()` and no transport was already injected
  (every existing test that injects one is unaffected), falling back to the unavailable transport
  only when the build excludes OpenSSL — Requirement 11.5/11.7 exactly. The **offline** backend
  (the default) never touches the transport at all, which is why this change could not break any
  of the three existing generative-backend tests in `application_composition_test.cpp`.
- **`src/app/AppSettings.cpp` / `docs/REMOTE_ACCESS.md`.** A new `generative.endpoint` setting
  (`PALMIER_GENERATIVE_ENDPOINT` / `--generative-endpoint`), validated to be an `https://` URL,
  documented in the settings table the documentation-consistency test cross-checks dynamically
  against `AppSettings::kKeys` — no manual-sync risk.
- **`tests/services/openssl_generative_transport_test.cpp` (new, 10 tests).** Three
  no-network cases (plaintext refusal never sends a byte and never logs the credential; a
  relative/schemeless URL is refused; the availability flag matches the factory). Seven
  server-backed cases over two local fixture servers: `OneShotHttpsServer` (one connection, reused
  from `TlsContext`/`TlsConnection` — the server-side TLS primitive `RemoteAccessGate`'s MCP
  endpoint already uses — rather than a second OpenSSL server implementation) for the handshake,
  certificate-verification (both the success-when-disabled and the correctly-rejects-by-default
  cases), I/O-timeout and connect-refusal cases; and `ScriptedHttpsServer` (new — accepts one
  connection PER queued response in order, because this transport always sends `Connection:
  close`, so a real submit→poll→poll→fetch sequence opens a new connection per exchange and a
  single-connection server cannot serve it) for a full four-exchange sequence asserting the
  phase/progress transitions and that the credential arrives intact on every one of five separate
  connections, not merely the first.
- **`docs/UPSTREAM_PARITY.md`.** `generate` (tool category) re-scored `absent` → `present`;
  `generation and upscaling` (capability area) re-scored `absent` → `partial` (not `present`: that
  row separately names catalog-driven model choice, upscale and audio generation, all still
  deferred to Task 7). The build-order projection, its priority-tier counts and the per-table
  present/partial/absent counts were re-derived and independently re-verified by direct row count
  against the two tables (4 present, 13 partial, 17 absent; 30 build-order items) before committing,
  not merely computed once and trusted.

### CI incidents hit and fixed during this task (kept for the audit trail)

1. **Run `32389768177` (first push, commit `f0841f7`) failed at 2m32s — a link failure.**
   `palmier_app_composition_tests` and `palmier_e2e_tests` are the only two test targets that
   compile `src/app/ApplicationComposition.cpp` as a standalone source path rather than linking the
   real `Palmier::services` library, so neither picked up the new `.cpp`'s symbols — confirmed by
   reading the failed job's log directly (`undefined reference to
   'palmier::services::openSslGenerativeHttpTransportAvailable()'` at the exact call site). Fixed in
   commit `8d75e98` by adding `OpenSslGenerativeHttpTransport.cpp` to both targets' source lists.
2. **Run `32390489582` (after the link fix) failed at 2m22s — this time three real, distinct bugs**,
   found by reading the actual test-failure log rather than assuming the same bug recurred:
   - `ARealHandshakeSucceedsAndTheResponseRoundTrips` asserted success with
     `verifyServerCertificate = true` against a self-signed fixture certificate no trust store
     contains — exactly the condition the transport is supposed to reject. The test was wrong, not
     the transport; fixed by disabling verification for that test (certificate verification is
     covered by the two dedicated tests either side of it).
   - `AResponseThatNeverArrivesWithinTheIoTimeoutReportsTimeout` crashed the whole test binary with
     `SIGPIPE` (see the transport-level fix above — a real production robustness gap, not merely a
     test artifact).
   - The repository hygiene checker (Property 68) correctly flagged two of the new test's own
     literal credential strings (`"Bearer should-never-be-sent"`, `"a-byok-key-value"`) as
     opaque/credential-shaped with no self-describing placeholder word. Renamed both to include
     `test` (`test-should-never-be-sent`, `test-byok-key-value`), matching the project's own excuse
     vocabulary.
   All three fixed in commit `00df0ae`.
3. **Run `32391278384` (after all three fixes) passed: 1266/1266.** Re-reading the two prior
   summaries' claim that the existing end-to-end test already proved the whole submit/poll/fetch
   stack against a real transport, direct inspection of that test's body showed it only ever drove
   `submit()` — the test was misleadingly named. Renamed it
   (`ASubmitExchangeAgainstALocalHttpsEndpointCompletes`) and added the genuinely full-sequence test
   described above in commit `e1f196d`, verified green in run `32392161099`: 1267/1267.
4. **Run `32393206437` (the `docs/UPSTREAM_PARITY.md` re-score commit, `3939a4b`) failed: 2 of
   1267.** Both failures were in `ParityCheckFalsifiability` — `DetectsAnEntryMissingFromTheBuildOrderList`
   and `DetectsABuildOrderPriorityThatDisagreesWithItsTable` — the checker's own negative-control
   tests, which prove it CAN detect a defect by mutating the real, checked-in document at a literal
   hardcoded anchor string (`"22. denoise (tool category) — later\n"`, `"31. auto-update
   (capability area) — later"`). Re-scoring `generate` to `present` removed it from the build-order
   projection entirely, shifting every later item up by one — `denoise` is now item 21, `auto-update`
   is now item 30 — so both anchors silently stopped matching. Fixed in commit `8355878` by updating
   both hardcoded numbers to the real, current values; verified no other position-dependent (rather
   than name-keyed) reference exists in either `tests/docs/report_parser_test.cpp` or
   `tests/docs/parity_report_property_test.cpp`. Run `32393760684`: 1267/1267, fully green.

---

## Phase 2, Task 7 (landing the deferred generative backlog entries) — complete

**Task 7 is CI-verified green on `main` at commit `0e831fe` (run `32406836062`, completed success):
CTest reports `100% tests passed, 0 tests failed out of 1275`, with a total test time of 24.49 seconds.
The build completed, the headless launch smoke test mapped the editor and painted 5031 distinct colours,
and the keyboard-driven Add Video Track smoke action also passed. The implementation itself first went
green at `6784ec5` (run `32404256042`, also 1275/1275); the two commits after it fixed test-side
bookkeeping that the implementation had invalidated, recorded as incidents 5 and 6 below.**

### What was actually built

- **Catalog-driven model selection (PR 406).** `GenerationModelCatalog` now publishes a provider-grouped
  catalog with multiple providers and model capabilities; `generation.list_models` returns the catalog,
  `generation.generate` accepts only catalog model ids, and unknown ids are refused by name.
- **Upscale generation (PR 396).** `generation.generate` accepts the declared upscale mode only for models
  that serve it, validates the source clip and target resolution, and places the generated asset as one
  undoable edit.
- **Audio generation (PR 395).** The generation schema accepts audio media, source-or-prompt requests and
  model-declared duration bounds; generated audio is registered and placed through the same undoable
  media-library path as video and image generation.
- **Undo ownership and consistency.** Generated asset imports are attached to the session media library,
  removed on undo when imported by the placement command, and generated/request media types must agree.
  Documentation now presents the exact no-argument marker as its own blank-line-delimited paragraph.

### Verification evidence

The final CI log shows all model-catalog tests 1082–1085, upscale tests 1086–1087 and audio tests 1088–1089
passing, followed by the explicit CTest summary above. The completed backlog entries are PRs 406, 396 and
395; their acceptance checks were exercised by the corresponding generation property suites. The final
run also completed the editor launch smoke test successfully.

### CI incidents 5 and 6 — documentation edits invalidating test-side bookkeeping

Re-scoring the parity report and adding a tool are both structural changes that some tests encode
positionally. Two runs failed after the implementation was already green, and neither was a defect in
the shipped behaviour; both were stale test-side accounting that the change had invalidated.

- **Incident 5 — stale build-order position anchors (run `32405386000`, commit `51822a4`, 2 failures).**
  `ParityCheckFalsifiability.DetectsAnEntryMissingFromTheBuildOrderList` and
  `...DetectsABuildOrderPriorityThatDisagreesWithItsTable` mutate the **real, checked-in**
  `docs/UPSTREAM_PARITY.md` through literal anchors — `"21. denoise (tool category) — later"` and
  `"30. auto-update (capability area) — later"`. Moving `generation and upscaling` out of the build-order
  projection shifted every entry below it up by one, so both anchors no longer matched and the mutations
  became no-ops that produced no defect to detect. Fixed at `1cb492f` by re-anchoring to `20.` and `29.`
  and correcting the file header's stale "31 build-order items" to 29. The sibling assertion
  `EXPECT_EQ(report.buildOrder.size(), needingPriority)` needed no change because it counts the parsed
  `absent`/`partial` entries rather than hard-coding a total.
- **Incident 6 — a new hook-backed tool absent from the test's accounting (run `32406052275`, commit
  `1cb492f`, 1 failure).** `McpProtocolProperties.ToolsCallSuccessShape` draws a tool uniformly from the
  whole registry (`registry.tools()[drawIndex(registry.size())]`) and requires `isError` false unless the
  tool is hook-backed, where it instead requires `isError` true naming the tool. `generation.list_models`
  is built with `guardedHookHandler`, so in a test binary — where the composition root wires no catalog —
  it correctly answers `Unsupported` with "no model catalog is configured". The test's `isHookBacked()`
  still named only the original trio, so the property demanded success from a tool that cannot have it.
  This was **seed-dependent, not introduced by the documentation commit**: the failing run reported
  `seed=15603858946716445787`, falsifiable after 95 tests, and the same test had passed at `6784ec5`
  only because no earlier seed drew that tool. Fixed at `0e831fe` by adding `generation.list_models` to
  `isHookBacked()`. The production error path was left untouched — it already reports the tool by name,
  which is what the hook-backed branch asserts.

The lesson recorded for later phases: after adding a tool, check every test that enumerates the registry,
and after re-scoring the parity report, check every test that anchors into it by position.

---

## Phase 3, Task 8 (ripple editing and gap management) — complete

**Task 8 is CI-verified green on `main` at commit `218620d` (run `32443629009`, completed success):
CTest reports `100% tests passed, 0 tests failed out of 1285`. The build completed, the headless launch
smoke test mapped the editor and painted 5031 distinct colours, and the keyboard-driven Add Video Track
smoke action also passed. The domain core and Tool_Surface first went green at `4f41a01` (run
`32408799695`, only the CTest suite itself failing — see incident 7); shell wiring and dedicated tests
landed together at `899090d` (run `32409848021`, 1285/1285) once the scenario fix inside that same commit
resolved incident 7; the parity re-score and PR 397 backlog update then reopened the suite at `cf77f71`
(run `32410542571`, 5 failures — incident 8), fixed at `218620d`.

### What was actually built

- **`RippleDeleteCommand`** (`src/core/EditCommands.{hpp,cpp}`) removes a clip and shifts every later
  clip on its track earlier by exactly the removed clip's duration, atomically reverting on any
  invariant violation.
- **`RippleTrimCommand`** mirrors `TrimClipCommand`'s clamping (one-frame minimum, `[0, sourceDuration]`)
  for a named clip's edge, then — satisfying upstream PR 397 — applies the identical source-time delta to
  every other member of any `Project.clipGroups` entry naming the clip, on that member's own track,
  refusing the whole edit if any member cannot absorb it. `Edge::End` shifts followers by the delta;
  `Edge::Start` leaves the trailing edge, and therefore every follower, fixed.
- **`CloseGapCommand`** shifts every later clip on a track earlier by the gap following a named clip,
  refusing when the clip is last on its track or no gap follows. Durations and source ranges are
  untouched.
- **Tool_Surface**: `timeline.ripple_delete`, `timeline.ripple_trim` (schema identical to
  `timeline.trim_clip`) and `timeline.close_gap`, registered beside the clip edits they are variants of,
  and documented in `docs/TOOLS.md`.
- **Editor_Shell**: `GuiToolGateway::rippleDelete/rippleTrim/closeGap`, and three `MainWindow` Edit-menu
  actions (`rippleDeleteAction_`, `rippleTrimAction_`, `closeGapAction_`) enabled only where
  `InspectorViewModel::hasSelection()` is true, matching the existing delete/split actions'
  `refreshSelectionActions()` pattern exactly. "Ripple Trim to Playhead" converts the playhead's timeline
  position to the selected clip's source time before dispatching `RippleTrimCommand::Edge::End`.
- **Tests** (`tests/core/edit_commands_test.cpp`): one-Undo and invariant-holds coverage for all three
  commands, an unknown-clip no-op case, and two tests exercising PR 397's own acceptance check directly —
  `RippleTrimCommand.KeepsGroupedMulticamAnglesSynchronised` (two clips on different tracks in one
  `clipGroups` entry move by the identical delta, a third ungrouped clip on a third track is untouched,
  the whole cross-track change undoes in one entry) and
  `RippleTrimCommand.AGroupedAngleThatCannotAbsorbTheTrimRefusesTheWholeEdit` (a member with no source
  headroom refuses the entire edit, leaving every track byte-for-byte unchanged).
- **`docs/PORT_BACKLOG.md`**: PR 397 moved `not-started` → `complete`, `linux-component` updated from
  speculative future-tense placeholders to the actual `RippleTrimCommand`/`timeline.ripple_trim` names,
  with a `note:` citing the two tests above.
- **`docs/UPSTREAM_PARITY.md`**: `clips` (table 1) `partial` → `present` (ripple-delete, ripple-trim and
  close-gap close the row's own stated gap). `multicam` (both tables) `absent` → `partial` (grouped
  trim-sync now exists; angle-switching still does not, so the row stops short of `present`). `timeline
  editing`'s rationale updated to name the three new selection-gated actions and drop the now-false
  "ripple" half of its remaining-gap claim. `linux-ref` advanced to `899090d` (the commit the re-scored
  rows were actually read from) and `comparison-date` to `2026-08-21`. Build order re-derived: 28 entries
  (was 29; `clips` left the list), 2 `must`/16 `should`/10 `later` (was 17 `should`); counts 6
  `present`/13 `partial`/15 `absent` (was 5/12/17).

### Verification evidence

The final CI log shows the eight new `RippleDeleteCommand`/`RippleTrimCommand`/`CloseGapCommand` unit
tests passing (1275 → 1285 total), the multicam-sync and multicam-refusal tests specifically among them,
followed by the explicit CTest summary above. `docs/TOOLS.md`'s consistency checker and both parity/backlog
falsifiability suites passed against the re-scored documents.

### CI incidents 7 and 8

Both incidents were test-side or documentation-side bookkeeping that the change had invalidated, not
defects in the shipped domain/tool/shell code — the same category as incidents 5 and 6 in Task 7.

- **Incident 7 — a new tool absent from the documentation scenario's accounting (run `32408799695`,
  commit `4f41a01`, 1 failure).** `DocumentationConsistency.EveryToolWithARegistryOwnedResultWasActuallyInvoked`
  asserts that every tool the registry renders a result for was actually invoked by the checker's own
  scenario, so the separate result-field check (`TheDocumentedResultFieldsAreTheFieldsTheHandlersReturn`)
  cannot pass vacuously by observing nothing. It failed with `missing = {generation.generate,
  timeline.close_gap, timeline.export, timeline.ripple_delete, timeline.ripple_trim}` against
  `expectedMissing = {generation.generate, timeline.export}` — the three new tools were real gaps in the
  scenario, not defects in the allow-list. Fixed at `899090d` by exercising all three inside
  `observeRealResults()`: the split clip's own right-hand piece is ripple-trimmed (`end` edge, shortening
  it), the gap that leaves before the next clip is closed, and the shortened piece is then removed with
  `timeline.ripple_delete` — chosen specifically because nothing later in the scenario references that
  piece by id, so the reorder/effect/transition/delete calls immediately after it are undisturbed.
- **Incident 8 — an over-length rationale and a stale single-row anchor from the parity re-score (run
  `32410542571`, commit `cf77f71`, 5 failures).** Two failures were direct: the rewritten `multicam`
  (table 1, 278 characters) and `timeline editing` (285 characters) rationales exceeded
  `ReportParser.cpp`'s `kMaxRationale = 200` (Requirements 13.3/13.5), which
  `ParityReportDocument.TheCheckedInReportHasNoDefects` caught directly against the real document, and
  which cascaded into `ParityReportProperties.EveryWellFormedRevisionPassesTheParityCheck` and
  `...TheParityCheckDetectsEveryMalformation` (both assert `real.defects.empty()` against the same real
  document). The other two were literal anchors keyed to `clips` specifically:
  `ParityCheckFalsifiability.DetectsAStatusOutsideItsValueSet` mutated the literal string `"| clips |
  partial |"`, which stopped matching the instant `clips` became `present`; and `...DetectsAnOverlongRationale`
  took `report.entriesIn(ParityTable::ToolCategory).front()` on the assumption that the first tool-category
  row would always still need a priority, which broke for the same reason. Fixed at `218620d`: both
  rationales rewritten to fit within the 200-character bound while keeping their factual content: the
  status anchor retargeted to `timeline` (a row that is genuinely, stably `partial`); and the overlong-
  rationale test rewritten to locate its subject via `std::find_if(..., requiresPriority())` rather than
  by table position, so it no longer depends on which row happens to be first.

The lesson recorded for later phases, extending the one above: a positional/literal dependency on a
*specific named row* is just as fragile as a dependency on *row order* — re-scoring an entry's status is
enough to break a test keyed to its old status, even without moving anything. And a rationale rewrite
needs its length checked against the document's own stated bound (`kMaxRationale`) before it is written,
not after CI reports it.

Phase 3 Tasks 9–11, Phase 4 Tasks 12–15 and Phase 5 Tasks 16–17 remain unstarted.
