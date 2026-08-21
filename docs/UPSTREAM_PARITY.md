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
- linux-ref: c911c5ca94ce308a0b0ac0925e01ec658c73ef3d (branch `main`)
- comparison-date: 2026-08-21

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
| clips | present | core::EditCommands, services::ToolRegistry timeline.add_clip/delete_clip/move_clip/trim_clip/split_clip/reorder_clips/ripple_delete/ripple_trim/close_gap | - | - | - | - |
| timeline | partial | core::TimelineEngine, services::ToolRegistry timeline.read/add_track/remove_track/set_track_muted, edit.undo, edit.redo, ui::TimelinePanel | should | Read, track add/remove/mute, undo/redo, a scrub/step/timecode playhead control and Add Video/Audio Track menu actions are reachable from the mounted TimelinePanel; no marker or zoom tool exists yet. | - | - |
| texts | absent | none | should | No text or title clip type, tool or text renderer exists anywhere in the tree, so titles and lower thirds cannot be created on Linux. | - | - |
| captions | absent | none | should | core::TrackKind has only Video and Audio; no caption track, burn-in path or sidecar export exists, so captions cannot be authored or delivered. | - | - |
| transcription | absent | services::TranscriptionService | should | TranscriptionService is unreachable: no tool registers it, the composition root never constructs it and no recognizer backend is bundled. | - | - |
| color | partial | core::EffectType::ColorGrade, core::ColorSpace, gpu::EffectKernels | should | Only one color_grade effect is reachable via timeline.add_effect; no curve, wheel, scope or LUT operation exists, so a grade cannot be shaped or judged. | - | - |
| effects | present | core::EffectType, gpu::EffectKernels, gpu::Compositor, services::ToolRegistry timeline.add_effect/remove_effect/reorder_effects/set_effect_parameter, ui::InspectorPanel | - | - | - | - |
| denoise | absent | none | later | No denoise effect type and no denoise kernel exist in core::Effect or gpu::EffectKernels, so noise reduction is unavailable on every surface. | - | - |
| multicam | partial | core::ClipGroup, core::EditCommands::RippleTrimCommand, services::ToolRegistry timeline.ripple_trim | should | A ripple-trim on a grouped clip propagates the same source-time delta to every clipGroups member (PR 397); no angle-switching tool exists, so angles trim in sync but cannot be cut between. | - | - |
| organize | absent | none | later | The media library is a flat list; no bin, folder, tag, rating or colour-label operation exists, so a project cannot be organised. | - | - |
| layout | partial | core::EffectType::CropTransform, gpu::EffectKernels | later | Only crop_transform changes clip geometry; no multi-clip layout or picture-in-picture operation exists, so composing more than one clip on screen is not possible. | - | - |
| media | partial | services::MediaImportService, core::MediaManager, services::ToolRegistry media.import/media.list, ui::MediaBrowserPanel | should | Import and list are reachable and the media browser panel is mounted, showing the library and a selected clip's retained versions and key moments; no tool removes, relinks or re-probes an asset. | - | - |
| import | present | services::MediaImportService, media::MediaProbe, media::ImportValidation, services::ToolRegistry media.import | - | - | - | - |
| export | present | services::ExportCoordinator, media::ExportEngine, media::MediaEncoder, media::EncoderSelector, services::ToolRegistry timeline.export | - | - | - | - |
| generate | present | services::GenerativeBackendRegistry, services::HostedGenerativeBackend, services::ByokGenerativeBackend, services::OpenSslGenerativeHttpTransport, services::GenerativeClient, services::GenerativeMediaCoordinator, services::ToolRegistry generation.generate | - | - | - | - |
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
| timeline editing | partial | core::TimelineEngine, core::EditCommands, services::ToolRegistry, ui::TimelineViewModel, ui::TimelineModel, ui::TimelinePanel, ui::MainWindow, ui::InspectorViewModel, ui::MediaBrowserViewModel | must | The shell is mounted; selection drives the Inspector, actions create tracks/clips, a scrub moves the playhead, and ripple delete/trim/close-gap are selection-gated. No graphical timeline/effects yet. | SwiftUI | Qt 6 Widgets |
| multicam | partial | core::ClipGroup, core::EditCommands::RippleTrimCommand, services::ToolRegistry timeline.ripple_trim | should | A ripple-trim on a grouped clip propagates in sync to every clipGroups member (PR 397); angle switching is still deferred, so multi-angle footage trims together but cannot be cut between angles. | - | - |
| transcription and captions | absent | services::TranscriptionService | should | No recognizer backend is bundled and nothing reaches TranscriptionService; there is also no caption track kind, so captions cannot be produced or burned in. | - | - |
| text and graphics | absent | none | should | No text, title or shape layer exists in the domain core or the renderer, so on-screen graphics cannot be authored at all. | - | - |
| color and effects | partial | gpu::EffectKernels, gpu::Compositor, core::EffectType, services::ToolRegistry timeline.add_effect/remove_effect/reorder_effects/set_effect_parameter | should | Six effects including invert_colors render on both paths, and remove/reorder/re-parameterise are all reachable; there is still no curve, wheel, scope, LUT or denoise. | - | - |
| audio scrub and metering | absent | none | should | The audio pipeline mixes and outputs, but no level meter, waveform or scrub-audio component exists, so levels cannot be monitored while editing. | SwiftUI | Qt 6 Widgets |
| generation and upscaling | present | services::GenerationModelCatalog, services::GenerativeBackendRegistry, services::HostedGenerativeBackend, services::ByokGenerativeBackend, services::OpenSslGenerativeHttpTransport, services::GenerativeClient, services::GenerativeMediaCoordinator, services::ToolRegistry generation.list_models/generation.generate | - | - | - | - |
| project browser and search | partial | ui::MediaBrowserViewModel, ui::MediaBrowserPanel | later | The media browser panel is mounted and lists the library as a flat list; no bin, folder or tag structure and no search index exist, so assets cannot be organised or searched. | SwiftUI | Qt 6 Widgets |
| MCP and agent chat | partial | services::McpServer, services::McpProtocolHandler, services::McpSessionRegistry, services::RemoteAccessGate, services::AgentOrchestrator, services::OfflineIntentInterpreter, ui::AgentChatPanel | must | initialize, tools/list and tools/call work over JSON-RPC 2.0, the offline interpreter maps utterances, and the agent chat panel is mounted; no SSE stream or tools/list_changed. | SwiftUI | Qt 6 Widgets |
| settings | partial | app::AppSettings, app::AppConfig | should | Defaults, config file, environment and flags are honoured at startup only; nothing changes a setting at runtime and there is no preferences surface. | SwiftUI | Qt 6 Widgets |
| telemetry | absent | none | later | No telemetry, metrics or crash-reporting component exists in the tree, and Requirements 1 to 16 do not ask for one, so nothing is collected or reported. | - | - |
| auto-update | absent | none | later | No in-app update check exists; Linux delivery is by distribution packaging (deb, flatpak and AppImage under packaging/), so updates arrive through the package manager. | - | - |

