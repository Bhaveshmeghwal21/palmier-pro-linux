# Requirements Document

## Introduction

The `end-to-end-editor-integration` spec is complete: the editor builds, 1239 of 1239 tests pass, and
the Qt shell launches and renders its five docks (verified by a CI screenshot, run `32361381399`).
What that spec did **not** establish is that a person can actually *edit* with it.

This spec covers the work needed to make Palmier Pro for Linux **usable**, against a deliberately
concrete threshold:

> **Usable means:** a person who has never seen this tool can install it, launch it, import their own
> footage, cut a multi-clip video with titles, play it back, and export a finished deliverable —
> using only the GUI, without invoking the MCP endpoint, without hand-editing a `.palmier` JSON
> document, and without a developer's help.

The audit below establishes that the editor currently fails that threshold at the very first step,
and — importantly — that it fails for a much smaller reason than the parity report suggests. Almost
all of the domain logic, the tool surface, the undo/redo stack and even the GUI's own
`GuiToolGateway` methods already exist and are covered by passing tests. What is missing is a small
number of **unwired connections in the presentation layer**. That distinction drives the phasing:
Phase 1 is a handful of wiring tasks that convert the GUI from read-only to genuinely editable.

## Scope decision

Unlike the predecessor spec, this one **is** explicitly phased by priority, because the phases differ
enormously in cost and the earliest is disproportionately cheap:

| Phase | Theme | Blocking severity |
|---|---|---|
| 1 | Wire the existing edit plumbing to GUI affordances | **Total** — no GUI editing is possible at all today |
| 2 | Make editing tolerable rather than merely possible | Severe friction |
| 3 | Content creation most deliverables need (text, captions, frame capture) | Blocks most real deliverables |
| 4 | The generative-AI premise (one missing transport) | Blocks the product's headline feature |
| 5 | Distribution and documentation accuracy | Blocks non-developer adoption |

Phase 1 is a prerequisite for every later phase being *observable* by a user. Phase 4 is called out
separately in the sequencing note below because its value-to-effort ratio is the highest in the
document and it does not depend on Phases 2 or 3.

## Codebase audit findings (verified against source at commit `1bc20d1` on `main`)

Every row was confirmed by reading the named file. The "Evidence" column gives the exact location, so
each claim is re-checkable rather than asserted.

### The blocking discovery: the GUI cannot select, create, or place anything

