<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Upstream port backlog

This is the Port_Backlog of Requirement 14. Like `docs/UPSTREAM_PARITY.md` it is a **checked**
document: `tests/support/ReportParser` parses it and the Verification_Suite fails on any defect
(Requirement 14.11). Each entry is a flat block of `key: value` lines under an `### ` heading, so a
field the parser cannot find is a defect in this document.

## Provenance (Requirement 14.1)

- upstream-repository: https://github.com/palmier-io/palmier-pro
- upstream-range: unknown — the upstream repository is not reachable from the environment this
  backlog was written in, and no upstream snapshot or submodule exists in this tree, so no commit
  or tag range could be observed. The ten entries below are the ten upstream changes identified in
  Requirement 14.2, which is this backlog's only source for the upstream side. Whether ten is the
  complete set of changes merged in the window is therefore **unverified** — see "Known limits".
- window: 2026-06-25..2026-07-25

## Field rules

| Field | Values | Rule |
|---|---|---|
| `identifier` | e.g. `PR 405` | required; unique across the whole document |
| `summary` | 1–200 characters | required |
| `disposition` | `port` \| `adapt` \| `not-applicable` | required; exactly one |
| `linux-component` | comma-separated component names, or `none` | required |
| `rationale` | at least one sentence naming the affected Linux component, or the reason the change does not apply | required |
| `check:` | a block of `given:`, `when:`, `then:` | required if and only if `disposition` is `port` or `adapt`; **absent** for `not-applicable` (Requirement 14.3) |
| `status` | `not-started` \| `in-progress` \| `complete` | required; exactly one. A `not-applicable` entry has no work and no check, so it carries `not-started` permanently — see the note on PR 401 |
| `note` | free text | optional; carries what the fixed fields cannot |

`status` is what Requirement 14.12 governs: an entry whose acceptance check has not been run and
passed stays at `not-started` or `in-progress`, and its `disposition` and `rationale` are left
untouched while it does. `status: complete` is a claim that the `check:` block below it **has been
run and passed**, not that code exists.

Every `check:` is written so two reviewers reach the same verdict without reading source:
`given` names an observable starting state, `when` names one action, `then` names one observable
outcome.

---

## Implemented by this feature

### PR 403 — save failures must not block the user interface thread

identifier: PR 403
summary: Move project saving off the UI thread and make a failed save leave the file and the modified state untouched.
disposition: adapt
linux-component: services::ProjectSession::requestSave, services::ProjectSaveService, services::RawFileWriter
rationale: The upstream fix is a Swift concurrency change, so it is adapted rather than ported: on Linux `ProjectSession::requestSave` captures a `(Project snapshot, revision)` pair, hands it to a `std::jthread` running `ProjectSaveService::save` and returns immediately, and the revision guard decides on completion whether the dirty flag may be cleared. A `services::RawFileWriter` seam injects the three failure kinds so the preservation rule is testable without a real broken disk.
check:
  given: a project with unsaved modifications and a previously saved document on disk, with the injected RawFileWriter set to fail the next write
  when:  requestSave is called for that path and the reported completion is awaited
  then:  the call returns before the write completes, the completion reports failure naming the save, the file on disk is byte-identical to before, and the session still reports modified
status: complete
note: The check above is exercised by Property 18 in tests/services/project_save_failure_property_test.cpp and passes. Requirement 14.6's other half — that no window event is blocked for more than 100 ms during a slow save — is **not** verified yet: it needs the event-latency sampling of task 11.11, which is unbuilt, so this entry's `complete` covers the off-thread save and the preservation rule only.

### PR 405 — audio-decoder teardown moved off the concurrent worker pool