## Build order (Requirement 13.9)

Exactly the `absent` and `partial` entries of both tables — 27 of the 34 — sorted `must` before
`should` before `later`. This list is a **projection** of the two tables and carries no
independent facts: an entry appears here if and only if its status above is `absent` or `partial`,
with the priority recorded above. The seven omitted entries are the seven `present` ones —
`import`, `export`, `projects`, `generate`, `clips` and `effects` (all table 1), plus `generation and
upscaling` (capability area, as of task 7).

Each item is written `<name> (<table>) — <priority>` because `multicam` appears in both tables.

1. timeline editing (capability area) — must
2. MCP and agent chat (capability area) — must
3. timeline (tool category) — should
4. texts (tool category) — should
5. captions (tool category) — should
6. transcription (tool category) — should
7. color (tool category) — should
8. multicam (tool category) — should
9. media (tool category) — should
10. project settings (tool category) — should
11. capture frame (tool category) — should
12. multicam (capability area) — should
13. transcription and captions (capability area) — should
14. text and graphics (capability area) — should
15. color and effects (capability area) — should
16. audio scrub and metering (capability area) — should
17. settings (capability area) — should
18. denoise (tool category) — later
19. organize (tool category) — later
20. layout (tool category) — later
21. search (tool category) — later
22. sync (tool category) — later
23. beats (tool category) — later
24. words (tool category) — later
25. project browser and search (capability area) — later
26. telemetry (capability area) — later
27. auto-update (capability area) — later

