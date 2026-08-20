<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Upstream parity report

This is the Parity_Report of Requirement 13. It is a **checked** document: the parity checker in
`tests/support/ReportParser` parses it and fails the Verification_Suite on any defect
(Requirement 13.8). Every field below is written to be found by that parser, so a field it cannot
find is a defect in this document, not in the parser.

## Provenance (Requirement 13.4)

- upstream-repository: https://github.com/palmier-io/palmier-pro
- upstream-ref: unknown — the upstream repository is not reachable from the environment this
  comparison was made in, and no upstream snapshot, submodule or vendored tree exists in this
  repository, so no upstream commit identifier or release tag could be observed. The upstream
  side of every row below is therefore taken from the upstream description recorded in
  `.kiro/specs/end-to-end-editor-integration/requirements.md` ("Upstream reference"), which is
  also the source of the 22 tool-category names and the 12 capability-area names.
- linux-ref: 65fb3d9e524a9c1defe0ea08da5fd8623fd4e28d (branch `main`)
- comparison-date: 2026-08-20

## Status definitions (Requirement 13.7)

Status is decided by **reachability at the product surface** — that is, whether an operation can
actually be performed by a user of the Editor_Shell or by a caller of the Tool_Surface. Whether a
class exists, compiles or is unit-tested does not enter into it.

- **present** — every upstream operation in the entry is reachable in the Linux port, either
  through the Tool_Surface (`services::ToolRegistry`, as served by `tools/call`) or through the
  Editor_Shell (`ui::MainWindow` and its panels).
- **partial** — at least one, but not every, such operation is reachable.
- **absent** — no such operation is reachable. A component may exist and still score `absent`: if
  nothing registers it with the Tool_Surface and no panel exposes it, no operation of that entry
  can be performed, which is what this report measures.

## Field rules

Stated explicitly because the checker of task 12.3 is written against this document.

| Field | Values | Rule |
|---|---|---|
| `category` / `area` | the names in Requirements 13.1 and 13.2 | exactly once per table |
| `status` | `present` \| `partial` \| `absent` | exactly one |
| `linux-components` | comma-separated component names, or `none` | `none` only where no Linux component exists, which implies `status: absent` |
| `priority` | `must` \| `should` \| `later` | present if and only if `status` is `absent` or `partial`; `-` otherwise |
| `rationale` | 1–200 characters | present if and only if `priority` is present; `-` otherwise |
| `macos-framework` | framework name, or `-` | when set, `linux-replacement` must be set |
| `linux-replacement` | technology name \| `out-of-scope: <1–200 char reason>` \| `-` | as above |

Two conventions the checker must know:

- **An entry is identified by the pair (table, name).** `multicam` is both a tool category and a
  capability area; the two rows are different entries and both are required. Nothing else collides
  across the two tables.
- **`must` / `should` / `later`** mean what Requirement 13.3 says: `must` = needed for the Linux
  port to complete the import → edit → playback → save → open → export workflow; `should` = needed
  for upstream parity but not for that workflow; `later` = deferred beyond parity.

The only **ordered** list in this document is the build-order projection, so a checker may treat
every `N. ` line as a build-order item.

## Table 1 — upstream agent tool categories (22 entries, Requirement 13.1)