identifier: PR 405
summary: Release decoder resources on a dedicated teardown path instead of on the concurrent worker pool, so stop and seek never stall.
disposition: adapt
linux-component: media::DecoderTeardownQueue, media::DecoderClipFrameProvider, media::AudioEngine
rationale: Upstream moves teardown off its concurrent worker pool; the Linux adaptation is `media::DecoderTeardownQueue`, a single dedicated thread that takes ownership of retired `MediaDecoder` objects as `unique_ptr` so the caller returns immediately, with drain-to-empty observable. `DecoderClipFrameProvider` hands its LRU-evicted decoders to it, and the audio decoder reuses the same queue, which is why task 7.1 was ordered before the audio stage.
check:
  given: playback running on a timeline that references three distinct assets, so several decoders are live
  when:  100 successive stop-playback or seek operations are issued
  then:  each operation returns within 2 seconds, the full sequence completes with no deadlock, and the teardown queue is observed to drain to empty
status: complete
note: The check above is exercised by Property 75 in tests/media/decoder_teardown_property_test.cpp and passes.

### PR 408 — invert-colors effect with panel hierarchy clarification

identifier: PR 408
summary: Add a per-channel colour inversion effect, and clarify the inspector panel hierarchy that presents a clip's effects.
disposition: port
linux-component: core::EffectType::InvertColors, gpu::EffectKernels, gpu::Compositor, services::ToolRegistry timeline.add_effect
rationale: The effect itself ports directly, because the arithmetic is platform-independent: `EffectType::InvertColors` is realised by a software branch in `applyEffectSoftware` and a matching SPIR-V kernel in `gpu::EffectKernels`, wired through `gpu::Compositor`, exposed as the `invert_colors` value of `timeline.add_effect`, and persisted as `effects[].type = "invert_colors"` in schema 1.1. Red, green and blue become 255 minus the input; alpha is unchanged.
check:
  given: a project with one clip over a source frame of known per-channel values
  when:  timeline.add_effect is invoked with type invert_colors for that clip, and the clip is both played back and exported
  then:  every red, green and blue sample equals 255 minus its input, every alpha sample is unchanged, and playback and export agree on each channel to within 1 of 255
status: complete
note: The check above is exercised by Properties 73 and 74 in tests/gpu/invert_colors_property_test.cpp and passes. The panel-hierarchy half of the upstream change is adapted separately as Qt QDockWidget nesting with the inspector's effect list grouped per clip, and it lands with task 11.2, which is unbuilt; this entry's `complete` covers the effect, which is what Requirements 14.4 and 14.5 ask for.

### PR 404 — editor panel layout-sizing performance

identifier: PR 404
summary: Stop recomputing editor panel sizes per frame; fix the sizing contract so panels stay usable at every window size.
disposition: adapt
linux-component: ui::MainWindow
rationale: The upstream change is a SwiftUI sizing fix, which has no direct Linux counterpart, so it is adapted into a fixed minimum-size contract carried by Qt's layout engine: `setMinimumSize(1024, 640)` on `ui::MainWindow` and `setMinimumSize(80, 60)` on each of the five docks, with no per-frame recomputation anywhere. That contract is what makes Requirement 1.4's reachability claim decidable across the whole size range.
check:
  given: the assembled editor shell running under xvfb with all five docks visible
  when:  the window is resized across the full range from its minimum size upward
  then:  every one of the five panels remains present and reachable at every size, and the window refuses to shrink below 1024x640
status: not-started
note: Honest status. `ui::MainWindow` is still the placeholder from before this feature — a QLabel central widget and a File menu whose only action is Quit — with no docks and no setMinimumSize call anywhere in src/ui/MainWindow.cpp. Task 11.2 has not been implemented and Property 1 (tests/ui/shell_layout_property_test.cpp, task 11.8) has not been written, so the check above has never been run.

### PR 399 — dependency lockfile refresh

identifier: PR 399
summary: Refresh pinned dependency versions so a clean checkout configures and builds reproducibly.
disposition: adapt
linux-component: tests/CMakeLists.txt FetchContent pins, cmake/PalmierDependencies.cmake, docs/BUILD.md
rationale: Upstream refreshes a Swift package lockfile, which does not exist here, so the Linux equivalent is the pair this tree actually pins with: the exact `FetchContent` pins for the test dependencies (googletest v1.15.2 and rapidcheck b2d9ed2dddefc4b84318d664b4f221eb792d89c7 in tests/CMakeLists.txt) and the per-distribution native package list that docs/BUILD.md is to carry. The build-flag fix of stage 0 and the audio and TLS dependencies added by stages 6 and 8 both touch that package set.
check:
  given: a clean checkout on a supported distribution with only the native packages that docs/BUILD.md lists installed
  when:  the documented configure command is run and then the build and test commands
  then:  configuration succeeds and its summary reports every required dependency as found, the build completes, and the test suite runs with no dependency-related failure