Counts, so a reader can check the projection without re-deriving it: 34 entries total — 7 `present`, 12 `partial`, 15 `absent`; 27 in this list, of which 2 `must`, 15 `should` and 10 `later`. Per table:
table 1 holds 22 entries (6 `present`, 6 `partial`, 10 `absent`, so 16 appear below) and table 2
holds 12 (1 `present`, 6 `partial`, 5 `absent`, so 11 appear below).

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
- **2026-08-20 re-check (`.kiro/specs/usable-editor` Phase 2, tasks 6–7).** The generation rows now reflect
  the complete reachable generative surface. `services::OpenSslGenerativeHttpTransport` provides the
  real HTTPS path verified against a local TLS server, while `GenerationModelCatalog` supplies
  provider-grouped models and capability flags to `generation.list_models` and `generation.generate`.
  The same generation tool accepts catalog-validated upscale requests and source-or-prompt audio
  requests with model-specific duration bounds. `generate` remains `present`, and `generation and
  upscaling` moves from `partial` to `present`: prompt video/image, catalog model selection, upscale
  and audio generation all complete through `GenerativeMediaCoordinator`, with generated assets
  registered and placed as one undoable edit. The acceptance checks for PRs 406, 396 and 395 are
  recorded `complete` in `docs/PORT_BACKLOG.md` and passed in CI run `32404256042`, whose explicit
  CTest result was 1275/1275 tests passed. No other parity row changed in this re-check.
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
  `generate` row (`services::GenerativeHttpTransport` still had no real implementation at that
  ref) or add a graphical (non-tree) timeline view, ripple editing, effect removal/reorder, or any
  of the `absent` rows below `timeline editing` in the build order; those remained open exactly as
  scored, at that ref.
- **2026-08-20 re-check (`.kiro/specs/usable-editor` Phase 2, task 6).** The premise the prior
  entry's `generate`/`generation and upscaling` rows rested on — no transport is implemented, so
  every backend fails at `submit` — is now false. `services::OpenSslGenerativeHttpTransport` is a
  real client-side TLS 1.2+ HTTPS implementation over OpenSSL: it verifies the server certificate
  by default (including hostname, via `SSL_set1_host`), applies a connect timeout and a separate
  I/O timeout, maps connection failure/timeout/certificate failure onto distinct `ErrorCode`s
  (`Io`/`Timeout`/`PermissionDenied`), and refuses a plaintext `http://` endpoint before sending a
  byte. `ApplicationComposition` installs it by default whenever OpenSSL is linked, falling back to
  the pre-existing unavailable transport only when it is not. It is verified against a real local
  HTTPS server (not the closed hosted service, per Requirement 11.6): `submit`, two non-terminal
  polls, a terminal poll and `fetchResult` all round-trip correctly over it in
  `tests/services/openssl_generative_transport_test.cpp`, including the credential arriving intact
  on every one of five separate TLS connections. `generate` (tool category) moves from `absent` to
  `present`: `generation.generate` is now reachable and completes. `generation and upscaling`
  (capability area) moves from `absent` to `partial`, not `present`, because that row names three
  further upstream operations task 6 does not touch — catalog-driven model choice (PR 406), upscale
  (PR 396) and audio generation (PR 395) — all still deferred to task 7. Nothing else changed: no
  other row's Linux-side facts moved at this ref.