| category | status | linux-components | priority | rationale | macos-framework | linux-replacement |
|---|---|---|---|---|---|---|
| clips | partial | core::EditCommands, services::ToolRegistry timeline.add_clip/delete_clip/move_clip/trim_clip/split_clip/reorder_clips | should | Add, delete, move, trim, split and reorder are reachable; no ripple trim and no grouped edit exist, so a cut cannot be propagated across clips. | - | - |
| timeline | partial | core::TimelineEngine, services::ToolRegistry timeline.read/add_track/remove_track/set_track_muted, edit.undo, edit.redo, ui::TimelinePanel | should | Read, track add/remove/mute, undo/redo, a scrub/step/timecode playhead control and Add Video/Audio Track menu actions are reachable from the mounted TimelinePanel; no marker or zoom tool exists yet. | - | - |
| texts | absent | none | should | No text or title clip type, tool or text renderer exists anywhere in the tree, so titles and lower thirds cannot be created on Linux. | - | - |
| captions | absent | none | should | core::TrackKind has only Video and Audio; no caption track, burn-in path or sidecar export exists, so captions cannot be authored or delivered. | - | - |
| transcription | absent | services::TranscriptionService | should | TranscriptionService is unreachable: no tool registers it, the composition root never constructs it and no recognizer backend is bundled. | - | - |
| color | partial | core::EffectType::ColorGrade, core::ColorSpace, gpu::EffectKernels | should | Only one color_grade effect is reachable via timeline.add_effect; no curve, wheel, scope or LUT operation exists, so a grade cannot be shaped or judged. | - | - |
| effects | partial | core::EffectType, gpu::EffectKernels, gpu::Compositor, services::ToolRegistry timeline.add_effect, ui::InspectorPanel | should | Six effect types can be appended and the inspector panel is mounted and shows a selected clip's effect chain; no tool removes, reorders or re-parameterises an effect from any mounted UI. | - | - |
| denoise | absent | none | later | No denoise effect type and no denoise kernel exist in core::Effect or gpu::EffectKernels, so noise reduction is unavailable on every surface. | - | - |
| multicam | absent | core::ClipGroup | should | Nothing reads clipGroups: schema 1.1 only reserves it, and PR 397's RippleTrimCommand and timeline.ripple_trim are deferred, so angle-synced trims cannot be made. | - | - |
| organize | absent | none | later | The media library is a flat list; no bin, folder, tag, rating or colour-label operation exists, so a project cannot be organised. | - | - |
| layout | partial | core::EffectType::CropTransform, gpu::EffectKernels | later | Only crop_transform changes clip geometry; there is no multi-clip layout or picture-in-picture operation, so composing more than one clip on screen is not possible regardless of the shell now being mounted. | - | - |
| media | partial | services::MediaImportService, core::MediaManager, services::ToolRegistry media.import/media.list, ui::MediaBrowserPanel | should | Import and list are reachable and the media browser panel is mounted, showing the library and a selected clip's retained versions and key moments; no tool removes, relinks or re-probes an asset. | - | - |
| import | present | services::MediaImportService, media::MediaProbe, media::ImportValidation, services::ToolRegistry media.import | - | - | - | - |
| export | present | services::ExportCoordinator, media::ExportEngine, media::MediaEncoder, media::EncoderSelector, services::ToolRegistry timeline.export | - | - | - | - |
| generate | absent | services::GenerativeBackendRegistry, services::HostedGenerativeBackend, services::ByokGenerativeBackend, services::GenerativeHttpTransport, services::GenerativeClient, services::GenerativeMediaCoordinator, services::ToolRegistry generation.generate | should | 10.5 landed the backend registry and the hosted and BYOK clients, but no HTTPS transport is implemented in tree, so every configured backend fails at submit and no generation completes. | - | - |
| projects | present | services::ProjectSession, services::ProjectStore, services::ProjectSaveService, services::ToolRegistry project.create/project.open/project.save/project.info | - | - | - | - |
| project settings | partial | core::Project, services::ToolRegistry project.create/project.info | should | Frame rate, canvas and colour space are settable only at project.create and readable by project.info; no tool changes them later and no settings panel exists. | - | - |
| search | absent | none | later | No search index, search tool or search field exists; assets and clips can only be listed in full, so a project cannot be searched. | - | - |
| sync | absent | none | later | No synchronisation operation exists on any surface, neither audio-based angle alignment nor project or account sync; this category's upstream meaning is unconfirmed. | - | - |
| beats | absent | services::KeyMomentDetector, services::KeyMomentMarkers | later | The key-moment components have no bundled analysis backend and no tool or panel reaches them, so no beat or key-moment marker can be produced. | - | - |
| capture frame | absent | none | should | No still-frame capture exists: ExportCoordinator writes only timeline video, and nothing saves the presented preview frame to an image file. | - | - |
| words | absent | none | later | Word-level transcript editing has no component: no transcript is reachable at the product surface and no word-addressed trim operation exists. | - | - |

## Table 2 — upstream user-facing capability areas (12 entries, Requirement 13.2)

