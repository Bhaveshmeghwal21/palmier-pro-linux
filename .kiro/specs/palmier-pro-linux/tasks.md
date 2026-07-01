# Implementation Plan: Palmier Pro for Linux (with GPU Support)

## Overview

This plan converts the design into an incremental, ground-up C++20 implementation using
Qt 6 (UI), FFmpeg (media/codecs), and Vulkan (GPU compositing + hardware codec bridge). The
repository is currently empty, so the plan begins with project scaffolding and build setup,
then builds the UI-agnostic domain core (Timeline Engine, command/undo-redo, data models,
persistence) before layering the Media Engine, GPU Abstraction Layer (the headline new
capability), services (MCP server, Auth, Generative AI, Agent chat), export, localization, the
Qt UI, and packaging. Each step builds on prior steps and ends by wiring components together.

Property-based tests use [RapidCheck](https://github.com/emil-e/rapidcheck) and cover
correctness properties **P1–P12** from the design. Each property test references its property
and requirement clause, runs a minimum of 100 iterations, and is tagged
**Feature: palmier-pro-linux, Property {n}: {text}**. GPU/CPU parity is validated with
golden-image comparison (P5) and a dedicated software-only fallback lane (P7).

## Tasks

- [x] 1. Set up project scaffolding and build system
  - [x] 1.1 Create CMake project structure and dependency discovery
    - Create top-level `CMakeLists.txt` targeting C++20, with subdirectory layout:
      `src/core`, `src/media`, `src/gpu`, `src/services`, `src/ui`, `src/app`, `tests`
    - Add `find_package`/`pkg-config` discovery for Qt 6, FFmpeg (libav*), Vulkan, shaderc,
      libva, LittleCMS (lcms2), libsecret; fail configuration with a clear message per missing dep
    - Define build options for enabling/disabling vendor HW codec paths (VAAPI/NVENC/QSV)
    - _Requirements: 1.1, 1.2_
  - [x] 1.2 Wire up test frameworks (GoogleTest + RapidCheck)
    - Integrate GoogleTest and RapidCheck via CMake; create a `tests` target with CTest
    - Add a placeholder property test demonstrating the required tag format and 100-iteration config
    - _Requirements: 1.6_
  - [x] 1.3 Implement core shared types and utilities
    - Define `Uuid`, `Duration`, `FrameRate`, `Resolution`, `ColorSpace`, `Result<T>`/error type,
      and `SchemaVersion` in `src/core`
    - _Requirements: 2.1, 3.5_

- [x] 2. Implement project data models and validation
  - [x] 2.1 Implement Project, Track, Clip, and Effect data structures
    - Implement `Project`, `Track`, `Clip`, `Effect`, `Transition`, `MediaAssetRef` per the design
    - Implement validation rules: `timelineFps > 0`, positive canvas, `sourceOut > sourcein`,
      `opacity ∈ [0,1]`, `gain ≥ 0`, every `Clip.assetRef` resolves, known schema version
    - _Requirements: 2.1, 3.5_
  - [x]* 2.2 Write unit tests for data model validation
    - Test each validation rule with valid and invalid inputs
    - _Requirements: 2.1, 3.5_

- [x] 3. Implement Timeline Engine and command/undo-redo system
  - [x] 3.1 Implement EditCommand interface, CommandResult, and undo/redo stack
    - Define abstract `EditCommand` (apply/revert), `CommandResult`, and the bounded undo stack
      supporting at least 20 sequential undo operations
    - _Requirements: 2.9, 2.10_
  - [x] 3.2 Implement TimelineEngine (apply/undo/redo, observers, invariant enforcement)
    - Implement `apply`, `undo`, `redo`, `snapshot`, `clip`, `duration`, and `observe`
    - Enforce timeline invariants (no negative durations; ordered, non-overlapping clips per track)
    - Emit granular `ChangeSet` events to subscribers; guarantee no partial mutation on failure
    - _Requirements: 2.9, 2.10, 6.6_
  - [x] 3.3 Implement editing commands (add/delete/move/trim/split/reorder)
    - Implement `AddClipCommand`, `DeleteClipCommand`, `MoveClipCommand`, `TrimClipCommand`,
      `SplitClipCommand`, `ReorderClipsCommand`, `AddEffectCommand`
    - Move rejects overlapping drops (retain original position + invalid indication); trim
      constrained to [1 frame, source duration]; split only within clip boundaries
    - _Requirements: 2.2, 2.3, 2.4, 2.5, 2.6, 2.7_
  - [x]* 3.4 Write property test for undo/redo round-trip
    - **Property 1: Undo/redo round-trip** — `apply(c)` then `undo()` restores exact prior state;
      `redo()` reproduces the post-apply state
    - **Validates: Requirements 2.9**
  - [x]* 3.5 Write property test for track ordering and non-overlap invariant
    - **Property 3: Track ordering and non-overlap invariant** — after any command sequence, every
      track's clips remain ordered by `timelineStart` and non-overlapping outside transitions
    - **Validates: Requirements 2.2, 2.3**
  - [x]* 3.6 Write property test for split contiguity and duration conservation
    - **Property 8: Split contiguity and duration conservation** — splitting at an interior playhead
      yields two contiguous non-overlapping clips whose combined duration and source range equal the original
    - **Validates: Requirements 2.5**
  - [x]* 3.7 Write property test for reorder preserving clip count
    - **Property 9: Reorder preserves clip count** — any reordering leaves the track's clip count unchanged
    - **Validates: Requirements 2.7**
  - [x]* 3.8 Write property test for trim adjusting duration to boundary
    - **Property 10: Trim adjusts duration to boundary** — after a valid trim the clip duration equals
      `sourceOut - sourceIn` for the new boundary
    - **Validates: Requirements 2.4**
  - [x]* 3.9 Write unit tests for clip math edge cases and undo-empty/split-miss handling
    - Test undo with empty history (no change + indication) and split when playhead misses all clips
    - _Requirements: 2.6, 2.10_

- [x] 4. Checkpoint - domain core
  - Ensure all tests pass, ask the user if questions arise.

- [x] 5. Implement project persistence (.palmier)
  - [x] 5.1 Implement .palmier serialization and deserialization
    - Serialize/deserialize complete project state (clips, tracks, edits, media references,
      clip positions, track order, selected clip version) to a single documented `.palmier` store
    - _Requirements: 3.5, 3.6_
  - [x]* 5.2 Write property test for project persistence round-trip
    - **Property 11: Project persistence round-trip** — serialize then deserialize yields an
      equivalent project (all clips, tracks, edits, and media references preserved)
    - **Validates: Requirements 3.5**
  - [x] 5.3 Implement save success/failure handling
    - Report successful save; on disk-space/permission/inaccessible-location failure, preserve the
      last successfully saved state and report the failure
    - _Requirements: 3.6, 3.7_

- [x] 6. Implement media library and generated-clip versioning
  - [x] 6.1 Implement Media Manager library and version retention
    - Add imported media to the library; retain prior versions when a generated clip replaces a clip,
      keeping at least the 10 most recent versions selectable
    - _Requirements: 3.1, 3.4_
  - [x]* 6.2 Write property test for generated-clip version retention
    - **Property 12: Generated-clip version retention** — a clip replaced by a generated clip keeps its
      prior version retained and selectable
    - **Validates: Requirements 3.4**

- [x] 7. Implement GPU Abstraction Layer (Vulkan) — headline capability
  - [x] 7.1 Implement Vulkan context, device enumeration, and selection policy
    - Implement `GpuContext::create`, physical-device enumeration, `selectGpu` policy
      (Auto/PreferVendor/ForceIndex/ForceSoftware), capability probing into `GpuCaps`, and
      persisted user GPU selection across restarts; complete detection within the time budget
    - Never throw for "no GPU": degrade to software fallback context
    - _Requirements: 10.1, 10.4, 10.6_
  - [x] 7.2 Implement zero-copy GPU FramePool
    - Implement pooled GPU-resident frames (DMA-BUF / CUDA-Vulkan interop) capped by VRAM
    - _Requirements: 10.2_
  - [x] 7.5 Implement HW decode/encode bridge and software fallback selection
    - Implement vendor bridges: NVDEC/NVENC (NVIDIA), VAAPI (AMD/Intel), Quick Sync (Intel), with
      FFmpeg software decode/encode (x264/x265/SVT-AV1) as fallback; route per detected capability
    - On GPU operation failure, retry once on CPU, preserve input/project state, log the failure
    - _Requirements: 10.2, 10.5_
  - [x]* 7.8 Write unit tests for capability probing with mocked device descriptors
    - Test scoring/selection (discrete > iGPU, hwEncode weighting) and no-device/force-software paths
    - _Requirements: 10.1, 10.4, 10.6_
  - [x] 7.3 Implement Compositor render graph (renderAt)
    - Implement painter's-order `renderAt`: gather visible video clips per position, sort by z,
      clear target, apply per-clip effects, alpha-composite by opacity; preserve the loop invariant
    - _Requirements: 10.2, 10.7_
  - [x] 7.4 Implement SPIR-V effect kernels and registration
    - Implement compute-shader effects (brightness/contrast, blur, crop/transform, color grade,
      transitions) compiled to SPIR-V and registered with the Compositor
    - _Requirements: 10.2, 10.7_
  - [x]* 7.6 Write property test for GPU/CPU parity (golden-image)
    - **Property 5: GPU/CPU parity** — for any source frame and effect parameters, GPU vs software
      compositing differ by no more than 1 per channel on a 0–255 scale (golden-image comparison)
    - **Validates: Requirements 10.7**
  - [x]* 7.7 Write property test for graceful degradation (software-only lane)
    - **Property 7: Graceful degradation** — with no supported GPU (or ForceSoftware), decode/composite/
      encode remain fully functional via the software path
    - **Validates: Requirements 10.4, 13.3**

- [x] 8. Implement Media Engine (FFmpeg)
  - [x] 8.1 Implement demux/probe and media import (MediaInfo)
    - Implement container probing and `MediaInfo` extraction (streams, codecs, resolution, fps);
      normalize heterogeneous inputs
    - _Requirements: 3.1_
  - [x] 8.2 Implement MediaDecoder (hardware-preferred, software fallback)
    - Implement `open`/`nextFrame`/`seek`; return GPU-resident frames via the FramePool when possible,
      otherwise CPU frames; fall back to SW decode per unsupported codec transparently
    - _Requirements: 3.1, 10.2, 10.5_
  - [x] 8.3 Implement MediaEncoder (hardware-preferred, software fallback)
    - Implement `create`/`submit`/`finish`; accept GPU or CPU frames; retry on encoder init failure
      with the software encoder
    - _Requirements: 10.2, 10.5_
  - [x] 8.4 Implement audio graph and resampler
    - Implement decode of audio streams and resampling/mixing via libswresample for playback/export
    - _Requirements: 2.8_
  - [x] 8.5 Implement import validation and error handling
    - Reject unsupported formats (name the format), and supported-but-undecodable files (indicate
      unreadable), leaving the library unchanged in both cases
    - _Requirements: 3.2, 3.3_
  - [x]* 8.6 Write unit tests for media import error paths
    - Test unsupported-format rejection, unreadable-file rejection, and library-unchanged guarantees
    - _Requirements: 3.2, 3.3_

- [x] 9. Checkpoint - media + GPU pipeline
  - Ensure all tests pass, ask the user if questions arise.

- [x] 10. Implement Export Engine
  - [x] 10.1 Implement export render loop and progress reporting
    - Render the full timeline via Compositor + MediaEncoder into a single output file at the selected
      format/resolution without modifying the source timeline; report monotonic 0–100% at least once/sec;
      prefer HW encode when a compatible backend is active
    - _Requirements: 11.1, 11.2, 10.3, 10.8_
  - [x] 10.2 Implement export validation and failure cleanup
    - Reject unsupported format/resolution and empty timelines before rendering; on mid-export failure
      remove the incomplete output, preserve the source timeline, and report the reason; notify on success
      with the output location
    - _Requirements: 11.3, 11.4, 11.5, 11.6_
  - [x]* 10.3 Write property test for export frame ordering
    - **Property 6: Export frame ordering** — rendered frames are emitted in strictly increasing
      presentation time
    - **Validates: Requirements 11.1**
  - [x]* 10.4 Write integration test for import -> edit -> export
    - End-to-end test on sample media per codec producing a valid output file
    - _Requirements: 11.1, 11.6_

- [x] 11. Implement Transcription Service
  - [x] 11.1 Implement transcription producing time-aligned segments
    - Produce ordered, non-overlapping segments (start < end, ms relative to clip start) associated
      with the source clip; empty transcript + indication when no audio; unchanged segments + indication
      on failure; complete within the time budget
    - _Requirements: 4.1, 4.2, 4.3, 4.4, 4.5, 4.6_
  - [x]* 11.2 Write unit tests for transcription ordering, empty-audio, and failure paths
    - Test non-decreasing start order/no overlap, empty transcript for no audio, and failure preserves segments
    - _Requirements: 4.2, 4.4, 4.5_

- [x] 12. Implement Key Moment Detection
  - [x] 12.1 Implement KeyMoment detector
    - Return 0–500 ms-precision timestamps within the time budget, each in `[0, clipDuration]`;
      return an error indication (no timestamps) when detection fails or the clip is empty/zero-duration
    - _Requirements: 5.1, 5.2, 5.5_
  - [x] 12.2 Integrate key-moment markers with the Timeline Engine model
    - Add marker data at each detected timestamp for display; when zero detected, add no markers and
      surface a "no key moments" indication
    - _Requirements: 5.3, 5.4_
  - [x]* 12.3 Write unit tests for key-moment bounds and error handling
    - Test timestamp bounds, empty-result handling, and empty/zero-duration clip errors
    - _Requirements: 5.2, 5.4, 5.5_

- [x] 13. Implement Authentication and Subscription client
  - [x] 13.1 Implement login, session, and entitlement handling
    - Establish authenticated session within budget and return entitlement (active/expired/none);
      reject invalid credentials with indication; lock the account for 15 minutes after 5 consecutive failures
    - _Requirements: 9.1, 9.2, 9.3, 9.4_
  - [x] 13.2 Implement BYOK credential validation and secure storage
    - Validate BYOK credentials with the provider; persist valid ones via libsecret and authorize
      subsequent requests; reject and discard invalid credentials with indication
    - _Requirements: 9.5, 9.6_
  - [x]* 13.3 Write unit tests for auth flows and lockout
    - Test valid/invalid login, lockout threshold, BYOK accept/reject, and entitlement gating
    - _Requirements: 9.3, 9.4, 9.6, 9.7_

- [x] 14. Implement Generative AI client
  - [x] 14.1 Implement GenerativeClient submit/poll/fetch with timeout and cancel
    - Implement `submit`/`poll`/`fetchResult` against the hosted backend over TLS using the auth token;
      cancel and error on the timeout budget; return provider failure reasons leaving state unchanged
    - _Requirements: 6.1, 6.3, 6.4, 6.6, 6.8_
  - [x] 14.2 Integrate generated media into library and timeline placement
    - On success, add generated media to the library and place it at the user-specified frame position;
      reject empty/over-length prompts and unauthenticated requests, leaving the timeline unchanged
    - _Requirements: 6.2, 6.5, 6.7, 9.7_
  - [x]* 14.3 Write property test for no-partial-edits atomicity
    - **Property 2: No partial edits (atomicity)** — any command, including a generative request that
      fails at the provider, either fully applies or leaves the project unchanged
    - **Validates: Requirements 6.6**
  - [x]* 14.4 Write unit tests for generation validation and auth gating
    - Test prompt-length bounds, timeout cancellation, and subscription/BYOK gating
    - _Requirements: 6.5, 6.7, 6.8_

- [x] 15. Implement MCP Server
  - [x] 15.1 Implement ToolRegistry and tool schemas mapped to EditCommands
    - Define the tool surface (read timeline; add/trim/split/move/delete clips; add transitions/effects;
      trigger generation; export) with JSON schemas; map handlers to the same `EditCommand` path as the UI;
      expose the identical tool set used by the Agent chat
    - _Requirements: 7.4, 7.8_
  - [x] 15.2 Implement HTTP endpoint binding and lifecycle
    - Serve MCP over HTTP at `http://127.0.0.1:19789/mcp` (loopback only); begin accepting connections
      on start and stop within the time budgets; on port-in-use report the conflict and refuse to start,
      leaving the project unchanged
    - _Requirements: 7.1, 7.2, 7.3, 7.9_
  - [x] 15.3 Implement tool execution with rollback, timeout, and error handling
    - Execute recognized tools on the current project and return within budget; unknown tool -> error +
      unchanged; execution failure or timeout -> roll back to pre-invocation state + error; no-project ->
      error indicating no project open; validate inputs against schemas before creating commands
    - _Requirements: 7.4, 7.5, 7.6, 7.7, 7.10_
  - [x]* 15.4 Write property test for UI/MCP/agent edit equivalence
    - **Property 4: UI / MCP / agent edit equivalence** — issuing any EditCommand via UI, MCP tool call,
      or in-app agent produces the same resulting project state
    - **Validates: Requirements 7.4, 8.1, 8.4**
  - [x]* 15.5 Write integration tests for MCP HTTP protocol conformance
    - Drive edits over HTTP and assert resulting project state, rollback on failure, and error responses
    - _Requirements: 7.4, 7.5, 7.6, 7.10_

- [x] 16. Implement In-App Agent Chat orchestrator
  - [x] 16.1 Implement agent orchestrator reusing MCP tool handlers
    - Maintain a chat session and translate intents into the same tool handlers as the MCP server;
      reflect edits in project state within budget; on failed edit, show error and leave state unchanged;
      gate on subscription/BYOK while preserving unsent message content
    - _Requirements: 8.1, 8.5, 8.6, 8.7_
  - [x] 16.2 Implement @-mention resolution
    - Resolve @ mentions to media items in the current project; reject unmatched mentions with an error
      (message not submitted); prompt for selection when multiple candidates match
    - _Requirements: 8.2, 8.3, 8.4_
  - [x]* 16.3 Write unit tests for mention resolution and auth gating
    - Test single-match resolve, no-match rejection, multi-match prompt, and unauthenticated gating
    - _Requirements: 8.2, 8.3, 8.4, 8.5_

- [x] 17. Implement Localization Manager
  - [x] 17.1 Implement language selection, live switching, fallback, and persistence
    - Expose the macOS edition's supported language set; switch visible text within budget without restart;
      fall back to English for untranslated strings; persist selection and reapply on launch; default to
      system language when supported else English
    - _Requirements: 12.1, 12.2, 12.3, 12.4, 12.5_
  - [x]* 17.2 Write unit tests for localization fallback and default selection
    - Test English fallback for missing translations and system-language/English default logic
    - _Requirements: 12.3, 12.5_

- [x] 18. Checkpoint - services layer
  - Ensure all tests pass, ask the user if questions arise.

- [x] 19. Implement Qt 6 UI and application shell
  - [x] 19.1 Implement application shell and platform compatibility checks
    - Create the Qt app entry point and main window; on launch verify architecture, glibc >= 2.31, and
      runtime dependencies; on unmet deps/unsupported platform show a message naming each missing item and
      exit without the editor; otherwise show the editor within the launch budget without a network connection
    - _Requirements: 1.1, 1.3, 1.4, 1.5, 13.3, 13.4_
  - [x] 19.2 Implement Timeline view (QML) bound to the engine model
    - Expose the project via `QAbstractItemModel`; render multi-track timeline (1–50 tracks) with
      drag-move, trim, split, reorder wired to editing commands; show invalid-drop and no-op indications
    - _Requirements: 2.1, 2.2, 2.3, 2.4, 2.5, 2.6, 2.7_
  - [x] 19.3 Implement Preview/Player view with playback
    - Render the composited preview at the playhead at >= 24 fps using the Compositor; wire GPU-active
      playback path with CPU fallback
    - _Requirements: 2.8, 10.7_
  - [x] 19.4 Implement Inspector and Effects panel
    - Expose clip properties and effect parameters bound to `AddEffectCommand`/edit commands
    - _Requirements: 2.4_
  - [x] 19.5 Implement Media Browser panel
    - Import UI wired to Media Manager; display library, clip versions, and key-moment markers
    - _Requirements: 3.1, 3.4, 5.3, 5.4_
  - [x] 19.6 Implement Agent Chat panel and GPU-unavailable/service-unavailable notifications
    - Chat UI wired to the agent orchestrator with @-mention affordances; non-blocking notification when
      GPU acceleration is unavailable; error when Generative_AI_Service is unreachable while editor stays functional
    - _Requirements: 8.1, 8.2, 10.4, 13.4_

- [x] 20. Packaging and licensing
  - [x] 20.1 Add GPLv3 license and source headers
    - Add the full unmodified GPLv3 license text and per-file headers for editor, MCP server, and agent chat
    - _Requirements: 13.1, 13.2_
  - [x] 20.2 Create distributable packaging (Flatpak, AppImage, .deb)
    - Add Flatpak manifest (primary), AppImage recipe, and `.deb` packaging with bundled runtime deps
    - _Requirements: 1.2_

- [x] 21. Final integration and wiring
  - [x] 21.1 Wire all components into the application composition root
    - Compose GpuContext, TimelineEngine, Media Engine, Project I/O, Auth, Generative client, MCP server,
      Agent orchestrator, Localization, and Qt UI in `src/app`; start the MCP server on launch and stop on close
    - _Requirements: 1.6, 7.1, 7.2, 7.9, 13.3_

- [x] 22. Final checkpoint - Ensure all tests pass
  - Ensure all tests pass, ask the user if questions arise.

## Notes

- Tasks marked with `*` are optional (tests) and can be skipped for a faster MVP.
- Each task references specific requirement clauses for traceability.
- Property tests use RapidCheck, run >= 100 iterations, and are tagged
  **Feature: palmier-pro-linux, Property {n}: {text}**.
- Correctness properties covered: P1 (3.4), P2 (14.3), P3 (3.5), P4 (15.4), P5 (7.6), P6 (10.3),
  P7 (7.7), P8 (3.6), P9 (3.7), P10 (3.8), P11 (5.2), P12 (6.2).
- P5 uses golden-image GPU-vs-software comparison; P7 exercises the dedicated software-only lane.
- The domain core is UI-agnostic so the MCP server and agent drive identical editing operations headlessly.

## Task Dependency Graph

```json
{
  "waves": [
    { "id": 0, "tasks": ["1.1"] },
    { "id": 1, "tasks": ["1.2", "1.3", "20.1"] },
    { "id": 2, "tasks": ["2.1", "7.1", "13.1"] },
    { "id": 3, "tasks": ["2.2", "3.1", "7.2", "7.5", "7.8", "8.1", "13.2"] },
    { "id": 4, "tasks": ["3.2", "7.3", "8.2", "8.3", "8.4", "8.5", "5.1", "6.1", "11.1", "13.3", "14.1", "17.1"] },
    { "id": 5, "tasks": ["3.3", "7.4", "5.2", "5.3", "6.2", "8.6", "11.2", "12.1", "14.2", "17.2"] },
    { "id": 6, "tasks": ["3.4", "3.5", "3.6", "3.7", "3.8", "3.9", "7.6", "7.7", "10.1", "12.2", "14.3", "14.4", "15.1"] },
    { "id": 7, "tasks": ["10.2", "10.3", "10.4", "12.3", "15.2", "15.3"] },
    { "id": 8, "tasks": ["15.4", "15.5", "16.1"] },
    { "id": 9, "tasks": ["16.2", "16.3", "19.1"] },
    { "id": 10, "tasks": ["19.2", "19.3", "19.4", "19.5", "19.6"] },
    { "id": 11, "tasks": ["20.2", "21.1"] }
  ]
}
```