| # | Finding | Evidence |
|---|---|---|
| 1 | **Nothing sets the Inspector's clip selection, so it is permanently empty.** `InspectorViewModel::selectClip()` is defined but has **no caller anywhere in `src/`**. `MainWindow` never connects the timeline tree's selection signal to it (no `selectionModel`, `currentChanged`, `clicked` or `activated` connection exists in the file). Clicking a clip in the timeline therefore does nothing. | `src/ui/InspectorViewModel.cpp:181` defines it; grep for callers across `src/ui/*.cpp` returns only the definition. The `selectClip` call at `src/ui/MediaBrowserPanel.cpp:117` is a *different* method — `MediaBrowserViewModel::selectClip` (`src/ui/MediaBrowserViewModel.cpp:102`), which selects a clip for **version listing**, not for the Inspector. |
| 2 | **Consequently `Edit > Delete Clip` and `Edit > Split at Playhead` can never act.** Both early-return on `!inspectorViewModel_.hasSelection()` and show "No clip is selected". Since finding 1 means that predicate is always false, both actions are dead code paths at runtime. | `src/ui/MainWindow.cpp`, `onDeleteClip()` and `onSplitAtPlayhead()`. |
| 3 | **Every Inspector edit is unreachable for the same reason.** `InspectorViewModel`'s trim, opacity, gain and add-effect gestures are implemented, gateway-routed and unit-tested, but all require a selection that nothing establishes. | `src/ui/InspectorViewModel.cpp`; tests in `tests/ui/inspector_viewmodel_test.cpp` drive `selectClip()` directly, which is why they pass while the GUI cannot. |
| 4 | **No GUI affordance creates a track.** `GuiToolGateway::addTrack()` exists and is tested; no menu action, button or panel calls it. `TimelineViewModel` does not even expose an `addTrack` gesture — only the `canAddTrack()` predicate. | `src/ui/GuiToolGateway.hpp:89` / `.cpp:125` define it; no caller in `src/ui/`. `src/ui/TimelineViewModel.hpp:197` has `canAddTrack()` with no corresponding `addTrack()`. |
| 5 | **No GUI affordance places a clip on the timeline.** `GuiToolGateway::addClip()` and `TimelineViewModel::addClip()` both exist and are tested; no widget calls either. | `src/ui/GuiToolGateway.hpp:80`, `src/ui/TimelineViewModel.hpp:240`, `src/ui/TimelineViewModel.cpp:135`; no caller in any panel or `MainWindow`. |
| 6 | **There is no drag-and-drop and no context menu anywhere in the UI layer.** Grep across `src/ui/*.{cpp,hpp}` for `dragEnter`, `dropEvent`, `setAcceptDrops`, `setDragEnabled`, `setDragDropMode`, `contextMenu` and `CustomContextMenu` returns **zero** matches. So the usual route from a media-library entry to a timeline track does not exist in any form. | `src/ui/` — no matches. |
| 7 | **The playhead cannot be positioned.** The Playback menu offers only Play/Pause, Stop and Go to Start. `PreviewView::seekSeconds()` exists and is reachable programmatically, but no scrub bar, time field or click-to-seek affordance is wired, so "Split at Playhead" could only ever split at wherever playback happened to pause. | `src/ui/MainWindow.cpp` `buildMenus()`; `onGoToStart()` calls `seekSeconds(0.0)`. |
| 8 | **The "timeline" is a tree, not a timeline.** `TimelinePanel` is a `QTreeView` over `TimelineModel` (tracks as rows, clips as child rows) plus transport buttons. There is no time axis, no clip rectangles, no zoom and no ruler, so clip position and duration are not visually represented at all. | `src/ui/TimelinePanel.hpp` class comment and members (`QTreeView* tree_`). |

**Net effect:** the GUI today supports project create/open/save, media import into the library,
playback, and export. It supports **no editing whatsoever**. The predecessor spec's Requirement 1.7
("a user triggers an edit through any of the five panels") is satisfied in the view-model and gateway
layers, and is proven by `EditEquivalenceProperties.GuiMcpAgentProduceIdenticalState`, but no user
gesture can reach it.

### Gaps confirmed still open from the parity report

| # | Finding | Evidence |
|---|---|---|
| 9 | **Generative AI cannot complete in any configuration.** The only implementation of `GenerativeHttpTransport` in product code is `UnavailableGenerativeHttpTransport`, and `transportFor()` installs it whenever nothing is injected. Every hosted and BYOK backend therefore fails at `submit` having sent no bytes. Everything else in the stack — registry, hosted client, BYOK client, job lifecycle, auth gating, the coordinator — is built and tested. | `src/services/GenerativeHttpTransport.cpp:80` (the sole subclass), `:145` (the factory); `src/services/GenerativeBackendRegistry.cpp:84-88,191`. |
| 10 | **Effects can be added but never removed, reordered or re-parameterised** by any surface, so a mistaken effect is unrecoverable except by immediate undo. | `docs/UPSTREAM_PARITY.md`, `effects` row. |
| 11 | **Project settings are fixed at creation.** Frame rate, canvas and colour space are settable only via `project.create` and readable via `project.info`; no tool changes them later and no settings panel exists. | `docs/UPSTREAM_PARITY.md`, `project settings` row. |
| 12 | **No ripple trim and no grouped edit**, so a cut cannot be propagated across clips and deleting a clip leaves a gap the user must close by hand. | `docs/UPSTREAM_PARITY.md`, `clips` row; upstream PR 397 deferred. |
| 13 | **Text/titles, captions, transcription, denoise, multicam, organise, search, sync, beats, still-frame capture and word-level editing are all `absent`.** `core::TrackKind` has only `Video` and `Audio`. | `docs/UPSTREAM_PARITY.md`. |
| 14 | **No packaging exists.** The project is built from source per `docs/BUILD.md`; there is no Flatpak, AppImage, `.deb` or release artifact, so a non-developer cannot install it. | Absence of any packaging manifest in the tree. |
| 15 | **`docs/UPSTREAM_PARITY.md` is stale and understates the project.** Multiple rows still assert "no timeline panel is mounted (task 11.3 unbuilt)" and "MainWindow is still a placeholder label, so a GUI user cannot edit at all (tasks 11.2, 11.3)". Stage 11 built all of those. The document's single `must`-priority row is therefore wrong in its stated reason — though, per findings 1–7, right in its conclusion, for different reasons. | `docs/UPSTREAM_PARITY.md`, `timeline`/`effects`/`media`/`layout` and `timeline editing` rows. |