| area | status | linux-components | priority | rationale | macos-framework | linux-replacement |
|---|---|---|---|---|---|---|
| timeline editing | partial | core::TimelineEngine, core::EditCommands, services::ToolRegistry, ui::TimelineViewModel, ui::TimelineModel, ui::TimelinePanel, ui::MainWindow, ui::InspectorViewModel, ui::MediaBrowserViewModel | must | The shell is mounted and every edit is now reachable from it: clip selection drives the Inspector, Add Video/Audio Track and Place at Playhead create tracks and clips, and a scrub/step/timecode control positions the playhead — all through the same Tool_Surface path the MCP endpoint and the agent use. The timeline itself is still a QTreeView rather than a graphical time-axis view (no drag-to-move, drag-to-trim or clip-rectangle rendering), and there is no ripple trim, grouped edit or effect removal/reorder from any mounted UI. | SwiftUI | Qt 6 Widgets |
| multicam | absent | core::ClipGroup | should | Nothing reads clipGroups, and ripple trim and angle switching are deferred with PR 397, so multi-angle footage cannot be cut in sync. | - | - |
| transcription and captions | absent | services::TranscriptionService | should | No recognizer backend is bundled and nothing reaches TranscriptionService; there is also no caption track kind, so captions cannot be produced or burned in. | - | - |
| text and graphics | absent | none | should | No text, title or shape layer exists in the domain core or the renderer, so on-screen graphics cannot be authored at all. | - | - |
| color and effects | partial | gpu::EffectKernels, gpu::Compositor, core::EffectType, services::ToolRegistry timeline.add_effect | should | Six effects including the ported invert_colors render on both paths; nothing removes, reorders or edits an effect and there is no LUT, scope or denoise. | - | - |
| audio scrub and metering | absent | none | should | The audio pipeline mixes and outputs, but no level meter, waveform or scrub-audio component exists, so levels cannot be monitored while editing. | SwiftUI | Qt 6 Widgets |
| generation and upscaling | absent | services::GenerativeBackendRegistry, services::HostedGenerativeBackend, services::ByokGenerativeBackend, services::GenerativeHttpTransport, services::GenerativeClient, services::GenerativeMediaCoordinator | should | Backend selection is reachable, but the sole in-tree transport reports Unsupported, so nothing generates; no model catalog (PR 406), upscale mode (PR 396) or audio generation (PR 395). | - | - |
| project browser and search | partial | ui::MediaBrowserViewModel, ui::MediaBrowserPanel | later | The media browser panel is mounted and lists the library, but there is no bin, folder or tag structure and no search index or tool exists, so projects and assets can be browsed only as a flat list and cannot be searched. | SwiftUI | Qt 6 Widgets |
| MCP and agent chat | partial | services::McpServer, services::McpProtocolHandler, services::McpSessionRegistry, services::RemoteAccessGate, services::AgentOrchestrator, services::OfflineIntentInterpreter, ui::AgentChatPanel | must | initialize, tools/list and tools/call work over JSON-RPC 2.0, the offline interpreter maps utterances, and the agent chat panel is mounted (tabbed with the Inspector); no SSE stream and no tools/list_changed. | SwiftUI | Qt 6 Widgets |
| settings | partial | app::AppSettings, app::AppConfig | should | Defaults, config file, environment and flags are honoured at startup only; nothing changes a setting at runtime and there is no preferences surface. | SwiftUI | Qt 6 Widgets |
| telemetry | absent | none | later | No telemetry, metrics or crash-reporting component exists in the tree, and Requirements 1 to 16 do not ask for one, so nothing is collected or reported. | - | - |
| auto-update | absent | none | later | No in-app update check exists; Linux delivery is by distribution packaging (deb, flatpak and AppImage under packaging/), so updates arrive through the package manager. | - | - |

## Build order (Requirement 13.9)

Exactly the `absent` and `partial` entries of both tables — 31 of the 34 — sorted `must` before
`should` before `later`. This list is a **projection** of the two tables and carries no
independent facts: an entry appears here if and only if its status above is `absent` or `partial`,
with the priority recorded above. The three omitted entries are the three `present` ones:
`import`, `export` and `projects`, all in table 1.