status: in-progress
note: Honest status. The FetchContent pins exist and are exact, and the configuration summary already reports each dependency. `docs/BUILD.md` now exists — task 12.6 landed it, with the per-distribution native package table Requirement 16.1 asks for — so the "packages that docs/BUILD.md lists" half of the check is now runnable, but it has not been run: nothing has configured, built and tested a CLEAN checkout on a supported distribution with only that package set installed, which is the check's `given`. This entry moves to `complete` when that run happens and passes, which is the CI work of tasks 12.11 and 12.12.

---

## Backlog only — implementation deferred beyond this feature

Each entry below is authored now with its disposition, rationale and acceptance check, as
Requirement 14.2 asks, and left at `status: not-started` because none of the work has been done.

### PR 397 — multicam ripple-trim synchronisation

identifier: PR 397
summary: Keep grouped multicam angles synchronised when one of them is ripple-trimmed, so a trim propagates across the group.
disposition: port
linux-component: core::ClipGroup, core::EditCommands::RippleTrimCommand, services::ToolRegistry timeline.ripple_trim
rationale: The behaviour is domain logic with no platform content, so it ports rather than adapts. `core::ClipGroup` and the project-level `clipGroups` array were reserved by schema 1.1 in task 1.5; `RippleTrimCommand` is the first command that reads it. For the named clip's edge, it computes the source-time delta the trim represents, applies that identical delta to every other member of any `clipGroups` entry naming the clip (on that member's own track), and refuses the whole edit — leaving the project unchanged — if any member cannot absorb the delta within its own source range.
check:
  given: a project with two clips on different tracks that are members of one clipGroup, and a third ungrouped clip
  when:  timeline.ripple_trim extends the in-point of one grouped clip by a known duration
  then:  both grouped clips move by exactly that duration and stay aligned with each other, the ungrouped clip does not move, and the whole change undoes as one history entry
status: complete
note: The acceptance check is covered by RippleTrimCommand.KeepsGroupedMulticamAnglesSynchronised and RippleTrimCommand.AGroupedAngleThatCannotAbsorbTheTrimRefusesTheWholeEdit in tests/core/edit_commands_test.cpp.

### PR 406 — catalog-driven source-video preparation and provider-grouped model selection

identifier: PR 406
summary: Drive source-video preparation and model choice from a provider-grouped catalog instead of hard-coded model identifiers.
disposition: adapt
linux-component: services::GenerationModelCatalog, services::GenerativeBackendRegistry, services::ToolRegistry generation.list_models/generation.generate
rationale: It is adapted rather than ported because upstream's catalog is tied to its closed hosted service, while the Linux port expresses the same choice across hosted, BYOK and offline backends. The catalog is now the shared source of provider/model capabilities used by the tool surface.
check:
  given: a running editor with a generative backend configured and a catalog listing at least two providers with at least one model each
  when:  the catalog is requested through the tool surface and a model is selected from a named provider
  then:  the returned listing groups every model under its provider, and generation.generate accepts the selected model id and refuses an id absent from the catalog with an error naming the rejected id
status: complete
note: The acceptance check is covered by the GenerationModelCatalogTools tests in the generative lifecycle suite and passed in CI run 32404256042.

### PR 396 — catalog-driven upscale generation mode

identifier: PR 396
summary: Add an upscale generation mode selected from the model catalog, so an existing clip can be upscaled rather than generated from a prompt.
disposition: adapt
linux-component: services::ToolRegistry generation.generate, services::GenerationModelCatalog, services::GenerativeMediaCoordinator, core::MediaManager
rationale: Adapted because the Linux tool surface expresses the mode as a declared `ToolSchema` argument on `generation.generate` rather than as upstream's SwiftUI mode picker, so the same choice is reachable from the GUI, the MCP endpoint and the agent through one declaration. The catalog now identifies models that serve upscale, and the coordinator imports the resulting asset through the undoable placement path.
check:
  given: a project with one clip in the media library and a generative backend that serves an upscale-capable model
  when:  generation.generate is invoked for that clip with mode upscale and a target resolution larger than the source
  then:  the published tool schema lists mode with upscale among its permitted values, the request is accepted, and a successful job registers exactly one new asset at the requested resolution as one undoable edit