### Sequencing recommendation

Phase 1 is unambiguously first: it is small, and until it lands no other user-visible improvement can
be exercised by a user at all.

**Phase 4 (the generative HTTPS transport) is recommended second, ahead of Phases 2 and 3**, on
value-to-effort grounds: it is a single class behind an already-defined narrow seam with six existing
test doubles, OpenSSL is already a linked dependency with TLS code in tree for the MCP server, and it
converts the product's entire headline capability from dead to live. Phases 2 and 3 are each larger
than Phase 4 and neither blocks it.

One honest caveat on Phase 4: implementing the transport makes the **BYOK** path genuinely functional,
because that path talks to third-party providers the user holds their own keys for. The **hosted**
path additionally requires the closed generative service to exist and be reachable, which is outside
this repository. Requirement 11 is therefore written to be verifiable against a local test endpoint,
not against the hosted service.

## Glossary

| Term | Meaning |
|---|---|
| **Editor_Shell** | `ui::MainWindow` and the five dock panels it owns. |
| **Tool_Surface** | `services::ToolRegistry`, the single named-tool entry point shared by the GUI, the MCP endpoint and the agent. |
| **Gui_Gateway** | `ui::GuiToolGateway`, which routes GUI gestures through the Tool_Surface. |
| **Timeline_Panel** | `ui::TimelinePanel`, the timeline dock. |
| **Inspector** | `ui::InspectorViewModel` and its panel. |
| **Media_Browser** | `ui::MediaBrowserViewModel` and its panel. |
| **Selection** | The single clip the Inspector currently targets, as reported by `InspectorViewModel::hasSelection()`. |
| **Transport** | `services::GenerativeHttpTransport`, the seam between a generative backend and the network. |

---

## Requirement 1: Clip Selection From The Timeline

**User Story:** As an editor, I want to click a clip in the timeline and have the inspector show it, so
that I can delete, split, trim and adjust that clip.

1. WHEN a user selects a clip row in the Timeline_Panel, THE Editor_Shell SHALL set the Inspector's
   Selection to that clip's identifier, and THE Inspector SHALL display that clip's source range,
   opacity, gain and effect list within 100 milliseconds of the selection changing.
2. WHEN a user selects a track row (rather than a clip row) in the Timeline_Panel, THE Editor_Shell
   SHALL clear the Selection, and THE Inspector SHALL display its no-selection state.
3. WHEN the Selection refers to a clip and that clip is removed from the project by any surface — the
   GUI, the MCP_Endpoint or the agent — THE Editor_Shell SHALL clear the Selection rather than leave
   it referring to an absent clip.
4. WHERE a clip is selected, THE Editor_Shell SHALL present `Edit > Delete Clip` and
   `Edit > Split at Playhead` in an enabled state; WHERE no clip is selected, THE Editor_Shell SHALL
   present both in a disabled state instead of showing a "No clip is selected" message after the fact.
5. WHEN a user activates `Edit > Delete Clip` with a clip selected, THE Editor_Shell SHALL remove
   exactly that clip through the Tool_Surface, and the removal SHALL be undoable in one Undo.

## Requirement 2: Creating Tracks From The GUI

**User Story:** As an editor, I want to add a video or audio track from the GUI, so that I have
somewhere to put my footage.

1. THE Editor_Shell SHALL present an activatable action that creates a video track and one that
   creates an audio track, both reachable from the menu bar.
2. WHEN a user activates either action, THE Editor_Shell SHALL create the track through the
   Tool_Surface, and THE Timeline_Panel SHALL show the new track within 200 milliseconds without
   further user action.