Each item is written `<name> (<table>) — <priority>` because `multicam` appears in both tables.

1. timeline editing (capability area) — must
2. MCP and agent chat (capability area) — must
3. clips (tool category) — should
4. timeline (tool category) — should
5. texts (tool category) — should
6. captions (tool category) — should
7. transcription (tool category) — should
8. color (tool category) — should
9. effects (tool category) — should
10. multicam (tool category) — should
11. media (tool category) — should
12. generate (tool category) — should
13. project settings (tool category) — should
14. capture frame (tool category) — should
15. multicam (capability area) — should
16. transcription and captions (capability area) — should
17. text and graphics (capability area) — should
18. color and effects (capability area) — should
19. audio scrub and metering (capability area) — should
20. generation and upscaling (capability area) — should
21. settings (capability area) — should
22. denoise (tool category) — later
23. organize (tool category) — later
24. layout (tool category) — later
25. search (tool category) — later
26. sync (tool category) — later
27. beats (tool category) — later
28. words (tool category) — later
29. project browser and search (capability area) — later
30. telemetry (capability area) — later
31. auto-update (capability area) — later

Counts, so a reader can check the projection without re-deriving it: 34 entries total — 3 `present`,
11 `partial`, 20 `absent`; 31 in this list, of which 2 `must`, 19 `should` and 10 `later`. Per table:
table 1 holds 22 entries (3 `present`, 7 `partial`, 12 `absent`, so 19 appear below) and table 2
holds 12 (0 `present`, 4 `partial`, 8 `absent`, so all 12 appear below).

## Known limits of this comparison

Recorded because Requirement 13.7 makes every status a factual claim about upstream, and three of
those claims rest on weaker evidence than the rest.

- **No upstream tree was observed.** `upstream-ref` is `unknown` for the reason given in the
   provenance block. The upstream side of this report is the description in
   `requirements.md` ("Upstream reference"), which enumerates the 22 tool categories, the module
   layout and the MCP surface, but not the individual operations inside each category. Every
   Linux-side claim, by contrast, was read from this tree at `linux-ref`.
- **The three `present` rows are the weakest.** `import`, `export` and `projects` are scored
   `present` because every operation this repository's own requirements define for them
   (Requirements 2, 7 and 3 respectively) is reachable through the Tool_Surface, and no missing
   operation is known. Because upstream's per-category operation list could not be enumerated, an
   upstream operation outside those requirements would make the row `partial`. Every other row
   cites a specific missing Linux-side operation, so it does not depend on that assumption.
- **Three category names are ambiguous without the upstream tree**: `sync` (audio-based angle
   alignment, or project/account sync), `layout` (clip layout and picture-in-picture, or editor
   panel layout) and `organize` (bins and folders, or tagging). Each is scored the same under
   either reading — nothing of the kind is reachable, and for `layout` only `crop_transform` is —
   so the ambiguity does not change the status, but it does mean the rationale names the Linux gap
   rather than the upstream feature.
- **macOS framework attribution is deliberately shallow.** The only macOS-only framework this
   tree evidences is **SwiftUI**, recorded in `requirements.md` as the upstream UI technology; its
   Linux replacement is Qt 6 Widgets, as the shell design and the PR 404 and PR 408 entries in
   `docs/PORT_BACKLOG.md` describe. No evidence was found for any other upstream framework
   dependency, so no other row names one rather than guessing at Metal, AVFoundation, Speech or an
   updater framework. Those rows carry `-`, which asserts nothing.
- **`present` is about reachability, not about verification.** `export` is `present` because the
   full export path is reachable through `timeline.export`; hardware encode on it remains
   unverified, because no host in this environment has a hardware or software H.264 encoder — see
   the task 9.8 note in `tasks.md`. Reachability and verification are tracked separately by design.