status: complete
note: The GenerationUpscaleProperties acceptance tests passed in CI run 32404256042, including one-undo completion and refusal of a model that does not serve upscale.

### PR 395 — source-or-prompt audio generation with duration ranges

identifier: PR 395
summary: Generate audio from either a source clip or a text prompt, with a permitted duration range declared per model.
disposition: adapt
linux-component: services::ToolRegistry generation.generate, services::GenerativeMediaCoordinator, core::MediaManager
rationale: Adapted because the Linux side registers generated audio into the media library and places it on an audio-bearing track through the same path as video, which is `GenerativeMediaCoordinator` plus `core::MediaManager`, and because duration bounds are declared `ToolSchema` constraints rather than upstream's UI control. The generation schema now admits audio, source-or-prompt requests and model-specific duration ranges.
check:
  given: a project with one audio track and a generative backend that serves an audio model with a declared duration range
  when:  generation.generate is invoked twice for audio, once with a source clip and once with a prompt only, each requesting a duration inside the declared range
  then:  both requests are accepted and each registers exactly one audio asset of the requested duration as one undoable edit, and a request for a duration outside the declared range is refused with an error naming the permitted range
status: complete
note: The GenerationAudioProperties acceptance tests passed in CI run 32404256042, covering source and prompt generation within model bounds and naming the permitted range for an out-of-range duration.

---

## Not applicable

### PR 401 — non-English README maintenance

identifier: PR 401
summary: Maintain the set of translated README files that accompany the upstream English README.
disposition: not-applicable
linux-component: none
rationale: The change has no Linux counterpart for a structural reason rather than a scheduling one: this repository ships a single English `README.md` plus the `docs/` set, and has no translated README files to maintain, so there is nothing for the change to act on. Interface localisation is a separate concern already served by `services::LocalizationManager`, which the change does not touch. Per Requirement 14.3 no acceptance check is recorded for a `not-applicable` entry, and per the note below the disposition is structural, not a deferral.
status: not-started
note: `not-started` here is a formality of the closed status value set, not a claim that work is pending. There is nothing to start: the disposition is the outcome, the entry carries no acceptance check, and Requirement 14.12 — which only downgrades an entry whose check has not passed — never applies to it. This entry will never move off `not-started`.

---

## Known limits of this backlog

- **Completeness of the window is unverified.** Requirement 14.1 asks for *every* upstream change
   merged between 2026-06-25 and 2026-07-25. The ten entries here are the ten Requirement 14.2
   identifies, and no upstream tree or commit range could be observed to confirm that no eleventh
   change exists in that window. `upstream-range` records `unknown` for that reason rather than
   inventing a range.
- **Each summary is a restatement of the identification in Requirement 14.2**, not of an upstream
   commit message, since no upstream commit was read. The dispositions, the Linux components and
   every acceptance check are, by contrast, grounded in this tree.
- **Three entries carry a status weaker than "the code exists".** PR 403 is `complete` for the
   off-thread save and the preservation rule, while Requirement 14.6's event-latency half awaits
   task 11.11. PR 404 is `not-started` because `ui::MainWindow` is still a placeholder. PR 399 is
   `in-progress` because its check has never been run on a clean checkout, even though
   `docs/BUILD.md` and its package table now exist. Those three notes are the honest
   reading of Requirement 14.12: a check that has not been run and passed does not make an entry
   complete, however much code is in place.
- **PR 408's two halves have different fates.** The effect is `complete`; the panel-hierarchy
   clarification is part of the shell work in task 11.2 and is not. The entry carries one
   disposition, as Requirement 14.10 requires, so the split is recorded in its `note`.