3. WHEN a track is created, THE new track SHALL be appended after the last existing track of the same
   kind, matching the ordering `core::AddTrackCommand` already guarantees.
4. IF the project already holds the maximum number of tracks of the requested kind, THEN THE
   Editor_Shell SHALL present the corresponding action in a disabled state and SHALL leave the project
   unchanged.
5. WHEN a track is created, the creation SHALL be undoable in one Undo, restoring the exact prior
   track list.

## Requirement 3: Placing Media On The Timeline From The GUI

**User Story:** As an editor, I want to put an imported asset onto a track, so that I can build a
sequence — this is the single gesture whose absence makes GUI editing impossible today.

1. THE Editor_Shell SHALL provide at least one gesture that places a Media_Browser asset onto a
   chosen track at a chosen timeline position, using the existing `Gui_Gateway::addClip`.
2. WHEN a user selects an asset in the Media_Browser, selects a target track, and activates the place
   gesture, THE Editor_Shell SHALL insert a clip carrying that asset's reference at the current
   playhead position, and THE Timeline_Panel SHALL show it within 200 milliseconds.
3. IF the requested position would overlap an existing clip on the target track, THEN THE
   Editor_Shell SHALL refuse the placement, SHALL report the refusal with an indication naming the
   overlap, and SHALL leave the project unchanged.
4. IF no asset is selected, or no track exists, THEN THE Editor_Shell SHALL present the place gesture
   in a disabled state.
5. WHEN a clip is placed, the placement SHALL be undoable in one Undo, and the resulting project state
   SHALL compare equal to the state produced by invoking `timeline.add_clip` with the same arguments
   through the MCP_Endpoint.
6. THE Editor_Shell SHALL additionally support dragging an asset from the Media_Browser onto a track
   row in the Timeline_Panel as an equivalent route to criterion 2, producing an identical project
   state.

## Requirement 4: Frame-Accurate Playhead Control

**User Story:** As an editor, I want to move the playhead to an exact frame, so that I can cut where I
intend to.

1. THE Editor_Shell SHALL present a playhead control that lets a user move the playhead to any
   position between zero and the timeline duration without starting playback.
2. WHEN a user enters or scrubs to a position, THE Editor_Shell SHALL move the playhead to the frame
   boundary nearest that position and SHALL update the preview to that frame within 200 milliseconds.
3. THE Editor_Shell SHALL present actions that step the playhead one frame backward and one frame
   forward, each moving it by exactly one frame interval at the project frame rate.
4. WHEN the playhead is moved by any means, THE displayed timecode SHALL agree with the playhead
   position to the frame.
5. IF a requested position is negative or beyond the timeline duration, THEN THE Editor_Shell SHALL
   clamp it to the nearer bound rather than reporting an error.

## Requirement 5: Ripple Editing And Gap Management

**User Story:** As an editor, I want deleting or trimming a clip to close the gap it leaves, so that I
do not have to reposition every following clip by hand.

1. THE Tool_Surface SHALL publish a ripple-delete operation that removes a clip and shifts every
   later clip on the same track earlier by exactly the removed clip's duration.
2. THE Tool_Surface SHALL publish a ripple-trim operation that changes a clip's out-point and shifts
   every later clip on the same track by exactly the change in duration.
3. WHEN either operation is applied, the result SHALL leave the track ordered by timeline start with
   no overlaps, and SHALL be undoable in one Undo.
4. THE Editor_Shell SHALL present both operations as activatable actions, enabled only WHERE a clip is
   selected.
5. THE Editor_Shell SHALL present a close-gap action that removes the gap immediately following a
   selected clip by shifting later clips earlier, leaving clip durations unchanged.

## Requirement 6: Effect Lifecycle Management

**User Story:** As an editor, I want to remove, reorder and re-adjust effects I have already applied,
so that a mistake is not permanent.

1. THE Tool_Surface SHALL publish operations that remove an effect from a clip, reorder a clip's
   effects, and change an existing effect's parameters.
2. WHEN a user removes an effect through the Inspector, THE Inspector SHALL reflect the removal within
   100 milliseconds, and the removal SHALL be undoable in one Undo.