- **The two generation rows have been re-scored, and they did not move.** The previous revision of
  this report predicted that when task 10.5 landed, `generate` (tool category) and `generation and
  upscaling` (capability area) would move from `absent` to `partial`. 10.5 has landed (`f0a7925`),
  and this revision re-read both rows against it at the new `linux-ref`. What landed is real:
  `services::GenerativeBackendRegistry` compiles all three backends in, `selectGenerativeBackend()`
  is called by `ApplicationComposition`, `--generative-backend hosted|byok` is honoured from the
  shipped binary now that `main.cpp` wires `AppSettings`, and the `generation.generate` hook asks
  the selected backend for `unmetPrecondition()` before anything downstream runs. **But the
  prediction rested on a premise that is false in this tree**: it assumed that only the *offline
  default* completes no generation. In fact **no** configuration completes one, because the tree
  implements no HTTPS transport — `services::GenerativeHttpTransport` is a declared seam whose only
  implementation in product code is `makeUnavailableGenerativeHttpTransport()`, there is no
  build option or `#ifdef` that supplies another, and `hosted` and `byok` therefore fail at
  `submit` with `Unsupported` having sent no bytes. So the answer to the question this report
  measures — can any operation of the entry actually be performed at the product surface? — is
  still no, for every entry operation: prompt-to-video, prompt-to-image, catalog-driven model
  choice (PR 406), upscale (PR 396) and audio generation (PR 395). `partial` requires **at least
  one** reachable operation, and every row scored `partial` above cites a user operation that
  *completes*; a request path that is always refused is not one, which is precisely what the
  `absent` definition means by "a component may exist and still score `absent`". Both rows keep
  `absent` with `should`, so the build-order projection and the status counts are unchanged; only
  their `linux-components` and `rationale` moved, because the *reason* changed — the blocker is no
  longer "the only installed backend refuses" but "no transport is implemented". Implementing that
  one interface is what would make these rows `partial`.
- **`linux-ref` is a moving target.** It records the commit this comparison was read from. Work on
   this feature is ongoing, so a later revision of this report should update `linux-ref` and
   re-check every row whose components have changed. At the current `linux-ref` that re-check was
   done by diffing `src/` against the previous ref: the only product changes are task 10.5's
   generative backends (both generation rows re-read above), `main.cpp` and `AppSettings` finally
   wiring the configuration surface into the shipped binary (which makes the `settings` row's
   "honoured at startup only" claim more literally true, not less), and `core::AddClipCommand` now
   registering a clip's asset in `Project.assets` (a document-integrity fix that adds no operation).
   **`src/ui/` is byte-identical to the previous ref**, so every row that cites task 11.2 or 11.3 —
   the unmounted panels and the placeholder `MainWindow` — stands unchanged, and stage 11 remains
   unbuilt.
- **2026-08-20 re-check (`.kiro/specs/usable-editor` Phase 1).** `src/ui/` is no longer
  byte-identical to the previous ref: the shell was mounted (task 11.2/11.3, already reflected in
  the `end-to-end-editor-integration` spec) and this pass closed the gap that discovery surfaced —
  the mounted shell had no clip selection, no track-creation affordance, no clip-placement
  affordance and no playhead-positioning control, so `InspectorViewModel::selectClip()`,
  `GuiToolGateway::addTrack()` and `GuiToolGateway::addClip()` were reachable in code but had no
  caller anywhere in `src/`. Four changes closed it: `TimelinePanel` now reports its tree
  selection (clip and track) to `MainWindow`, which drives the Inspector and the two
  selection-gated Edit actions; `TimelineViewModel::addTrack()` plus two menu actions call the
  existing `GuiToolGateway::addTrack()`; a new "Place at Playhead" gesture plus a new
  library-asset-selection concept on `MediaBrowserViewModel` call the existing
  `GuiToolGateway::addClip()`; and `TimelinePanel` gained a scrub slider, frame-step buttons and an
  editable timecode field, all snapping to the project's edit frame rate. This re-scored the
  `timeline`, `timeline editing`, `effects`, `layout`, `media`, `project browser and search` and
  `MCP and agent chat` rows above, each citing exactly what changed. It did **not** touch the
  `generate` row (`services::GenerativeHttpTransport` still has no real implementation) or add a
  graphical (non-tree) timeline view, ripple editing, effect removal/reorder, or any of the
  `absent` rows below `timeline editing` in the build order; those remain open exactly as scored.