3. WHEN a user changes an effect parameter, THE preview SHALL render the changed value at the current
   playhead within 200 milliseconds.
4. THE rendered result of a clip's effect list SHALL depend on effect order, and reordering SHALL
   change the rendered output accordingly on both the GPU and software paths.
5. IF an effect removal or reorder names an effect the clip does not carry, THEN THE Tool_Surface SHALL
   refuse the operation and leave the project unchanged.

## Requirement 7: Mutable Project Settings

**User Story:** As an editor, I want to change the frame rate, canvas size and colour space after
creating a project, so that a wrong choice at creation does not force me to start over.

1. THE Tool_Surface SHALL publish an operation that changes a project's frame rate, canvas resolution
   and colour space after creation, accepting the same ranges `project.create` accepts.
2. THE Editor_Shell SHALL present a settings surface that reads the current values and submits changes
   through that operation.
3. WHEN the frame rate changes, THE Project_Session SHALL preserve every clip's timeline position and
   source range as a duration, so that no clip is silently moved or retimed.
4. WHEN any setting changes, the change SHALL be undoable in one Undo and SHALL mark the project
   modified.
5. IF a requested value falls outside the documented range, THEN THE Tool_Surface SHALL refuse the
   change, SHALL name the offending setting, and SHALL leave the project unchanged.

## Requirement 8: A Graphical Timeline

**User Story:** As an editor, I want to see my clips laid out against time, so that I can judge and
adjust their positions and lengths directly.

1. THE Timeline_Panel SHALL render each track as a horizontal lane and each clip as a rectangle whose
   horizontal position and width correspond to its timeline start and duration.
2. THE Timeline_Panel SHALL render a time ruler and a playhead marker at the current playhead
   position.
3. WHEN a user clicks a position in the ruler or an empty part of a lane, THE Timeline_Panel SHALL move
   the playhead to that position, honouring Requirement 4's frame snapping.
4. THE Timeline_Panel SHALL support zooming the visible time range, and SHALL keep the playhead visible
   across a zoom change.
5. WHEN a user drags a clip rectangle horizontally, THE Timeline_Panel SHALL move that clip through the
   Tool_Surface, refusing and visually reverting a drop that would overlap another clip.
6. WHEN a user drags a clip's left or right edge, THE Timeline_Panel SHALL trim that clip's in- or
   out-point through the Tool_Surface, refusing a trim that would leave a non-positive duration.
7. THE Timeline_Panel SHALL indicate which clip is selected, consistent with Requirement 1.

## Requirement 9: Text And Titles

**User Story:** As an editor, I want to put a title and a lower third on screen, so that I can deliver
a video that names itself and its subjects.

1. THE domain core SHALL carry a text clip type holding at least the string, font family, point size,
   colour, alignment and screen position, and it SHALL round-trip through save and open.
2. THE Tool_Surface SHALL publish operations that create a text clip, change its string, and change its
   styling, each undoable in one Undo.
3. THE compositor SHALL render a text clip on both the GPU and software paths, producing results that
   agree within the tolerance the existing GPU/CPU parity property already applies to effects.
4. THE Editor_Shell SHALL present a surface for entering and styling text that shows the result in the
   preview at the current playhead.
5. WHEN a project containing text clips is exported, THE exported file SHALL show the text rendered
   identically to the preview at the corresponding frames.
6. IF a requested font family is unavailable on the host, THEN THE renderer SHALL substitute a
   documented default font and SHALL report the substitution rather than failing the render.

## Requirement 10: Captions And Transcription

**User Story:** As an editor, I want captions on my video, so that it is accessible and works on
social platforms.

1. `core::TrackKind` SHALL gain a caption track kind, and caption cues SHALL round-trip through save
   and open.
2. THE Tool_Surface SHALL publish operations that add, edit, retime and remove a caption cue, each
   undoable in one Undo.
3. THE export path SHALL support burning captions into the video and, separately, writing them as a
   sidecar file in a documented subtitle format.
4. THE Composition_Root SHALL construct `services::TranscriptionService` and THE Tool_Surface SHALL
   publish an operation that produces caption cues from a clip's audio.
5. IF no recognizer backend is available on the host, THEN the transcription operation SHALL report
   that precondition by name, SHALL leave the project unchanged, and SHALL NOT prevent captions from
   being authored by hand.

## Requirement 11: A Working Generative Transport

**User Story:** As a user with my own provider API key, I want prompt-to-media generation to actually
run, so that the product's central capability is not inert.

1. THE product SHALL provide an implementation of `services::GenerativeHttpTransport` that performs a
   real HTTPS request, verifies the server certificate by default, and returns the response status and
   body to its caller.
2. WHEN a generative backend submits, polls, or fetches, THE Transport SHALL carry the request the
   backend built — including its authorization header — unchanged, and SHALL NOT log any credential
   value.
3. THE Transport SHALL apply a request timeout and SHALL map a timeout, a connection failure and a
   TLS verification failure onto distinct error codes the existing backends already handle.
4. IF an endpoint is configured with a plaintext `http://` scheme, THEN THE Transport SHALL refuse the
   request without sending bytes, preserving the behaviour
   `HostedGenerativeBackend.APlaintextEndpointIsRefusedRatherThanSentTo` already asserts.
5. WHEN a BYOK credential for the requested provider is present and the endpoint is reachable, THE
   generation SHALL complete end to end and the generated media SHALL be placed on the timeline as one
   undoable edit, satisfying the path Property 65 already covers with a scripted transport.
6. THE verification of criteria 1–5 SHALL run against a local HTTPS test endpoint, so it does not
   depend on the closed hosted service.
7. THE Composition_Root SHALL install this Transport by default, and SHALL fall back to the
   unavailable transport only WHERE the build excludes TLS support.

## Requirement 12: Deferred Upstream Ports

**User Story:** As a maintainer, I want the deferred upstream changes landed once their prerequisites
exist, so that the port does not drift further behind.

1. WHEN Requirement 11 is satisfied, THE product SHALL land the deferred generative ports recorded in
   `docs/PORT_BACKLOG.md` — upscale, audio generation and catalogue-driven model selection — each with
   the acceptance check its backlog entry already declares.
2. WHEN Requirement 5 is satisfied, THE product SHALL land the deferred ripple-trim port, and its
   backlog entry SHALL move to `complete`.
3. WHEN a backlog entry is landed, `docs/PORT_BACKLOG.md` SHALL be updated in the same change, and the
   existing backlog-consistency test SHALL pass against the updated document.

## Requirement 13: Installable Distribution

**User Story:** As a Linux user who is not a developer, I want to install and run the editor without
building it, so that I can actually try it.

1. THE project SHALL publish at least one self-contained installable artifact for a documented set of
   distributions, bundling or declaring every runtime dependency the platform check requires.
2. WHEN a user installs that artifact on a host meeting the documented minimum specification and
   launches it, THE Editor_Shell SHALL become visible within the 15-second budget the predecessor spec
   set, with no manual dependency installation.
3. THE artifact SHALL be produced by CI from a tagged commit, and CI SHALL verify that the produced
   artifact launches and renders, reusing the launch smoke test already in `ci.yml`.
4. THE installed application SHALL carry the GPLv3 licence text and the `NOTICE` file, preserving the
   licensing split the repository documents.
5. IF a required runtime dependency is missing on the target host, THEN the application SHALL name each
   missing item, as `PlatformCompatibility` already does, rather than failing opaquely.

## Requirement 14: Documentation Accuracy

**User Story:** As anyone reading this repository, I want its parity report to describe the code as it
is, so that I can trust it when deciding what to work on.

1. `docs/UPSTREAM_PARITY.md` SHALL be re-scored against the current tree, and SHALL NOT state that the
   shell is a placeholder or that any stage-11 panel is unmounted.
2. THE re-scored report SHALL record, for each row whose status changes, the reason the status changed.
3. THE re-scored report SHALL reflect the findings of this document's audit — in particular that GUI
   editing is blocked by unwired selection and placement affordances rather than by absent panels.
4. WHEN any requirement in this spec is completed, the parity and backlog documents SHALL be updated in
   the same change, and the existing documentation-consistency tests SHALL pass.
