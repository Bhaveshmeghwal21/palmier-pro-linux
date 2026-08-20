# Implementation Plan: End-to-End Editor Integration

## Overview

Implementation follows the **Migration and sequencing** table at the end of `design.md` exactly:
thirteen stages, 0 through 12, each of which leaves the tree buildable and CI green. Top-level
task numbers below **are** the design's stage numbers.

Three rules from the design govern the whole plan and are not restated in every task:

- **Tests land with their stage.** Each stage's property, unit and integration tests are part of
  that stage, never batched into a trailing task. A property whose subject does not exist yet is
  not written as a disabled test.
- **New dependencies enter optionally.** Every third-party addition arrives as
  `PALMIER_ENABLE_<X>` (default ON) with *optional* detection setting `PALMIER_HAVE_<X>`, and the
  code behind an absent guard degrades to a working fallback (`NullAudioSink`, loopback-only
  binding, software encode). No stage may make configuration fail on a host that configured
  successfully at the previous stage.
- **Schema 1.1 from stage 1 onward**, with every 1.1 field optional on read, so a document saved
  at any stage is readable at every later stage.

Language and tooling are fixed by the existing tree: **C++20**, CMake, GoogleTest for
example-based tests, **RapidCheck** (`RC_GTEST_PROP`) for property tests, registered through
`palmier_register_test()`. Every property test carries the tag comment
`// Feature: end-to-end-editor-integration, Property {n}: {property text}` and runs at least 100
generated cases.

---

## Tasks

### Stage 0 — Build-flag fix (no C++)

- [x] 0. Make vendor hardware-codec paths compile in CI
  - [x] 0.1 Make vendor SDK detection optional
    - In `cmake/PalmierDependencies.cmake`, change the `libva`, `vpl`/`libmfx` and `ffnvcodec`
      lookups so a miss records `PALMIER_VAAPI_AVAILABLE=OFF` / `PALMIER_QSV_AVAILABLE=OFF` /
      `PALMIER_NVENC_AVAILABLE=OFF` and emits a status message instead of appending to the fatal
      missing-dependency list
    - _Requirements: 8.1, 8.9_

  - [x] 0.2 Gate the `PALMIER_HAVE_*` definitions on `ENABLE AND FOUND`
    - In `src/media/CMakeLists.txt`, define `PALMIER_HAVE_VAAPI` / `PALMIER_HAVE_QSV` /
      `PALMIER_HAVE_NVENC` only when `PALMIER_ENABLE_*` is ON **and** the SDK was found, and link
      `PkgConfig::LIBVA` / `PkgConfig::LIBVPL` only inside that same branch
    - Follow the already-correct form in `src/gpu/CMakeLists.txt` as the model
    - This is the root cause of audit item 8: today the options cannot be left ON without the SDKs
    - _Requirements: 8.1, 8.9_

  - [x] 0.3 Report each vendor path in the configuration summary
    - In `cmake/PalmierSummary.cmake`, print each of VAAPI, NVENC, QSV as
      `enabled (SDK found)` / `disabled (SDK not found)` / `disabled (option OFF)`
    - _Requirements: 8.1, 8.9_

  - [x] 0.4 Configure the hardware paths ON in CI and add an SDK-free configure job
    - In `.github/workflows/ci.yml`, install `nv-codec-headers` and `libvpl-dev` alongside
      `libva-dev`, and configure with
      `-DPALMIER_ENABLE_VAAPI=ON -DPALMIER_ENABLE_NVENC=ON -DPALMIER_ENABLE_QSV=ON`
    - Assert after configure that the summary lists each vendor path as enabled with the SDK found
    - Add a second configure job with no vendor SDKs installed that asserts configuration succeeds
      and the summary lists each path as disabled
    - _Requirements: 8.1, 8.9_

  - [x] 0.5 Enforce the per-test time limit and property iteration floor in the test harness
    - In `tests/CMakeLists.txt`, give `palmier_register_test()` `PROPERTIES TIMEOUT 600` and keep
      the `RC_PARAMS=max_success=${PALMIER_PBT_MIN_SUCCESS}` environment with a floor of 100
    - _Requirements: 15.2, 15.8_

### Stage 1 — Core extensions (additive to `Palmier::core` / `Palmier::gpu`)

- [x] 1. Extend the domain core and GPU effect set
  - [x] 1.1 Add `ChangeOrigin::Reset` and `TimelineEngine::reset`
    - `src/core/ChangeSet.hpp`: new `ChangeOrigin::Reset`
    - `src/core/TimelineEngine.{hpp,cpp}`: `[[nodiscard]] CommandResult reset(Project initial)`
      swaps the project value in place, clears the undo and redo stacks, and emits a `ChangeSet`
      with `origin = ChangeOrigin::Reset` so the executor's applied-command counter never counts it
    - Extend `tests/core/timeline_engine_test.cpp` with reset, observer-notification and
      history-clearing cases
    - _Requirements: 3.4, 4.3_

  - [x] 1.2 Add track commands and promote `SetTransitionCommand` into the core
    - `src/core/EditCommands.{hpp,cpp}`: `AddTrackCommand` (appends after the last track of its
      kind, rejects beyond 64 per kind), `RemoveTrackCommand` (removes the track and its clips,
      preserving the relative order of the rest)
    - Move `SetTransitionCommand` out of `src/services/ToolRegistry.cpp` into
      `src/core/EditCommands` as a real `EditCommand` (audit finding)
    - Extend `tests/core/edit_commands_test.cpp`
    - _Requirements: 3.3, 3.8, 3.10_

  - [x] 1.3 Add the invert-colors effect (upstream PR 408, ported here)
    - `src/core/Effect.hpp`: `EffectType::InvertColors`
    - `src/gpu/EffectKernels.{hpp,cpp}`: software reference branch in `applyEffectSoftware` plus the
      matching GLSL/SPIR-V kernel, wired through `gpu/Compositor`
    - Per-channel rule: R, G, B become 255 minus input; alpha is unchanged
    - _Requirements: 14.4_

  - [x]* 1.4 Write property tests for the invert-colors effect
    - **Property 73: Invert-colors channel arithmetic** — **Validates: Requirements 14.4**
    - **Property 74: Invert-colors agrees between playback and export** —
      **Validates: Requirements 14.5**
    - File: `tests/gpu/invert_colors_property_test.cpp`; new target `palmier_gpu_invert_colors_tests`
    - _Requirements: 14.4, 14.5_

  - [x] 1.5 Move the document schema to 1.1 with optional reads
    - `src/core/SchemaVersion.{hpp,cpp}`: version 1.1
    - `src/services/ProjectStore.cpp`: read/write `effects[].type = "invert_colors"`,
      `tracks[].name` (default `""`) and `clipGroups` (default `[]`, reserved for upstream PR 397);
      every 1.1 field optional on read so 1.0 documents round-trip unchanged
    - Extend `tests/services/project_store_test.cpp` with a 1.0-document read and a
      future-major-version rejection
    - _Requirements: 3.9, 4.7, 4.10, 14.4_

### Stage 2 — Project_Session (introduce only, no existing caller changes)

- [x] 2. Introduce the session abstraction
  - [x] 2.1 Implement `services::ProjectSession`
    - `src/services/ProjectSession.{hpp,cpp}`: owns exactly one `TimelineEngine` and the
      `MediaManager` media library for its lifetime; `Status`, `engine()`, `mediaLibrary()`,
      `status()`, `modified()`, `revision()`, `documentPath()`, `createProject()`, `openProject()`,
      `markModified()`, `observeStatus()`
    - Default construction yields an empty project at the documented default frame rate, canvas
      resolution and colour space
    - `createProject`/`openProject` build a complete `Project` value locally and commit with
      `TimelineEngine::reset` only on full success; they are not `EditCommand`s and are not undoable
    - _Requirements: 1.1, 1.10, 3.2, 3.4, 3.5, 3.8, 3.9, 4.6, 4.10_

  - [x] 2.2 Move saving off the UI thread with a revision guard
    - Add the `services::RawFileWriter` injection seam used by the save-failure tests
    - `ProjectSession::requestSave(path, SaveCompletion)` captures `(Project snapshot, revision r)`,
      hands them to a `std::jthread` calling `ProjectSaveService::save`, and returns immediately;
      completion is marshalled back — on success clear the dirty flag and record the path only if
      the revision is still `r`, otherwise record the path and stay modified; on failure leave the
      in-memory project, its modified state and any previously saved file untouched
    - This is the Linux adaptation of upstream PR 403
    - _Requirements: 4.1, 4.4, 4.6, 14.6, 14.7_

  - [x]* 2.3 Write persistence property tests for the session
    - **Property 16: Save/open round-trip preserves the project** — **Validates: Requirements 4.7**
    - **Property 17: Saving a loaded project is idempotent** — **Validates: Requirements 4.8**
    - **Property 19: Unmodified until the next tool-applied edit** — **Validates: Requirements 4.6**
    - File: `tests/services/project_session_roundtrip_property_test.cpp`; new target
      `palmier_services_project_session_tests`, which also carries the `ProjectSession` and
      `TimelineEngine::reset` unit tests
    - _Requirements: 4.6, 4.7, 4.8_

  - [x]* 2.4 Write the save-failure property test
    - **Property 18: A failed save preserves the file and the modified state** —
      **Validates: Requirements 4.4, 14.7**
    - File: `tests/services/project_save_failure_property_test.cpp`, injecting the three failure
      kinds through `services::RawFileWriter`
    - _Requirements: 4.4, 14.7_

### Stage 3 — ToolSchema, then the session switch (the only atomic multi-file step)

- [x] 3. Unify argument specification, then retarget the tool surface at the session
  - [x] 3.1 Implement `services::ToolSchema`
    - `src/services/ToolSchema.{hpp,cpp}`: `ArgSpec` (name, `JsonKind`, required, description,
      `minInt`/`maxInt`, `minNum`/`maxNum`, `minLength`/`maxLength`, `enumValues`, `uuid`),
      `ToolSchema::arg()`, `toJsonSchema()` rendering the draft-07 object schema, and `validate()`
      enforcing exactly the same constraint set
    - Unit tests in `tests/services/tool_schema_test.cpp`; new target
      `palmier_services_tool_schema_tests`
    - _Requirements: 9.3, 9.9_

  - [x] 3.2 Convert each existing tool to `ToolSchema`, one tool at a time
    - `src/services/ToolRegistry.{hpp,cpp}`: `Tool` gains `ToolSchema schema`, and `inputSchema`
      becomes `schema.toJsonSchema()`
    - For each of `timeline.read`, `add_clip`, `delete_clip`, `move_clip`, `trim_clip`,
      `split_clip`, `reorder_clips`, `add_effect`, `add_transition`, `generation.generate`,
      `timeline.export`: declare its `ArgSpec` list and assert the rendered schema is **byte-equal
      to today's hand-written `inputSchema`** before moving to the next tool
    - Lift every acceptance rule a handler enforced privately (for example
      `sourceOutNs > sourceInNs`) into the `ArgSpec` vocabulary, and add the `invert_colors` value
      to the `add_effect` type enum
    - _Requirements: 3.1, 9.3, 9.9, 9.12, 14.4_

  - [x]* 3.3 Write the schema/handler conformance property test
    - **Property 50: The advertised schema and the handler agree** —
      **Validates: Requirements 9.12**
    - File: `tests/services/tool_schema_conformance_property_test.cpp`
    - _Requirements: 9.12_

  - [x] 3.4 Switch the tool surface from `TimelineEngine&` to `ProjectSession&` in one commit
    - `buildDefaultToolRegistry(ProjectSession&, ToolRegistryHooks)` replaces the
      `TimelineEngine&` form; every handler resolves the engine at invocation time
    - `src/services/McpToolExecutor.{hpp,cpp}`: `TimelineEngine*` → `ProjectSession*`,
      `validateAgainstSchema` delegates to `ToolSchema::validate`, default budget becomes 60 s, and
      an `InvocationSource { Gui, Mcp, Agent }` argument is added for logging only
    - `src/app/ApplicationComposition.{hpp,cpp}`: construct the single `ProjectSession` and pass it
      through; add the `projectSession()` accessor
    - Purely mechanical `engine` → `session.engine()` substitution — no behavioural change; update
      `tests/services/mcp_tool_executor_test.cpp` and `tests/app/application_composition_test.cpp`
      in the same commit
    - _Requirements: 1.1, 1.7, 3.5, 9.4, 9.16, 11.5_

### Stage 4 — New session and media tools

- [x] 4. Make the headless surface able to build a project
  - [x] 4.1 Implement `services::MediaImportService`
    - `src/services/MediaImportService.{hpp,cpp}`: `ImportedAsset`, `import()`, `isPending()`;
      probe → validate → register exactly one asset; duplicate detection via
      `std::filesystem::weakly_canonical` against the library; 30 s probe/validation timeout;
      errors name the path and classify empty / missing / unreadable / undecodable; rejected
      containers and codecs named in the error
    - _Requirements: 2.1, 2.2, 2.3, 2.4, 2.5, 2.7, 2.8, 2.9_

  - [x]* 4.2 Write the media-import property tests
    - **Property 4: Import result completeness and optional-field rule** —
      **Validates: Requirements 2.2**
    - **Property 5: Rejected imports name the format and leave the library unchanged** —
      **Validates: Requirements 2.3**
    - **Property 78: A rejected import classifies its failure and changes nothing** —
      **Validates: Requirements 2.4**
    - **Property 6: Import is idempotent over path spellings** — **Validates: Requirements 2.5**
    - **Property 7: Media library entry count invariant** — **Validates: Requirements 2.6**
    - File: `tests/services/media_import_property_test.cpp`; new target
      `palmier_services_media_import_tests`
    - _Requirements: 2.2, 2.3, 2.4, 2.5, 2.6_

  - [x] 4.3 Add the `project.*` tools
    - `project.create`, `project.open`, `project.save`, `project.info` in
      `src/services/ToolRegistry.cpp` with the `ToolSchema` argument sets and success-result fields
      given in the design's tool table; `ToolRegistryHooks` gains `createProject`, `openProject`,
      `saveProject`, `projectInfo`
    - Every tool other than `project.create`/`project.open` returns "no project is open" when no
      project is current
    - _Requirements: 3.1, 3.2, 3.4, 3.5, 3.8, 3.9_

  - [x] 4.4 Add the `media.*` tools
    - `media.import` and `media.list` wired to `MediaImportService` through the new
      `importMedia` / `listMedia` hooks, returning the fields listed in the design's tool table
    - _Requirements: 2.2, 3.1_

  - [x] 4.5 Add the track tools
    - `timeline.add_track` and `timeline.remove_track` backed by the stage-1 `AddTrackCommand` /
      `RemoveTrackCommand`, so both are undoable through the same path as every other edit
    - _Requirements: 3.1, 3.3, 3.8, 3.10_

  - [x]* 4.6 Write the project-tool property tests
    - **Property 8: project.create carries exactly the requested settings** —
      **Validates: Requirements 3.2**
    - **Property 10: project.open reports the loaded project accurately** —
      **Validates: Requirements 3.4**
    - **Property 11: No project open blocks every other tool** — **Validates: Requirements 3.5**
    - **Property 12: Track and clip counts equal successful call counts** —
      **Validates: Requirements 3.7**
    - **Property 13: Out-of-range arguments are rejected and named** —
      **Validates: Requirements 3.8**
    - **Property 14: A failed open preserves the previous session exactly** —
      **Validates: Requirements 3.9, 4.10**
    - File: `tests/services/project_tools_property_test.cpp`; new target
      `palmier_services_project_tools_tests`
    - _Requirements: 3.2, 3.4, 3.5, 3.7, 3.8, 3.9, 4.10_

  - [x]* 4.7 Write the track-tool property tests
    - **Property 9: add_track appends after the last track of its kind** —
      **Validates: Requirements 3.3**
    - **Property 15: remove_track preserves the order of remaining tracks** —
      **Validates: Requirements 3.10**
    - File: `tests/services/timeline_track_tools_property_test.cpp`
    - _Requirements: 3.3, 3.10_

  - [x] 4.8 Checkpoint
    - Ensure all tests pass, ask the user if questions arise. The headless sequence of Requirement
      3.6 is now runnable except for export.

### Stage 5 — MCP protocol layer

- [x] 5. Replace the bespoke envelope with JSON-RPC 2.0
  - [x] 5.1 Implement `services::McpSessionRegistry`
    - `src/services/McpSessionRegistry.{hpp,cpp}`: `McpSessionRecord`, `create()` minting a 256-bit
      value from `std::random_device` as 64 lowercase hex characters, `touch()` returning
      `NotFound` / expired, `markInitialized()`, `expireIdle()`, `activeCount()`; options for
      max sessions 1–32 (default 8) and idle timeout 30–3600 s (default 300); issued ids retained
      in a never-pruned set so uniqueness holds for the process lifetime
    - _Requirements: 9.10, 9.11, 9.14, 9.15, 10.9, 10.11_

  - [x] 5.2 Implement `services::McpProtocolHandler`
    - `src/services/McpProtocolHandler.{hpp,cpp}`: `McpRequestContext`, `McpReply`,
      `kSupportedProtocolVersions`, `handle()`; dispatch order parse (−32700) → envelope (−32600)
      → method (−32601) → session state → `ToolSchema::validate` (−32602) → execute
    - `initialize` negotiates the requested version when supported and otherwise the highest
      supported, returning server name, version and a `tools` capability;
      `notifications/initialized` answers 202 with a zero-byte body; `tools/list` returns one entry
      per registered tool; `tools/call` returns a `content` array whose first entry is
      `"type":"text"` with `isError` false, or `isError` true naming the failing tool and reason
    - `MainThreadInvoker` seam with the 60 s budget; tests and headless builds supply the inline
      invoker
    - _Requirements: 9.1, 9.2, 9.3, 9.4, 9.5, 9.6, 9.7, 9.8, 9.9, 9.10, 9.14, 9.15, 9.16_

  - [x] 5.3 Extend the `McpServer` transport and update its contract test in the same commit
    - `src/services/McpServer.{hpp,cpp}`: `start(const BindDecision&)`, header capture into
      `McpRequestContext`, `Mcp-Session-Id` emission, 202-with-empty-body support, a 1 MiB body cap
      yielding −32700, and delegation to `McpProtocolHandler`; `dispatch()` keeps its pure,
      socket-free shape so the existing transport unit tests remain valid
    - Extend `tests/services/mcp_http_integration_test.cpp` to drive real loopback HTTP POSTs
      through `initialize` → `notifications/initialized` → `tools/list` → `tools/call`, asserting
      the JSON-RPC envelope on every response and `name`/`description`/`inputSchema` on every
      `tools/list` entry
    - _Requirements: 9.1, 9.6, 9.10, 9.11, 10.1, 15.3_

  - [x]* 5.4 Write the JSON-RPC protocol property tests
    - **Property 43: JSON-RPC envelope round-trip** — **Validates: Requirements 9.1, 9.13**
    - **Property 44: initialize negotiates a supported protocol version** —
      **Validates: Requirements 9.2**
    - **Property 45: tools/list describes every registered tool** — **Validates: Requirements 9.3**
    - **Property 46: tools/call success shape** — **Validates: Requirements 9.4**
    - **Property 47: A failing tool leaves the project and history untouched** —
      **Validates: Requirements 9.5**
    - **Property 48: Protocol faults map to their assigned codes and create no edit command** —
      **Validates: Requirements 9.6, 9.7, 9.8, 9.9**
    - File: `tests/services/mcp_protocol_property_test.cpp`; new target
      `palmier_services_mcp_protocol_tests`
    - _Requirements: 9.1, 9.2, 9.3, 9.4, 9.5, 9.6, 9.7, 9.8, 9.9, 9.13_

  - [x]* 5.5 Write the session-identity property tests
    - **Property 49: Session identifiers are opaque and unique for the process lifetime** —
      **Validates: Requirements 9.11**
    - **Property 51: Session-state violations are rejected without touching the project** —
      **Validates: Requirements 9.14, 9.15**
    - File: `tests/services/mcp_session_property_test.cpp`; new target
      `palmier_services_mcp_session_tests`
    - _Requirements: 9.11, 9.14, 9.15_

### Stage 6 — Remote access (defaults to loopback, so CI is green before `libssl-dev` lands)

- [x] 6. Add opt-in authenticated remote MCP access
  - [x] 6.1 Implement `app::AppSettings` and extend `AppConfig`
    - `src/app/AppSettings.{hpp,cpp}`: precedence built-in defaults → `key=value` file at
      `$XDG_CONFIG_HOME/palmier-pro/config` → environment variables → command-line flags, producing
      an `AppConfig`
    - `AppConfig` gains `RemoteAccessConfig remote` and `MainThreadInvoker mainThreadInvoker`
    - _Requirements: 10.2, 16.3_

  - [x] 6.2 Implement `services::RemoteAccessGate` and `RejectionLog`
    - `src/services/RemoteAccessGate.{hpp,cpp}`: `RemoteAccessConfig`, `BindDecision`,
      `RejectionReason`, `Admission`, `validate()`, `admit()`, `noteSessionCreated/Closed()`
    - Bind-time: absent configuration yields `127.0.0.1:19789`; enabling requires a valid
      IPv4/IPv6 literal, a 32–512 printable-ASCII bearer token and `acknowledged == true`; any
      unmet prerequisite still binds loopback and carries the named unmet prerequisites, never the
      token; enabled without TLS emits exactly one warning before the first accepted request and
      still performs the non-loopback bind
    - Per-request: loopback-only returns `Allow` unconditionally; non-loopback checks source-block
      list, then constant-time bearer comparison (401), then `Origin` against the allow-list
      extended with the bound address and loopback hosts (403), then the session-count limit
    - `RejectionLog` records a UTC millisecond timestamp, the source address and a reason **code**
      only — the presented credential is never passed to the logger; five 401s from one source
      inside a 60 s window install a 60 s block for that source alone
    - _Requirements: 10.1, 10.2, 10.3, 10.4, 10.5, 10.7, 10.8, 10.9, 10.10, 10.12, 10.13_

  - [x] 6.3 Add the optional TLS transport and the non-loopback bind
    - `src/services/TlsTransport.{hpp,cpp}` on OpenSSL 3.x behind `PALMIER_ENABLE_OPENSSL`
      (default ON) with optional detection setting `PALMIER_HAVE_OPENSSL`; when the guard is absent,
      configuring TLS is an unmet prerequisite and the gate falls back to loopback
    - Wire the gate into `McpServer` strictly upstream of `McpProtocolHandler`; a plaintext request
      on a TLS port fails the handshake and is closed and logged without producing an `HttpRequest`
    - Install `libssl-dev` in `.github/workflows/ci.yml`
    - Construct the gate in `ApplicationComposition` with a `remoteAccessGate()` accessor
    - _Requirements: 10.6, 10.7, 10.12_

  - [x]* 6.4 Write the admission-gate property tests
    - **Property 52: A non-loopback bind requires all prerequisites, and failure falls back to
      loopback without leaking the token** — **Validates: Requirements 10.2, 10.3, 10.12**
    - **Property 53: Non-loopback admission is exactly the conjunction of its checks** —
      **Validates: Requirements 10.4, 10.5**
    - **Property 54: Every rejection is logged completely and without the credential** —
      **Validates: Requirements 10.8**
    - **Property 56: Loopback-only configurations preserve the current developer experience** —
      **Validates: Requirements 10.10**
    - **Property 58: The 401 rate limiter blocks the offending source only** —
      **Validates: Requirements 10.13**
    - File: `tests/services/remote_access_gate_property_test.cpp`; new target
      `palmier_services_remote_access_tests`
    - _Requirements: 10.2, 10.3, 10.4, 10.5, 10.8, 10.10, 10.12, 10.13_

  - [x]* 6.5 Write the session-limit and idle-expiry property tests
    - **Property 55: The session limit rejects only the excess request** —
      **Validates: Requirements 10.9**
    - **Property 57: Idle sessions expire exactly at their configured timeout** —
      **Validates: Requirements 10.11**
    - File: `tests/services/mcp_session_property_test.cpp` (extends the stage-5 file), driven by an
      injected clock
    - _Requirements: 10.9, 10.11_

  - [x]* 6.6 Write the remote-access and TLS integration tests
    - `tests/services/remote_access_http_integration_test.cpp`: bind a non-loopback-configured
      endpoint on a test-local address and issue three requests — no bearer token, a wrong token,
      the configured token — asserting 401, 401, 200 and a byte-identical project after each
      rejection
    - One HTTPS-succeeds / plaintext-rejected test, skipped with a recorded reason when
      `PALMIER_HAVE_OPENSSL` is absent
    - _Requirements: 10.6, 15.4_

### Stage 7 — Playback

- [x] 7. Assemble the decode → composite → present pipeline
  - [x] 7.1 Implement `media::DecoderTeardownQueue`
    - `src/media/DecoderTeardownQueue.{hpp,cpp}`: single dedicated thread; `MediaDecoder` objects
      are moved in as `unique_ptr` and the caller returns immediately; drain-to-empty is observable
    - This is the Linux adaptation of upstream PR 405 and **must precede stage 8** so the audio
      decoder reuses it
    - _Requirements: 14.8_

  - [x]* 7.2 Write the decoder-teardown property test
    - **Property 75: Decoder teardown never deadlocks or stalls** —
      **Validates: Requirements 14.8**
    - File: `tests/media/decoder_teardown_property_test.cpp`; new target
      `palmier_media_decoder_teardown_tests`
    - _Requirements: 14.8_

  - [x] 7.3 Implement `DecodeWorkerPool` and `media::DecoderClipFrameProvider`
    - `src/media/DecodeWorkerPool.{hpp,cpp}`: N = 2 workers, one `MediaDecoder` per active asset,
      decoded frames pushed to a bounded lock-protected per-clip queue
    - `src/media/DecoderClipFrameProvider.{hpp,cpp}`: implements `gpu::ClipFrameProvider`, maps
      `(Clip, timelinePosition)` to `sourceIn + (position - timelineStart)`, keeps decoders in an
      LRU cache, seeks when the request is not the next sequential frame, converts `DecodedFrame`
      to `gpu::SourceFrame`, and hands retired decoders to `DecoderTeardownQueue`; a decode failure
      is returned as an error so `Compositor::renderAt` emits no partial frame
    - _Requirements: 5.1, 5.5, 14.8_

  - [x]* 7.4 Write the frame-fidelity property test
    - **Property 20: Presented frames match the decoded source frames** —
      **Validates: Requirements 5.1**
    - File: `tests/media/playback_frame_fidelity_property_test.cpp`; new target
      `palmier_media_playback_tests`
    - _Requirements: 5.1_

  - [x] 7.5 Construct the playback engine in the composition root and complete the transport
    - `ApplicationComposition` constructs the single `gpu::Compositor`, the provider and the
      `ui::PreviewController`, exposing `playbackEngine()` and `compositor()`
    - `src/ui/PreviewController.{hpp,cpp}`: play, pause, stop, seek-with-clamp, end-of-timeline
      halt, playhead indicator updates at ≥10 Hz within 100 ms of the presented frame, drop
      accounting, decode-failure pause naming the asset, and runtime fallback to software
      compositing with a session-lifetime accessor and a status-bar notice
    - _Requirements: 1.1, 5.2, 5.3, 5.4, 5.5, 5.6, 5.7, 5.8, 5.9, 5.10_

  - [x]* 7.6 Write the playback transport and pacing property tests
    - **Property 21: Presentation rate stays within bounds and drops stay under 5%** —
      **Validates: Requirements 5.2**
    - **Property 22: Playhead indicator cadence** — **Validates: Requirements 5.3**
    - **Property 23: A decode failure pauses and retains the last good frame** —
      **Validates: Requirements 5.5**
    - **Property 24: Playback frame accounting matches the export planner** —
      **Validates: Requirements 5.7**
    - **Property 79: Each transport command reaches its specified resting state** —
      **Validates: Requirements 5.4, 5.8, 5.10**
    - **Property 25: Seek clamps to the timeline bounds** — **Validates: Requirements 5.9**
    - File: `tests/ui/preview_playback_property_test.cpp`; new target
      `palmier_ui_preview_playback_tests`, driven by an injected `PlaybackClock`
    - _Requirements: 5.2, 5.3, 5.4, 5.5, 5.7, 5.8, 5.9, 5.10_

### Stage 8 — Audio (`NullAudioSink` first, real sinks additive behind optional detection)

- [x] 8. Build the audio decode → mix → output pipeline
  - [x] 8.1 Add the audio surface to `MediaDecoder`
    - `src/media/MediaDecoder.{hpp,cpp}`: `AudioFrame`, `openAudioStream(int)`, `nextAudioFrame()`,
      `seekAudio(Duration)`, `hasAudio()`; `IDecodeBackend` gains `decodeAudio()` and
      `seekAudio()`, with the FFmpeg backend converting through `libswresample` into the
      interleaved-float `AudioBuffer` that `AudioGraph` already consumes
    - Buffers declare 8 000–192 000 Hz, 1–8 channels, and non-decreasing presentation timestamps
      per stream; audio decode is always software, so `CodecBridge` is not involved
    - _Requirements: 6.1_

  - [x]* 8.2 Write the audio-decode property test
    - **Property 26: Decoded audio buffers conform to the declared ranges** —
      **Validates: Requirements 6.1**
    - File: `tests/media/audio_decode_property_test.cpp`
    - _Requirements: 6.1_

  - [x] 8.3 Add `media::IAudioSink` and the always-compiled `NullAudioSink`
    - `src/media/AudioSink.{hpp,cpp}`: the sink interface plus `NullAudioSink`, which consumes and
      discards buffers while advancing a monotonic sample position from a steady clock
    - This makes "audio output unavailable" a normal code path and lets the audio tests run in CI
      with no sound card — it lands **before** the real sinks
    - _Requirements: 6.7_

  - [x] 8.4 Implement `media::AudioEngine`
    - `src/media/AudioEngine.{hpp,cpp}`: fixed output format 48 000 Hz / 2 interleaved channels /
      `SampleFormat::F32`; `start()`, `stop()`, `presentationPosition()` (the master clock),
      `outputAvailable()`, `notice()`, `renderRange()` for the export path
    - Mixes every clip on an unmuted audio-bearing track at its clip gain through `AudioGraph`,
      resampling per source; an audio-less asset contributes exactly silence with no error; an
      audio decode failure yields silence for the rest of that clip and an error naming the asset;
      mute and gain changes take effect within 200 ms without moving the playhead; unavailable
      output suppresses audio, keeps video running and raises a notice within 2 s
    - _Requirements: 6.2, 6.3, 6.4, 6.6, 6.7, 6.9_

  - [x]* 8.5 Write the audio-engine property tests
    - **Property 27: Mixing honours mute and gain and delivers without dropout** —
      **Validates: Requirements 6.2**
    - **Property 28: Audio and video stay within 40 milliseconds** —
      **Validates: Requirements 6.3**
    - **Property 29: Mute and gain changes take effect within 200 milliseconds** —
      **Validates: Requirements 6.4**
    - **Property 30: An asset without audio contributes exactly silence** —
      **Validates: Requirements 6.6**
    - **Property 31: Export mix duration and sample range invariant** —
      **Validates: Requirements 6.8, 6.11**
    - **Property 32: An audio decode failure yields silence for the rest of the clip** —
      **Validates: Requirements 6.9**
    - File: `tests/media/audio_engine_property_test.cpp`; new target
      `palmier_media_audio_engine_tests` using `IAudioSink` fakes
    - _Requirements: 6.2, 6.3, 6.4, 6.6, 6.8, 6.9, 6.11_

  - [x] 8.6 Add the PipeWire and ALSA sinks behind optional detection
    - PipeWire sink (`libpipewire-0.3`, MIT) behind `PALMIER_ENABLE_PIPEWIRE` /
      `PALMIER_HAVE_PIPEWIRE`; ALSA sink (`libasound2`, LGPL-2.1-or-later) behind
      `PALMIER_ENABLE_ALSA` / `PALMIER_HAVE_ALSA`; startup selection order PipeWire → ALSA → Null
    - Request a quantum of at most 512 frames when the project frame rate exceeds 48 fps
    - Install `libpipewire-0.3-dev` and `libasound2-dev` in `.github/workflows/ci.yml`
    - _Requirements: 6.2, 6.7_

  - [x] 8.7 Wire the audio engine into the composition root and make the sink the clock
    - `ApplicationComposition` constructs the single `AudioEngine` with the selected sink and the
      `DecoderTeardownQueue`, exposing `audioEngine()`
    - `PreviewController::pump` reads `AudioEngine::presentationPosition()` each pump: a frame more
      than one interval behind the audio position is counted dropped and skipped, more than one
      interval ahead waits, otherwise it is presented
    - _Requirements: 1.1, 6.3, 6.7_

### Stage 9 — Export

- [x] 9. Complete encode, mux and the export coordinator
  - [x] 9.1 Implement `media::EncoderSelector` and the hardware-skip helper
    - `src/media/EncoderSelector.{hpp,cpp}`: `EncoderSelection` with only the
      `EncoderSelection::hardware(...)` and `EncoderSelection::software(codec, reason)`
      constructors, so no path can report both hardware use and software fallback
    - Selection order: software immediately when hardware is not requested, not compiled in for the
      vendor, or the codec is absent from `GpuCaps::encodeCodecs`; otherwise a capability probe on
      a detached thread awaited with a 3000 ms deadline, a timeout treated as no compatible device;
      on a positive probe the vendor encoder, retrying initialization exactly once before falling
      back to `libx264` / `libx265` / `libvpx-vp9` with resolution, frame rate and bit rate
      unchanged and a reason naming hardware initialization failure
    - `tests/support/HardwareSkip.hpp`: `PALMIER_SKIP_WITHOUT_HW(codec, operation)` consulting
      `gpu::BridgeAvailability::fromBuildConfig()` and the live `GpuCaps`, calling `GTEST_SKIP()`
      with a reason naming the missing SDK or device
    - _Requirements: 8.2, 8.3, 8.4, 8.8, 15.5_

  - [x]* 9.2 Write the encoder-selection property tests
    - **Property 40: Exactly one encoder selection with consistent flags** —
      **Validates: Requirements 8.2, 8.8**
    - **Property 41: Hardware init failure retries once then falls back with parameters intact** —
      **Validates: Requirements 8.3**
    - **Property 42: Software selection uses the documented encoder for each codec** —
      **Validates: Requirements 8.4**
    - File: `tests/media/encoder_selector_property_test.cpp`; new target
      `palmier_media_encoder_selector_tests` using synthetic `GpuCaps` and `BridgeAvailability` so
      the tests always run
    - _Requirements: 8.2, 8.3, 8.4, 8.8_

  - [x] 9.3 Add the audio stream to the encoder and the export engine
    - `src/media/MediaEncoder.{hpp,cpp}`: `AudioEncodeSpec`, `std::optional<AudioEncodeSpec> audio`
      on `EncodeSpec`, `submitAudio(const AudioBuffer&, Duration)`, and a `finish()` that flushes
      both streams and writes the trailer
    - `src/media/ExportEngine.{hpp,cpp}`: for each video frame render, submit the frame, then submit
      that frame interval's audio mixed by an export-local `AudioGraph`; a timeline with no clip on
      an unmuted audio-bearing track still gets one silent stream spanning the full duration
    - _Requirements: 6.5, 6.11_

  - [x] 9.4 Implement `services::ExportCoordinator`
    - `src/services/ExportCoordinator.{hpp,cpp}`: `ExportRequest2`, `ExportOutcome`,
      `ExportProgressReport`, `begin()`, `cancel()`, `running()`, and the pure static `validate()`
      called before any file is created
    - One `std::thread` per export over a **value-copy `Project` snapshot**, an export-local
      `GpuContext`, `Compositor`, decoders, `AudioGraph` and `MediaEncoder`; progress marshalled to
      the main thread monotonically at ≤1 s intervals; a scope guard calls `MediaEncoder::finish()`
      best-effort then removes the output path on any failure, cancellation or mid-export hardware
      failure; a second concurrent request is rejected without disturbing the running export; an
      existing destination without overwrite acknowledgement is rejected and preserved
      byte-for-byte
    - _Requirements: 6.10, 7.1, 7.2, 7.3, 7.4, 7.5, 7.6, 7.7, 7.9, 7.10, 7.11, 8.11_

  - [x]* 9.5 Write the export-coordinator property tests
    - **Property 33: Export runs exactly the requested parameters and never touches the project** —
      **Validates: Requirements 7.1, 7.2**
    - **Property 34: Progress is monotonic, bounded and timely** — **Validates: Requirements 7.3**
    - **Property 35: A successful export matches the planner** — **Validates: Requirements 7.4**
    - **Property 36: Any failure after encoding begins leaves no file and no project change** —
      **Validates: Requirements 6.10, 7.5, 8.11**
    - **Property 37: Cancellation leaves no file and no project change** —
      **Validates: Requirements 7.7**
    - **Property 39: Invalid export requests are rejected before any file exists** —
      **Validates: Requirements 7.6, 7.9, 7.11**
    - File: `tests/services/export_coordinator_property_test.cpp`; new target
      `palmier_services_export_coordinator_tests`
    - _Requirements: 6.10, 7.1, 7.2, 7.3, 7.4, 7.5, 7.6, 7.7, 7.9, 7.11, 8.11_

  - [x]* 9.6 Extend the export determinism property test
    - **Property 38: Two successive exports are identical** — **Validates: Requirements 7.8**
    - File: `tests/media/media_export_ordering_property_test.cpp` (extends the existing test and its
      `palmier_media_export_ordering_property_tests` target)
    - _Requirements: 7.8_

  - [x] 9.7 Wire export into the tool surface and the composition root
    - Connect the `timeline.export` hook to `ExportCoordinator` so the tool returns the output path,
      encoded frame count, selected encoder name, hardware/fallback flags, audio presence and
      duration; expose `exportCoordinator()`
    - Add `ApplicationComposition::codecBackendReport()` returning a `CodecBackendStatus` per
      VAAPI, NVENC/NVDEC, QSV and FFmpeg software backend with compiled-in and usable-on-host
      booleans, always reporting the software backend as both, within 3000 ms and without changing
      any selection or export state
    - _Requirements: 1.1, 3.1, 7.2, 8.7_

  - [x]* 9.8 Write the hardware-versus-software output comparison test
    - Export the ≥300-frame 1920×1080/30 fps fixture once with hardware and once with software
      encoding, asserting both outputs decode, both frame counts equal the fixture count, and the
      durations differ by at most one frame interval; guard with `PALMIER_SKIP_WITHOUT_HW`
    - _Requirements: 8.6, 15.5_

  - [x] 9.9 Checkpoint
    - Ensure all tests pass, ask the user if questions arise. The headless sequence of Requirement
      3.6 now completes end to end.

### Stage 10 — Pluggable backends (offline-first, no network dependency)

- [x] 10. Make agent reasoning and generation pluggable
  - [x] 10.1 Implement the offline interpreter and the interpreter registry
    - `src/services/OfflineIntentInterpreter.{hpp,cpp}`: at least 12 documented phrase patterns
      (`split the clip at the playhead`, `mute track N`, `unmute track N`, `add a video track`,
      `add an audio track`, `delete the selected clip`, `import <path>`,
      `export as mp4 to <path>`, `save the project`, `undo`, `redo`, `show the timeline`) matched
      case-insensitively on the whitespace-trimmed utterance, resolving to tool invocations with
      captured arguments, issuing no network request and answering well inside 1 s
    - `src/services/AgentInterpreterRegistry.{hpp,cpp}`: ids `offline` (default), `hosted`, `byok`;
      replaces `makeUnconfiguredInterpreter()`; `AppConfig` gains `agentInterpreterId` and
      `ApplicationComposition` exposes `agentInterpreterId()`
    - Reject an utterance that is empty after whitespace removal or longer than 2000 characters
      with the permitted range; quote an unmappable utterance and invoke no tool; report credential
      or backend failures without touching the project
    - _Requirements: 11.1, 11.2, 11.3, 11.4, 11.8, 11.9_

  - [x]* 10.2 Write the offline-interpreter property tests
    - **Property 59: The offline interpreter is case- and whitespace-insensitive** —
      **Validates: Requirements 11.3**
    - **Property 60: Interpreter output is always executable** — **Validates: Requirements 11.2**
    - **Property 61: Unmappable utterances change nothing** — **Validates: Requirements 11.4**
    - **Property 64: Utterance length bounds are enforced** — **Validates: Requirements 11.9**
    - File: `tests/services/offline_interpreter_property_test.cpp`; new target
      `palmier_services_agent_offline_tests`
    - _Requirements: 11.2, 11.3, 11.4, 11.9_

  - [x]* 10.3 Write the agent-equivalence property test
    - **Property 62: The agent path equals a direct tool invocation** —
      **Validates: Requirements 11.5, 11.10**
    - File: `tests/services/agent_equivalence_property_test.cpp`
    - _Requirements: 11.5, 11.10_

  - [x]* 10.4 Write the mention-resolution property test
    - **Property 63: A unique @mention is substituted; a non-unique one is refused** —
      **Validates: Requirements 11.6, 11.7**
    - File: `tests/services/mention_resolver_property_test.cpp` (extends the existing
      `palmier_services_mention_resolver_tests` target)
    - _Requirements: 11.6, 11.7_

  - [x] 10.5 Implement the generative backend registry and its clients
    - `src/services/GenerativeBackendRegistry.{hpp,cpp}`: ids `offline` | `hosted` | `byok`, all
      compiled in and selected by configuration string with no recompilation
    - `src/services/HostedGenerativeBackend.{hpp,cpp}` and
      `src/services/ByokGenerativeBackend.{hpp,cpp}`: GPLv3 HTTPS clients reading credentials from
      `SecretStore` at runtime, with no endpoint credential values checked in
    - An unknown id or absent credentials installs `offline`, records a startup error naming the
      rejected id and the unmet requirement in `startupErrors()`, and still constructs every other
      component; `ApplicationComposition` exposes `generativeBackendId()`
    - Route every `generation.generate` invocation from the tool surface, the endpoint and the agent
      through the selected backend; reject offline requests within 1 s with the unmet precondition
      named and no library, clip or undo entry added
    - _Requirements: 12.1, 12.2, 12.4, 12.8_

  - [x]* 10.6 Write the generation lifecycle property tests
    - **Property 65: A successful generation is one undoable edit** —
      **Validates: Requirements 12.3**
    - **Property 66: Invalid generation requests never reach the network** —
      **Validates: Requirements 12.9**
    - File: `tests/services/generative_lifecycle_property_test.cpp`; new target
      `palmier_services_generative_lifecycle_tests`, with a network seam that fails the test if
      invoked
    - _Requirements: 12.3, 12.9_

  - [x]* 10.7 Extend the generation atomicity property test
    - **Property 67: A failed or timed-out job leaves nothing behind** —
      **Validates: Requirements 12.7, 12.10**
    - File: `tests/services/generative_atomicity_property_test.cpp` (extends the existing test)
    - _Requirements: 12.7, 12.10_

  - [x]* 10.8 Write the repository hygiene property test
    - **Property 68: Every source file carries the GPLv3 SPDX header and no credentials** —
      **Validates: Requirements 12.6**
    - File: `tests/docs/repository_hygiene_property_test.cpp`
    - _Requirements: 12.6_

  - [x]* 10.9 Write the offline-mode availability sweep test
    - One representative operation from each of edit, playback, save, open, export and `tools/call`
      succeeds while Offline_Mode applies, and the generation-unavailable indication requires no
      dismissal and blocks no other command
    - _Requirements: 12.5_

### Stage 11 — GUI assembly (last, because it consumes every service above)

- [x] 11. Mount the editor shell on the composed graph
  - [x] 11.1 Implement `ui::GuiToolGateway`
    - `src/ui/GuiToolGateway.{hpp,cpp}`: one method per gesture (`moveClip`, `trimClip`,
      `splitClip`, `reorderClips`, `addClip`, `deleteClip`, `addEffect`, `addTransition`,
      `addTrack`, `removeTrack`, `importMedia`, `createProject`, `openProject`, `saveProject`,
      `exportTimeline`), each building `Json` arguments and calling
      `McpToolExecutor::executeTool(name, args, InvocationSource::Gui)` and returning the existing
      `ui::GestureResult`
    - _Requirements: 1.7_

  - [x] 11.2 Build the docked shell, the menu bar and the status bar
    - `src/ui/MainWindow.{hpp,cpp}` takes `app::ApplicationComposition&` and builds five
      `QDockWidget`s — timeline (bottom), preview (central), inspector (right), media browser
      (left), agent chat (right, tabbed with the inspector) — each bound to the composition's single
      view-model instance, all visible with no further user action
    - Menus in order: **File** (New, Open…, Save, Save As…, Quit), **Edit** (Undo, Redo, Delete
      Clip, Split at Playhead), **Playback** (Play/Pause, Stop, Go to Start), **Export** (Export
      Video…, Cancel Export), **Help** (Documentation, About); Undo/Redo enabled from
      `canUndo()/canRedo()`
    - `setMinimumSize(1024, 640)` on the window and `setMinimumSize(80, 60)` on each dock; status
      bar hosts the persistent GPU-unavailable notice from `gpuUnavailableNotice()`, the
      software-compositing notice and the audio-unavailable notice
    - This is the Linux adaptation of upstream PR 404: a fixed minimum-size contract rather than
      per-frame layout recomputation
    - _Requirements: 1.2, 1.3, 1.4, 1.6, 1.8, 1.11, 5.6, 6.7_

  - [x] 11.3 Add the timeline panel and the timer-driven preview view
    - `src/ui/TimelinePanel.{hpp,cpp}`: a `QTreeView` over the existing `TimelineModel` plus a
      transport bar and the playhead indicator
    - `src/ui/PreviewView.{hpp,cpp}`: drive `PreviewController::pump` from a `QTimer` and paint the
      presented frame
    - _Requirements: 1.2, 5.3_

  - [x] 11.4 Re-point view-model gesture methods at the gateway
    - In `TimelineViewModel`, `InspectorViewModel`, `MediaBrowserViewModel` and
      `AgentChatViewModel`, keep the read-side projections but route every mutating gesture through
      `GuiToolGateway` instead of `TimelineEngine::apply`, so the GUI stops mutating project state
      directly
    - _Requirements: 1.7_

  - [x] 11.5 Implement `ui::ProjectFileActions` and the file dialogs
    - `src/ui/ProjectFileActions.{hpp,cpp}`: File > Save to the recorded document path with a
      confirmation naming the written path; File > Save As / first save via a destination prompt
      defaulting to `.palmier`; File > Open refreshing all four panels through the existing
      `ChangeSet` broadcast; a dismissed prompt writes nothing, loads nothing and reports no error
    - Explicit `PendingIntent` state machine for the unsaved-changes prompt: exactly three options,
      no state change and no file write while displayed, then exactly one outcome with no further
      prompting — save continues the pending close/open only if the write succeeds, discard
      continues without writing, cancel abandons the pending operation
    - _Requirements: 4.1, 4.2, 4.3, 4.5, 4.9, 4.10_

  - [x] 11.6 Implement `ui::ExportDialog` and the export progress surface
    - `src/ui/ExportDialog.{hpp,cpp}`: output path, container, codec, resolution, frame rate, bit
      rate, audio inclusion, hardware preference and overwrite acknowledgement, submitted through
      `GuiToolGateway::exportTimeline`
    - Status-bar progress bar with a cancel button, fed by the coordinator's marshalled progress
      callback, keeping the window responsive throughout
    - _Requirements: 7.1, 7.3, 7.7_

  - [x] 11.7 Guard startup construction and finish the accessor set
    - Wrap composition so a failure to construct any component named in Requirement 1.1 yields
      `ComponentConstructionError{componentName, reason}`; `src/app/main.cpp` reports it and exits
      without showing the shell
    - Verify every Requirement 1.1 accessor returns a non-null reference to the same instance for
      the process lifetime, and that startup with no supplied project path makes an empty default
      project current
    - _Requirements: 1.1, 1.9, 1.10_

  - [x]* 11.8 Write the shell layout property test
    - **Property 1: Panel reachability under any window size** — **Validates: Requirements 1.4**
    - File: `tests/ui/shell_layout_property_test.cpp`; new target `palmier_ui_shell_tests` run under
      `xvfb`
    - _Requirements: 1.4_

  - [x]* 11.9 Extend the edit-equivalence property test
    - **Property 2: GUI, MCP and agent produce identical project state** —
      **Validates: Requirements 1.7, 9.4, 11.5**
    - File: `tests/services/edit_equivalence_property_test.cpp` (extends the existing test) — this
      is what proves the gesture re-pointing in 11.4 changed no behaviour
    - _Requirements: 1.7, 9.4, 11.5_

  - [x]* 11.10 Extend the undo round-trip property test
    - **Property 3: Undo restores the immediately prior state** — **Validates: Requirements 1.8**
    - File: `tests/core/timeline_undo_redo_roundtrip_property_test.cpp` (extends the existing test)
    - _Requirements: 1.8_

  - [x]* 11.11 Write the shell unit and responsiveness tests
    - Qt widget tests under `xvfb`: five panels present and visible, five menus in the required
      order each with an activatable action, each notice persistent until dismissal or exit, and
      Undo disabled on an empty history
    - One injected construction failure per component named in Requirement 1.1
    - One test per (trigger × outcome) pair of the unsaved-changes prompt, plus an assertion that
      nothing is written or changed while the prompt is displayed
    - Main-thread event-latency sampling during an export (200 ms bound) and during a slow
      successful and a slow failing save (100 ms bound)
    - _Requirements: 1.2, 1.3, 1.6, 1.9, 1.11, 4.5, 7.3, 14.6_

  - [x] 11.12 Checkpoint
    - Ensure all tests pass, ask the user if questions arise. The full import → edit → play →
      save → open → export workflow is now reachable from the GUI.

### Stage 12 — Documents, suite hygiene, CI

- [x] 12. Author the checked-in document set and close out verification
  - [x] 12.1 Author `docs/UPSTREAM_PARITY.md`
    - Provenance block: `upstream-repository: https://github.com/palmier-io/palmier-pro`,
      `upstream-ref`, `linux-ref`, `comparison-date: YYYY-MM-DD`, plus the three status definitions
      stated in reachability terms
    - Two fixed-column tables: the 22 upstream tool categories and the 12 capability areas, each
      entry exactly once with one status, `linux-components` (or `none`), a priority and a 1–200
      character rationale whenever the status is `absent` or `partial`, and
      `macos-framework` / `linux-replacement` (name or `out-of-scope: <reason>`) where applicable
    - Build-order list: a projection containing exactly the `absent` and `partial` entries, sorted
      `must` before `should` before `later`
    - _Requirements: 13.1, 13.2, 13.3, 13.4, 13.5, 13.7, 13.9_

  - [x] 12.2 Author `docs/PORT_BACKLOG.md` for all ten identified upstream changes
    - Provenance block with `upstream-repository`, `upstream-range` and
      `window: 2026-06-25..2026-07-25`; one entry per change carrying identifier, ≤200-character
      summary, one disposition of `port`/`adapt`/`not-applicable`, a rationale of at least one
      sentence naming the affected Linux component, a `check:` block with `given`/`when`/`then` for
      every `port`/`adapt` entry, and a `status` field
    - **Implemented by this feature** (entry records the landing component and `status: complete`
      once its check passes): **PR 403** → `ProjectSession::requestSave` (task 2.2);
      **PR 405** → `media::DecoderTeardownQueue` (task 7.1); **PR 408** → `EffectType::InvertColors`
      (task 1.3); **PR 404** → the `MainWindow` minimum-size contract (task 11.2);
      **PR 399** → the `FetchContent` pins and the documented package set (tasks 0.4, 12.6)
    - **Backlog entry only — the deferred implementation is not part of this feature**:
      **PR 397** multicam ripple trim (`clipGroups` reserved by task 1.5),
      **PR 406** generation model catalog, **PR 396** upscale generation mode,
      **PR 395** source-or-prompt audio generation; each entry is authored with its disposition,
      rationale and acceptance check and left at `status: not-started`
    - **PR 401** non-English README maintenance: `not-applicable`, with the structural reason and
      no acceptance check
    - _Requirements: 14.1, 14.2, 14.3, 14.9, 14.12_

  - [x] 12.3 Implement `tests/support/ReportParser` and the two report checkers
    - `tests/support/ReportParser.{hpp,cpp}`: `parseParityReport`, `parsePortBacklog`,
      `checkParityReport`, `checkPortBacklog`, producing `ParityEntry` / `BacklogEntry` values and a
      `std::vector<Defect>` over the defect kinds `MissingEntry`, `DuplicateEntry`, `InvalidStatus`,
      `InvalidPriority`, `MissingRationale`, `MissingCheck`, `MissingField`, `DuplicateIdentifier`,
      `OutOfOrder`
    - Dependency-free and total — a malformed document yields defects, never an exception; reads
      from `${PROJECT_SOURCE_DIR}/docs/` via a `PALMIER_DOCS_DIR` compile definition; new target
      `palmier_docs_tests`
    - _Requirements: 13.8, 14.11_

  - [x]* 12.4 Write the parity-report property tests
    - **Property 69: Parity_Report well-formedness** —
      **Validates: Requirements 13.1, 13.2, 13.3, 13.5, 13.6, 13.9**
    - **Property 70: The parity check detects every malformation** —
      **Validates: Requirements 13.8**
    - File: `tests/docs/parity_report_property_test.cpp`, asserting the checked-in document has no
      defects and holding over generated well-formed documents and mutations
    - _Requirements: 13.1, 13.2, 13.3, 13.5, 13.6, 13.8, 13.9_

  - [x]* 12.5 Write the port-backlog property tests
    - **Property 71: Port_Backlog well-formedness** —
      **Validates: Requirements 14.1, 14.3, 14.9, 14.10**
    - **Property 72: The backlog check detects every malformation** —
      **Validates: Requirements 14.11**
    - File: `tests/docs/port_backlog_property_test.cpp`
    - _Requirements: 14.1, 14.3, 14.9, 14.10, 14.11_

  - [x] 12.6 Author the operator and agent-user documentation set
    - `docs/BUILD.md`: the complete configure/build/test/launch sequence from a clean checkout, the
      native package names per supported distribution family, every `PALMIER_*` option with its
      default and effect, and the minimum host specification
    - `docs/MCP_CLIENTS.md`: the full client configuration entry for Claude Code, Codex and Cursor
      at `127.0.0.1:19789`, and the confirmation that the client lists the Requirement 3.1 tools
    - `docs/REMOTE_ACCESS.md`: bind address, token generation (≥32 characters), acknowledgement
      flag, TLS material or the tunnel alternative, Origin allow-list, maximum sessions (≤32), idle
      timeout (≤3600 s), and the unencrypted-traffic warning
    - `docs/TOOLS.md`: every tool under the name `tools/list` returns, with its description, each
      argument's JSON type and required/optional marking, and every success-result field
    - `docs/HARDWARE_ENCODE.md`: per-vendor driver and runtime prerequisites, the CMake option that
      compiles each path, a host-verification command, and the L4 validation procedure with its
      fixture, command and recorded values
    - `docs/QUICKSTART.md`: the launch → import → add tracks → place clip → play → save → re-open →
      export walkthrough with the observable result of each step, plus remediation for no compatible
      GPU, unavailable audio output and a refused MCP connection
    - Trim `README.md` to an overview plus links into `docs/`, so each checked name lives in exactly
      one file
    - _Requirements: 16.1, 16.2, 16.3, 16.4, 16.5, 16.6_

  - [x] 12.7 Implement the documentation consistency checker
    - Emit `palmier_options.txt` at configure time from `get_cmake_property(... CACHE_VARIABLES)`
      filtered to `PALMIER_*`
    - Extract the documented option names from `docs/BUILD.md` and the documented tool, argument and
      result names from `docs/TOOLS.md`, and compare them two-way against that list and against the
      live `ToolRegistry::describe()` plus each `ToolSchema`; never write to the documentation
    - _Requirements: 16.7, 16.8_

  - [x]* 12.8 Write the documentation consistency property tests
    - **Property 76: Documentation and the running system agree on every name** —
      **Validates: Requirements 16.4, 16.7**
    - **Property 77: The documentation check reports every mismatch and modifies nothing** —
      **Validates: Requirements 16.8**
    - File: `tests/docs/documentation_consistency_property_test.cpp`
    - _Requirements: 16.4, 16.7, 16.8_

  - [x] 12.9 Delete the placeholder test and add the suite-hygiene property in the same task
    - Delete `tests/palmier_placeholder_property_test.cpp` and its `palmier_placeholder_tests`
      target, and in the **same commit** add:
    - **Property 80: Every registered test asserts a named component and none is a placeholder** —
      **Validates: Requirements 15.6**
    - File: `tests/docs/suite_hygiene_property_test.cpp` — so Requirement 15.6 is never transiently
      violated
    - _Requirements: 15.6_

  - [x] 12.10 Write the end-to-end test and the fixture generator
    - `tests/e2e/editor_end_to_end_test.cpp` drives the assembled `ApplicationComposition` with the
      null audio sink and an injected probe/decode/encode backend triple producing real bytes:
      `project.create` at 1920×1080/30 fps → `media.import` of a fixture with one video and one
      audio stream of ≥2 s → `timeline.add_track` ×2 → `timeline.add_clip` ×2 → play ≥24 consecutive
      frames under a controlled clock → `project.save` → `project.open` → `timeline.export`, then
      assert the output probes and its duration equals the timeline duration within one frame
      interval; every step goes through the Tool_Surface, so it also covers Requirement 3.6
    - Generate the 2-second synthetic A/V source and the reference `.palmier` document into
      `tests/fixtures/` at build time rather than checking in binaries; a missing or unreadable
      fixture makes the consuming test **fail** with the fixture named, never skip
    - New target `palmier_e2e_tests`
    - _Requirements: 3.6, 15.1, 15.9_

  - [x] 12.11 Publish the CI test log, summary and skip reasons
    - Run `ctest --output-on-failure --output-junit ctest-results.xml` under `xvfb-run -a`, and
      upload **unconditionally** the full test log plus a generated summary listing every test's
      name and its outcome of passed, failed or skipped with its recorded skip reason
    - _Requirements: 15.2, 15.5, 15.7_

  - [x] 12.12 Add the separate `l4-validation` CI job
    - A manually-triggered / self-hosted job that exports the ≥300-frame 1920×1080/30 fps fixture
      with `h264_nvenc`, records the selected encoder name, the elapsed wall-clock milliseconds and
      the output size in bytes as job output, and exits non-zero if the encoder is not
      `h264_nvenc`, the software-fallback flag is true, or the size is 0 — while still publishing
      the measurements
    - _Requirements: 8.5, 8.10, 16.5_

  - [x] 12.13 Final checkpoint
    - Ensure all tests pass, ask the user if questions arise.

---

## Notes

- Sub-tasks marked `*` are test-only and can be deferred for a faster slice; they are nevertheless
  placed inside their own stage, never batched at the end, because the design requires a stage's
  tests to land with the stage. Tasks that a requirement names as a deliverable in its own right —
  the report and documentation checkers (12.3, 12.7), the placeholder deletion paired with Property
  80 (12.9), the end-to-end test (12.10) and the CI jobs (12.11, 12.12) — are **not** optional.
- Task 3.4 is the only unavoidable multi-file atomic commit. It is safe because 3.2 has already
  proved each converted tool's schema byte-equal to today's, reducing 3.4 to a mechanical
  substitution.
- Cross-stage dependencies the design calls out explicitly: `DecoderTeardownQueue` (7.1) precedes
  the audio stage; `NullAudioSink` (8.3) precedes the real sinks (8.6); every new dependency enters
  behind optional `PALMIER_ENABLE_<X>` / `PALMIER_HAVE_<X>` detection with a working fallback, so no
  stage breaks configuration on a host that configured at the previous stage.
- Upstream ports implemented here: **PR 403, 405, 408, 404, 399**. Upstream changes authored as
  backlog entries only, with the implementation deferred beyond this feature: **PR 397, 406, 396,
  395**. **PR 401** is `not-applicable`.
- `xvfb`-dependent Qt tests stay confined to `palmier_ui_shell_tests` and `palmier_e2e_tests`, so a
  host without a display can still run the full non-GUI suite.

---

## Task Dependency Graph

```mermaid
graph TD
    S0["Stage 0<br/>Build-flag fix<br/>0.1–0.5"]
    S1["Stage 1<br/>Core extensions<br/>1.1–1.5"]
    S2["Stage 2<br/>ProjectSession<br/>2.1–2.4"]
    S3a["Stage 3a<br/>ToolSchema + conversion<br/>3.1–3.3"]
    S3b["Stage 3b<br/>Session switch (atomic)<br/>3.4"]
    S4["Stage 4<br/>Session + media tools<br/>4.1–4.7"]
    S5["Stage 5<br/>MCP protocol layer<br/>5.1–5.5"]
    S6["Stage 6<br/>Remote access<br/>6.1–6.6"]
    S7["Stage 7<br/>Playback<br/>7.1–7.6"]
    S8["Stage 8<br/>Audio<br/>8.1–8.7"]
    S9["Stage 9<br/>Export<br/>9.1–9.8"]
    S10["Stage 10<br/>Backends<br/>10.1–10.9"]
    S11["Stage 11<br/>GUI assembly<br/>11.1–11.11"]
    S12["Stage 12<br/>Docs, hygiene, CI<br/>12.1–12.12"]

    S0 --> S1
    S1 --> S2
    S2 --> S3a
    S3a --> S3b
    S3b --> S4
    S4 --> S5
    S5 --> S6
    S2 --> S7
    S4 --> S7
    S7 --> S8
    S1 --> S9
    S2 --> S9
    S7 --> S9
    S8 --> S9
    S4 --> S10
    S9 --> S11
    S10 --> S11
    S6 --> S11
    S11 --> S12

    S0 -. "hardware paths compiled in CI<br/>before any encode work" .-> S9
    S7 -. "7.1 DecoderTeardownQueue<br/>reused by the audio decoder (PR 405)" .-> S8
    S1 -. "schema 1.1 readable at every later stage" .-> S12
    S11 -. "tool surface final before<br/>the documentation checkers" .-> S12
```

```json
{
  "waves": [
    { "id": 0,  "tasks": ["0.1", "0.2", "0.3", "0.4", "0.5"] },
    { "id": 1,  "tasks": ["1.1", "1.2", "1.3", "1.5"] },
    { "id": 2,  "tasks": ["1.4", "2.1"] },
    { "id": 3,  "tasks": ["2.2", "3.1"] },
    { "id": 4,  "tasks": ["2.3", "2.4", "3.2"] },
    { "id": 5,  "tasks": ["3.3", "3.4"] },
    { "id": 6,  "tasks": ["4.1", "4.3"] },
    { "id": 7,  "tasks": ["4.2", "4.4"] },
    { "id": 8,  "tasks": ["4.5", "5.1"] },
    { "id": 9,  "tasks": ["4.6", "4.7", "5.2"] },
    { "id": 10, "tasks": ["5.3", "6.1"] },
    { "id": 11, "tasks": ["5.4", "5.5", "6.2"] },
    { "id": 12, "tasks": ["6.3", "6.4", "6.5"] },
    { "id": 13, "tasks": ["6.6", "7.1"] },
    { "id": 14, "tasks": ["7.2", "7.3"] },
    { "id": 15, "tasks": ["7.4", "7.5", "8.1"] },
    { "id": 16, "tasks": ["7.6", "8.2", "8.3"] },
    { "id": 17, "tasks": ["8.4", "8.6"] },
    { "id": 18, "tasks": ["8.5", "8.7", "9.1"] },
    { "id": 19, "tasks": ["9.2", "9.3"] },
    { "id": 20, "tasks": ["9.4", "10.1"] },
    { "id": 21, "tasks": ["9.5", "9.6", "9.7", "10.2"] },
    { "id": 22, "tasks": ["9.8", "10.3", "10.4", "10.5"] },
    { "id": 23, "tasks": ["10.6", "10.7", "10.8", "10.9", "11.1"] },
    { "id": 24, "tasks": ["11.2", "11.4"] },
    { "id": 25, "tasks": ["11.3", "11.5", "11.6"] },
    { "id": 26, "tasks": ["11.7", "11.8", "11.9", "11.10"] },
    { "id": 27, "tasks": ["11.11", "12.1", "12.2", "12.3"] },
    { "id": 28, "tasks": ["12.4", "12.5", "12.6"] },
    { "id": 29, "tasks": ["12.7", "12.9", "12.10"] },
    { "id": 30, "tasks": ["12.8", "12.11", "12.12"] }
  ]
}
```

Checkpoint tasks (4.8, 9.9, 11.12, 12.13) are intentionally excluded from the wave schedule.


---


## Progress

### Defect fix — configuration never reached the binary (no task ticked)

`src/app/main.cpp` constructed `ApplicationComposition` from a **default-constructed `AppConfig`** and
never called `AppSettings::load()`/`fromArgv()`. `app::AppSettings` (task 6.1) resolved four layers
into an `AppConfig` that nothing then used, so from the shipped binary the MCP endpoint could not be
moved, remote access could not be turned on at all, and `agentInterpreterId` / `generativeBackendId`
(tasks 10.1, 10.5) could not be selected — the hosted and BYOK interpreters and backends were
reachable only from tests. Fixed by wiring the entry point to the settings API that already existed:
resolve `fromArgv` (defaults < config file < `PALMIER_*` environment < command line), then construct
the composition from `settings.config()`, on both the Qt and the no-Qt path.

Three smaller changes came with it, each the minimum the fix needed:

- **`generative.backend` / `PALMIER_GENERATIVE_BACKEND` / `--generative-backend`** joins the key
  table. It was simply missing, so `generativeBackendId` had no configuration name; validation is
  split exactly as `agent.interpreter`'s is, leaving the registry's Requirement 12.8 fallback and its
  startup error intact instead of silently substituting the default here.
- **`AppSettings::helpRequested()` + `AppSettings::usage()`.** `--help` / `-h` now prints the accepted
  options — generated from the key table, so the text cannot drift — and exits before a QApplication,
  a composition or a session is constructed. An unknown option is **not** fatal: per the existing
  resolver contract it is reported on stderr together with the option list and ignored, leaving the
  safe default in place, and the editor still starts.
- **Startup diagnostics are surfaced.** `AppSettings::diagnostics()`, then
  `ApplicationComposition::startupErrors()` and, after `start()`, `remoteAccessStartupError()` /
  `remoteAccessWarning()` all go to stderr. Nothing silent, and no value (hence no token) is printed.

**Tests: 1124 total, from 1119** — 3 in `tests/app/app_settings_test.cpp` (the two selection ids
resolve from file/env/CLI with the documented precedence; `--help` sets the request without becoming a
diagnostic or a positional; `usage()` names every recognized key, flag and variable) and 2 new
`ApplicationStartupWiring` cases in `tests/app/application_composition_test.cpp`, where
`AppSettings.cpp` now joins that target so the resolved config and the composition constructed from it
can be asserted in one place. The wiring cases pin what was unreachable: a config file plus argv
overrides reach `mcpPort`, `remote.maxSessions`, the gate's `RemoteAccessConfig` and both selection
ids, an unknown option is reported rather than swallowed, and the rejected ids produce the documented
fallback-plus-startup-error while the application still starts. Hermetic: a pid-qualified scratch
config file, the injected environment seam, an ephemeral **loopback** port, and a remote configuration
whose prerequisites are deliberately unmet so no external address is ever bound. Same 3 expected
skips; no existing test changed.

> **`main.cpp` is not compiled in `build-nogui`** — `src/app/CMakeLists.txt` guards the `palmier-pro`
> target with `PALMIER_BUILD_UI AND TARGET Qt6::Widgets`, and Qt is absent. Its no-Qt path was
> syntax-checked directly (`g++ -std=c++20 -fsyntax-only -Wall -Wextra -Isrc src/app/main.cpp`), which
> covers the shared prologue (resolution, `--help`, both report helpers) and the console shell; the few
> Qt-only lines remain unverified until a Qt build. Everything the entry point *does* — resolution and
> composition-from-resolved-config — is covered by the runnable tests above.

**Stage 10 is now closed, and 12.6 with it — the stage-10 parent is ticked for the first time.**

**10.9 was the last open stage-10 sub-task.** It adds
`tests/services/offline_mode_availability_test.cpp` on the new
`palmier_services_offline_mode_tests` target: **8 cases, 1119 tests total** (from 1111), with 3
skips rather than 2. It is example-based on purpose — design.md lists the offline-mode sweep among
the surfaces deliberately NOT property-tested, because each of the six operations already has its
own property suite and what was unproven was their *availability*, not their behaviour.

The six representative operations, all over ONE `ProjectSession` and all through product code:
**edit** = `timeline.add_clip` via `McpToolExecutor`; **playback** = `ui::PreviewController`
play+pump over an injected `PlaybackClock`, compositing through `gpu::Compositor` on the software
fallback; **save** = `project.save`; **open** = `project.open` on the document the save leg just
wrote (so the open leg is non-vacuous without a checked-in fixture); **export** = `timeline.export`
through the real `makeExportToolHandler` over a real `ExportCoordinator`; **`tools/call`** =
`McpProtocolHandler` running the real `initialize` → `notifications/initialized` → `tools/call`
sequence, with a *mutating* call so "the endpoint is available" means an edit landed.

**Three things about 10.9 a reader should not have to infer.**

1. **Offline_Mode is swept in BOTH of the shapes task 10.5's registry can produce**, because they
   surface different indications: nothing configured (`offline` installed as the default, no startup
   error, the generic precondition) and `hosted` configured with no account (fallback to `offline`
   plus the Requirement 12.8 startup error). Assuming they behave alike would have been an
   assumption, so both run the full sweep.
2. **"Requires no dismissal and blocks no other command" is asserted as three checkable facts**, not
   as an intention: the indication is a *value* read on demand (`unmetPrecondition()` /
   `startupError`) that is byte-identical across repeated reads and still readable after the sweep,
   the tool surface advertises no dismiss/acknowledge/clear operation, and the full six-operation
   sweep is re-run **after** one refusal and again after five consecutive refusals with **nothing
   done in between** — no acknowledgement, no reset. A refusal is also shown to leave the session
   revision, undo depth, clip count and media-library size untouched, which is what makes "the next
   command is unaffected" true of the project rather than only of the return value.
3. **The no-network claim is guarded against vacuity twice.** A `ForbiddenTransport` (`ADD_FAILURE()`
   when asked to send) stays installed for the whole life of the rig, and the `dlsym(RTLD_NEXT, ...)`
   socket interposers of tasks 10.5/10.6 are armed around the refusal itself.
   `TheInterposersObserveARealSocketCall` opens a real socket so an unlinked interposer fails there,
   and `TheTransportSeamIsLive` builds the **same** rig with the credentials present and shows the
   transport is then provably reached — so the offline zero is a refusal, not a hole in the wiring.
   The rig also stores a BYOK credential for the model every request names, so the entitlement gate
   *downstream* of the hook would have passed: the refusal is attributable to the selected backend's
   unmet precondition and to nothing else.

**The export leg is the one host-dependent part, and it is split rather than weakened.** The sweep
supplies the encode backend through `ExportCoordinator`'s own `encodeFactory` seam — the same
arrangement `export_coordinator_test.cpp` uses, writing a real file — so offline export is asserted
on every host. A seventh case, `ExportWithTheHostEncoder`, removes that seam and runs the identical
`timeline.export` call through the **production FFmpeg backend**; it probes availability through
`media::MediaEncoder::create` and **SKIPS with a recorded reason** when the host has none. It skips
on this sandbox: `Unsupported: encoder not found: libx264`. That is the third skip, and it is the
same idiom task 9.8 uses.

**No product defect was found.** Every one of the six operations succeeds offline, unchanged. The
two failures seen while writing the test were both in the test: a frame-sink count that attributed
the frames a `seek()` presents on its own account to the pump loop, and a rig that omitted the BYOK
credential the entitlement gate needs, which made the liveness case refuse before reaching the
transport.

**12.6 is now ticked.** Its remaining three documents — `docs/TOOLS.md`, `docs/HARDWARE_ENCODE.md`,
`docs/QUICKSTART.md` — and the `README.md` trim are written and committed at `50bbd31`, so the whole
of the checked name set now lives in exactly one file each.

**12.7 is now ticked — 1150 tests, 3 expected skips (+26).** `cmake/PalmierOptionsManifest.cmake`
emits `palmier_options.txt` from `get_cmake_property(... CACHE_VARIABLES)` filtered to `PALMIER_*`;
it is called at the very END of the top-level `CMakeLists.txt`, because `tests/` creates
`PALMIER_PBT_MIN_SUCCESS` and any earlier call would emit an incomplete set (16 entries).
`tests/support/DocumentationChecker.{hpp,cpp}` extracts and compares, `tests/docs/`
`documentation_consistency_test.cpp` runs it on `palmier_docs_tests` — the shared target, extended
with `target_sources()` as its block instructs, with the tool surface / session / `AppSettings`
sources listed explicitly so the binary still links no libav\*, libsecret, lcms2, Vulkan or Qt.

**Nothing is restated in the test.** The option set comes from the manifest; the tool and argument
names come from `ToolRegistry::describe()`, i.e. the `tools/list` payload itself; the result fields
come from **invoking** the twenty registry-owned handlers over a real `ProjectSession`; the settings
keys come from `AppSettings::recognizedKeys()` and its env/flag accessors. `timeline.export` and
`generation.generate` render their payloads outside the registry (`exportOutcomeToJson`,
`makeGenerateHook`), and linking either collaborator would drag FFmpeg/libsecret/lcms2 in, so those
two are compared against the field names their real rendering function sets, read out of the source
of record — with `FunctionFieldScan::found` failing loudly if either function is renamed, rather than
checking nothing. Nineteen `DocumentationChecker*` cases inject each fault the checker must catch
(missing marker, renamed option, wrong Type, flipped `**yes**`, changed JSON type, missing `*uuid*`,
reordered tools, undocumented/never-returned result field, wrong env var), and drift was also
confirmed end-to-end by temporarily mutating the real `docs/BUILD.md` and `docs/TOOLS.md`.

**Five real doc/code inconsistencies were caught, and the DOC was fixed in each case** (the docs are
the derived artefact; no behaviour was changed):

1. `docs/REMOTE_ACCESS.md`'s table is headed "Every configuration input" but omitted
   **`agent.interpreter`** and **`generative.backend`** — two of the fourteen keys in
   `AppSettings.cpp`'s table. Both rows added, with a note that they are not remote-access inputs and
   only live there because that is where the surface is tabulated.
2. `docs/TOOLS.md` `project.info` grouped four fields under one type — "`trackCount`, `clipCount`,
   `assetCount`, `durationNs` (integers)" — which hides the first three from any per-field reader.
   Each now carries its own `(integer)`.
3. Same shape in `media.import`: "`width` and `height` (integers)" hid `width`. Now
   "`width` (integer) and `height` (integer)".
4. `docs/REMOTE_ACCESS.md` carried a **stale status block** claiming `src/app/main.cpp` never calls
   `AppSettings::load()`/`fromArgv()` and that the binary therefore ignores all configuration. `7b08fb3`
   fixed exactly that; the block is now false and was replaced.
5. `docs/QUICKSTART.md` likewise still said `palmier-pro` "accepts no command-line arguments today".
   Also false since `7b08fb3`; rewritten, pointing at `REMOTE_ACCESS.md`.

Items 2 and 3 are the interesting ones: prose that reads perfectly to a human silently drops names
for a checker, so `docs/TOOLS.md` gained an **"Extraction contract for task 12.7"** section stating
the grammar (tool heading, `Argument` table, `Field` table or `Result` paragraph, one parenthesised
type per field, the conditional-presence phrases, `*(command result)*`), mirroring the contract
`docs/BUILD.md` already declares for its option region. Items 4 and 5 are prose claims no name-level
checker can see — worth recording as the known boundary of this check.

> **On the 1119 count:** the +8 over 1110/1111 is entirely 10.9. Verified with **three consecutive
> clean full runs** of `ctest --test-dir build-nogui -j$(nproc)`: 1119/1119, 3 skips
> (`ExportCoordinatorValidate.RejectsAnUnwritableParentDirectory`,
> `ExportHardwareSoftwareComparisonTest.HardwareAndSoftwareExports...`,
> `OfflineModeAvailability.ExportWithTheHostEncoder`). One earlier run showed
> `ProjectSessionSave.DeliversCompletionsOnlyWhenPumped` failing under the load of a full rebuild;
> it is a **pre-existing flake** in that threaded save-completion timing test, unrelated to this
> target (it passed in the pre-change baseline run, passes on every rerun, and passed in all three
> clean runs). **The native stack was lost again before this run** — FFmpeg, the Vulkan headers,
> shaderc, ALSA/PipeWire, libsecret and lcms2 were all missing. FFmpeg was recovered by re-running
> `make install` in the already-built `/projects/sandbox/.ffmpeg-src/ffmpeg-6.1.2` tree (no
> recompile); the rest came from `dnf`. Qt is still absent, so `build-ui` was not touched.

**Complete:** stages 0–8 in full. Stage 8 closed with 8.6 and 8.7: `PipeWireAudioSink` and
`AlsaAudioSink` land behind `PALMIER_ENABLE_PIPEWIRE` / `PALMIER_HAVE_PIPEWIRE` and
`PALMIER_ENABLE_ALSA` / `PALMIER_HAVE_ALSA`, `AudioSinkSelector` performs the startup
PipeWire → ALSA → Null selection (requesting a ≤512-frame quantum above 48 fps), and
`ApplicationComposition` now constructs the single `AudioEngine` behind
`audioEngine()` / `audioSinkName()` / `audioOutputAvailable()` / `audioUnavailableNotice()`, with
the sink offered to `PreviewController` as an optional injectable `AudioMasterClock` so a frame
more than one interval behind the audio position is dropped and one more than an interval ahead
waits. `.github/workflows/ci.yml` installs the two dev packages, and the configuration summary
reports each sink.

> ### ⚠️ Read this before verifying: this sandbox loses its native dependency stack
>
> This sandbox has **repeatedly lost its native dependency stack mid-session** — FFmpeg, shaderc
> and Qt 6 have each disappeared from `/usr/local` between agent runs. Symptoms and rules:
>
> - **A stale CMake cache will falsely report `PALMIER_HAVE_FFMPEG=1`** against headers that are no
>   longer on disk. The tree then either fails to compile or silently compiles the FFmpeg paths as
>   **stubs**, which *lowers the test count without failing anything*. A run that reports fewer
>   tests than the count below is almost certainly a stub build, not a regression.
> - **After any such loss, delete and reconfigure BOTH build trees from scratch.**
>   `cmake --build` on an existing tree is not sufficient — only a fresh configure re-runs the
>   `pkg-config` probes. Never trust an incremental build to tell you what is installed.
> - **"Qt 6 not found" is usually not a missing Qt.** `find_package(Qt6 ... QUIET)` also fails when
>   Qt6Gui's `WrapOpenGL` dependency is unmet, and `QUIET` hides the real reason. Verify with a
>   throwaway non-`QUIET` `find_package` before concluding Qt is absent. The fix is the OpenGL/GLX
>   development packages, not reinstalling Qt.
> - Restoration is slow, and **each restore step must finish inside a single shell invocation** —
>   background jobs and `/tmp` do not survive across calls in this sandbox.
>
> **The same instability also damaged the git object store.** A whole pack file was lost — the
> `.idx` and `.rev` remained while the `.pack` itself was gone, leaving `git count-objects -v`
> reporting `packs: 0` and 155 of the 419 objects reachable from `HEAD` missing. The working tree
> was complete throughout (which is why the suite builds and passes), so most objects were
> recoverable by re-hashing the working tree into the object store:
>
> ```
> git ls-files -z | xargs -0 -n 50 git hash-object -w --   # additive, non-destructive
> ```
>
> That brought the 155 missing objects down to 19. The commit recorded for tasks 9.3/9.4/10.1/10.2
> was verified self-contained before being written (`git rev-list --objects --missing=print` on the
> staged tree reported zero missing), so **the current `HEAD` tree is complete and checkout-able**.
> What remains lost is history *behind* that commit: three subtrees and the previous `HEAD` blobs of
> the four task-9.3 media files, plus the unrelated `feat/linux-port-gpu` ref. Practical
> consequences: `git show --stat HEAD` and `git diff HEAD~1` fail because they must read the parent
> tree, and **`git checkout`, `git reset --hard` and similar history-reading commands must be
> avoided** — the working tree is the source of truth. A future push may need
> `--no-thin`, or a fresh clone with this commit replayed on top.

**Stage 9 in progress:** 9.1–9.6 are done. 9.1 and 9.2 delivered
`media::EncoderSelector` with the `hardware(...)` / `software(codec, reason)` constructor pair (so
no selection can report both hardware use and software fallback), the 3000 ms probe deadline, the
single initialization retry before the documented software fallback, and
`tests/support/HardwareSkip.hpp` (`PALMIER_SKIP_WITHOUT_HW`), covered by Properties 40, 41 and 42
on `palmier_media_encoder_selector_tests`. 9.3 added the audio stream: `MediaEncoder` gained
`AudioEncodeSpec`, `EncodeSpec::audio`, `submitAudio()` and a two-stream `finish()` that flushes
both streams and writes the trailer, and `ExportEngine` gained the `AudioRangeRenderer` seam and
`ExportCancelPredicate`. 9.4 added `src/services/ExportCoordinator.{hpp,cpp}` — the pure static
`validate()` that runs before any file is created, one worker thread per export over a
**value-copy `Project` snapshot**, the `OutputGuard` scope guard that calls `finish()` best-effort
then removes the output path on any failure or cancellation, and progress marshalled to the main
thread. 9.5 added `tests/services/export_coordinator_property_test.cpp` — Properties 33, 34, 35, 36,
37 and 39 — as a **second source on the existing `palmier_services_export_coordinator_tests`
target**, so the unit tests of 9.4 and the properties of 9.5 link one binary. 9.6 appended
Property 38 (two successive exports are identical) to
`tests/media_export_ordering_property_test.cpp`, comparing frame count, per-frame presentation
timestamps and the bound encoder route across four containers, three codecs and both encoder
selections, from a **test-owned trace** rather than from anything the code under test reports about
itself. 9.7 wired export into the tool surface and the composition root:
`ApplicationComposition` now constructs the one `services::ExportCoordinator` and the one
`services::MediaImportService` of Requirement 1.1, exposes them through `exportCoordinator()` /
`mediaImportService()`, binds the `timeline.export` and `media.import` hooks to them on the SAME
shared `ToolRegistry` the GUI, the MCP endpoint and the in-app agent dispatch through, and gained
`codecBackendReport()` / `kCodecBackendReportBudget{3000}`; `AppConfig` gained `exportOptions`,
`exportToolOptions` and `mediaProbeBackend` as injection seams. 9.9 closed the stage-9 checkpoint on
a green suite in both trees. 9.8 added
`tests/services/export_hardware_software_comparison_test.cpp` — Requirement 8.6's comparison — on the
new `palmier_services_export_hw_sw_comparison_tests` target. **Stage 9 is now closed**, with the
caveat on 9.8 recorded below.

**Task 9.8 is written but SKIPS on this host, and its assertions are therefore UNVERIFIED.** The
test exports the 300-frame 1920×1080/30 fps fixture timeline twice through the same
`ExportCoordinator::begin()` API — once with `preferHardware = true`, once with `false` — and then
asserts on the two files: that each probes and decodes, that each yields exactly 300 decoded video
frames, and that the two container durations agree to within one frame interval (33 ms), with each
also matching the timeline duration so two equally wrong outputs cannot satisfy the comparison.
Everything on the encode and decode side is real — `media::ffmpegEncodeBackendFactory()`,
`media::probeMediaFile` and a real `media::MediaDecoder` — because a mocked encode backend cannot
produce a decodable output and so could not answer Requirement 8.6 at all. The one injected
collaborator is the clip-frame provider, which paints a deterministic per-position colour ramp; that
is what makes the pixel sequence provably identical across the two runs, and it avoids the circular
need for a 1080p input fixture that would itself have to be encoded first.
**It cannot run here for TWO independent reasons, and the recorded skip reason names both,
separately labelled** (Requirement 15.5):

```
Requirement 8.6's hardware-versus-software comparison cannot run on this host,
because neither half of the comparison is available:
  * no hardware encoder: no vendor hardware encode path is compiled in
    (PALMIER_HAVE_NVENC, PALMIER_HAVE_VAAPI and PALMIER_HAVE_QSV are all
    undefined), so H.264 hardware encode cannot be exercised on this build
  * no software encoder to compare against: libavcodec on this host carries no
    software H.264 encoder ("libx264"): opening a software encode route failed
    with "encoder not found: libx264", so a hardware encode has nothing to be
    compared against
The comparison needs BOTH, so it is reported as skipped rather than failed
(Requirement 15.5).
```

The hardware half is gated by task 9.1's `PALMIER_SKIP_WITHOUT_HW`, as the task specifies. The
software half is a *different* missing thing that macro says nothing about, so the test probes it
directly by asking the production encode backend factory to open a software route at the fixture's
geometry — the same call the export itself makes — and reports its own distinct reason.

**Because a test that only ever skips is indistinguishable from a broken one, the guard is itself
asserted** by a second case, `ExportHardwareSoftwareComparisonGuard.DistinguishesMissingHardwareFrom
MissingSoftware`, which runs on every host: driven with synthetic "NVENC compiled in and the device
reports H.264 encode" values it asserts the hardware gate produces **no** skip reason, which is what
proves the comparison body is reachable on real hardware rather than dead code; it also pins the
"no vendor path" and "no capable device" reasons to naming the defines and the device respectively,
and asserts the combined message keeps the two causes separately labelled. That case passes here.

**What is still owed on 9.8:** the comparison's own assertions — that both outputs decode, that both
frame counts equal 300, and that the durations agree within one frame interval — have **never been
executed**, because no host in this sandbox can encode H.264. They must be exercised on a machine
with a real vendor encoder *and* a software H.264 encoder before 9.8 counts as verified rather than
merely written. The stage-9 parent is ticked on the basis that every task is implemented and the
suite is green; 9.8's runtime behaviour on real hardware remains unproven.

**Tasks 9.7 and 10.8 were reviewed after the fact, because the two agents that wrote them lost
their reports before anything was verified or committed.** The review is recorded here because it
found a build failure and two coverage gaps that the code comments alone would not have revealed.

**9.7 did not compile.** `palmier_app_composition_tests` compiles `src/app/ApplicationComposition.cpp`
directly into the test binary rather than linking `Palmier::services`, and its source list was never
given the two translation units the task added calls into — so the target failed to link with
undefined references to `ExportCoordinator`'s constructor and destructor,
`MediaImportService`'s constructor and `import()`, and `makeExportToolHandler`. The fix is the
minimal one: `src/services/ExportCoordinator.cpp` and `src/services/MediaImportService.cpp` joined
that target's source list. Nothing in `src/` was changed. Because the link failed, **neither 9.7 nor
10.8 had ever been built or run** before this review.

**`timeline.export`'s published `ToolSchema` did change, and the change is deliberate rather than
silent.** Six optional arguments were added — `codec` (enum `h264` | `hevc` | `vp9`), `fps`
(1..120), `bitrateKbps` (100..200000), `includeAudio`, `preferHardware` and `overwrite` — because
Requirement 7.2 names six things a caller may ask for and three of them had no argument to arrive
in, while `preferHardware` and `overwrite` express rules Requirements 8.2 and 7.11 make the caller
responsible for. The byte-stability expectation in `tests/services/tool_registry_schema_test.cpp`
was updated in the same change, and a new test,
`ToolRegistrySchema.ExportPublishesTheRequirement72ArgumentsAndTheirRanges`, asserts each published
bound, the codec enum, that the required set is still exactly `{outputPath, format}`, that the
pre-9.7 argument object still validates, and that each new bound is enforced. Properties 45 and 46
needed no edit because `mcp_protocol_property_test.cpp`'s `validValue` derives values generically
from each `ArgSpec`'s `minInt` / `maxInt` / `minNum` / `maxNum` / `enumValues` / `Bool` kind, so the
additions are covered automatically. **One caveat worth carrying forward:** `width` and `height` are
a *narrowing*, not a widening — they published `minimum: 1` with no maximum and now publish
Requirement 7.1's 128..3840 and 128..2160, so `width: 1` was schema-valid before this task and is
refused now. The observable outcome is unchanged, because `ExportCoordinator::validate` always
rejected those geometries, but the refusal moved from the handler to schema validation and a client
therefore sees a different error for them. The in-source comment's blanket claim that "every
argument object that validated before task 9.7 still validates" is accurate only for objects that
omit `width`/`height` or supply in-range values.

> ### ⚠️ `codecBackendReport()` is implemented but **not asserted by any test**
>
> Requirement 8.7 is the one part of task 9.7 that verification does not reach.
> `codecBackendReport`, `CodecBackendStatus` and `kCodecBackendReportBudget` appear **only** in
> `src/app/ApplicationComposition.hpp` and `.cpp` — no test references any of them. So none of
> Requirement 8.7's four obligations is pinned: that all four backends (VAAPI, NVENC/NVDEC, QSV,
> FFmpeg software) are reported, that the software backend is *always* both compiled in and usable,
> that the answer arrives inside 3000 ms, and that the call changes no encoder selection or export
> state. The header comment even spells out what "a caller may assert" — the budget by measuring,
> the no-change half by comparing the coordinator's `running()`, `lastOutcome()`, `lastSelection()`
> and `deliveredProgress()` and the session's project and revision across the call — and no caller
> does any of it. Reading the implementation, it looks correct by construction: it is `const`, the
> order is fixed, the software entry is hard-coded to both true, and the vendor answers are read
> from `gpu::BridgeAvailability::fromBuildConfig()` and the already-probed `gpu::GpuCaps`, so no
> probe is started and there is nothing to wait on. **"Looks correct by construction" is not
> verification.** A test over the composed `ApplicationComposition` is still owed before
> Requirement 8.7 can be called covered.

**Property 68 (task 10.8) is non-vacuous, and the credential half of it is genuinely exercised.**
`performScan()` discovers its input with a `recursive_directory_iterator` over
`PALMIER_SOURCE_DIR / {src, tests, cmake}` — injected as a compile definition because ctest's
working directory is the build tree — not from any hard-coded file list, and it fails loudly rather
than silently passing when the tree cannot be walked, when an expected directory is missing, or when
the file set comes back empty; the property additionally asserts at least 100 files were enumerated.
The checker is proven able to *fail* by eight positive controls: a missing SPDX header, an SPDX
header below the leading comment block, the wrong licence, an opaque credential assignment, the
JSON and YAML spellings of one, an AWS access key id, a JWT and an opaque bearer header, and a PEM
private key with a body (distinguished from a bare marker). **The credential scan was not made to
pass by broad exclusions.** There is no file, directory or line exemption anywhere in it; every
exclusion is scoped to the matched *value* inside `placeholderReason()`, each clause is individually
pinned by `EveryPlaceholderClauseIsReachableAndNamed`, and a dedicated non-vacuity test asserts the
rule actually fired on the real tree rather than staying silent: **39 credential shapes were found
and every one was excused by a named value clause**, across three distinct clauses (26 shorter than
16 characters, 9 self-describing, 4 a repeated pattern). Two honest limits on that: the
self-describing word list contains `test`, `sample`, `stub` and `mock`, so a genuine secret that
happened to contain one of those substrings would be excused — it is the loosest clause and it
accounts for 9 of the 39 excusals — and the scan covers `src/`, `tests/` and `cmake/` only, so a
credential committed under `.github/workflows/` or `packaging/` would not be caught. Requirement
12.6 asks only for `src/` and `tests/`, so that is within scope, but it is a real blind spot.

**No real credential was found anywhere in the repository.** The checker reports zero defects across
the whole scanned tree, and an independent scan for AWS access key ids, JWT shapes, PEM private-key
bodies and tracked `.env` / `.pem` / `.key` / `id_rsa` files found nothing either. The only
AWS-shaped string in the tree is `AKIAIOSFODNN7EXAMPLE`, which is AWS's own published documentation
example and appears inside a comment in the checker itself explaining the rule; every PEM hit is a
bare `-----BEGIN PRIVATE KEY-----\n` marker with no key body, used as a TLS path fixture, which is
exactly the case `DetectsAPemPrivateKeyWithBodyButNotABareMarker` pins as *not* a defect.

> ### ⚠️ The task-9.9 checkpoint was ticked on a green suite, not on a Requirement 3.6 test
>
> **RESOLVED by task 12.10** — see the 12.10 entry in the Progress section below.
> `tests/e2e/editor_end_to_end_test.cpp` now performs the six-call chain through the Tool_Surface
> and probes and decodes the exported file, so 9.9's claim is finally backed by a test. The
> assessment below is retained as the record of what 9.9 actually did and did not establish.
>
> 9.9's note reads "the headless sequence of Requirement 3.6 now completes end to end", but **no
> test performs that sequence.** Requirement 3.6's six-call chain — `project.create`,
> `timeline.add_track`, `media.import`, `timeline.add_clip`, `project.save`, `timeline.export`,
> ending in "a file at the requested export path that the media engine can probe and decode" — is
> not covered anywhere in `tests/`. The closest is 9.7's new
> `ExportCoordinatorTest.ExportToolRunsThroughTheSharedToolRegistry`, which is a real advance: it
> drives `timeline.export` through the shared `ToolRegistry` via the production
> `makeExportToolHandler`, so the wiring the composition root performs is exercised. But it runs
> against a scripted synthetic encode backend, so the file it asserts into existence is not a
> decodable media file, and it exercises that one call rather than the chain. On this host the
> decode half is impossible anyway — see the encoder blockquote below — so closing this properly
> needs either an injected encode backend that emits a decodable container, or a host with a real
> encoder stack.

**Stage 10 in progress:** 10.1–10.4 are done. 10.1 added
`src/services/OfflineIntentInterpreter.{hpp,cpp}` (the 12 documented phrase patterns, matched
case-insensitively on the whitespace-trimmed utterance, no network request) and
`src/services/AgentInterpreterRegistry.{hpp,cpp}` (ids `offline` default, `hosted`, `byok`);
`makeUnconfiguredInterpreter()` was removed, and `AppConfig` / `AppSettings` /
`ApplicationComposition` gained `agentInterpreterId` and `startupErrors()`. 10.2 added
`tests/services/offline_interpreter_property_test.cpp` covering Properties 59, 60, 61 and 64 on
`palmier_services_agent_offline_tests`. 10.3 added
`tests/services/agent_equivalence_property_test.cpp` (Property 62) on the new
`palmier_services_agent_equivalence_tests` target: every documented phrase is driven once through
the agent path and once as a direct tool invocation, and the two resulting project states are
compared through a canonicalizing fingerprint that hides freshly generated identifier values and
nothing else, with the sabotage modes asserted to fail and roll back on **both** paths. 10.4 added
`tests/services/mention_resolver_property_test.cpp` (Property 63): a unique `@mention` is
substituted and submitted, while an unmatched or ambiguous one is refused with no tool invoked and
no project change. 10.8 added `tests/docs/repository_hygiene_property_test.cpp` (Property 68,
Requirement 12.6) on the new `palmier_docs_tests` target: every source file under `src/`, `tests/`
and `cmake/` is discovered from the filesystem and checked for the GPLv3 SPDX header in its leading
comment block and for credential literals, with `PALMIER_SOURCE_DIR` and `PALMIER_DOCS_DIR` injected
as compile definitions so the checker can find its input from the build tree. The target is
deliberately set up to be **extended** by task 12.3 with `target_sources()` rather than
re-declared — 12.3 must not add a second `add_executable()` with that name, and must not call
`palmier_register_test()` on it again.

**10.5 added the generative backend registry and its two HTTPS clients** — four new translation
units in `src/services`, all four compiled into `palmier_services` so the choice between them is a
configuration string and never a rebuild (Requirement 12.2):

| File | What it owns |
| --- | --- |
| `GenerativeBackendRegistry.{hpp,cpp}` | ids `offline` (default) / `hosted` / `byok`, `selectGenerativeBackend()`, and the offline stub itself |
| `GenerativeHttpTransport.{hpp,cpp}` | the injectable network seam plus the submit → poll → fetch → cancel wire protocol both clients share |
| `HostedGenerativeBackend.{hpp,cpp}` | the hosted service client: bearer scheme, `palmier/hosted/<user>/session` key |
| `ByokGenerativeBackend.{hpp,cpp}` | the BYOK provider client: `X-Api-Key` scheme, `palmier/byok/<user>/<provider>` key |

`GenerativeBackend` is a new interface — `IGenerativeBackend` plus `backendId()` and
`unmetPrecondition()` — and `unmetPrecondition()` is the mechanism behind Requirement 12.4. The
`generation.generate` hook in `ApplicationComposition` asks the **selected** backend that question
as its first act, before the coordinator's validate → gate → generate → place pipeline runs at all,
which is what makes "no media library entry, no clip and no undo-history entry" true by
construction rather than by inspection. Because the tool surface, the MCP endpoint and the in-app
agent all dispatch through the one shared `ToolRegistry`, that single hook is the whole of
Requirement 12.1's routing obligation, and `ApplicationCompositionTest.OfflineGenerationIsRejected
WithNoLibraryClipOrUndoEntry` measures the rejection at well under 1 ms against the 1 s bound.

`AppConfig` gained `generativeBackendId`, `generativeEndpoint`, `generativeTransport` and
`featureCredentials`; `ApplicationComposition` gained `generativeBackendId()` and
`generationUnmetPrecondition()`. The composition root's private `OfflineGenerativeBackend` stub was
**removed** in favour of the registry's named, tested one, exactly as 10.1 removed
`makeUnconfiguredInterpreter()`. The credential-presence probe is now written **once** and shared by
both registries — two copies could disagree, and then `hosted` would be selectable for the agent and
not for generation with no stated reason.

**Three things about 10.5 that a reader should not have to infer.**

1. **`featureCredentials` is a new seam, and it exists because `hosted`/`byok` were otherwise
   unreachable through the composition root.** The default probe reads the composed auth stack,
   which at *construction* time necessarily holds no session and no authorized provider — nobody has
   signed in yet — so a cold start always demotes to `offline`. That is correct for a cold start,
   and it is the same limitation task 10.1 shipped with for the agent interpreter, but it also means
   the selected-client path could never be entered or tested. `AppConfig::featureCredentials`
   overrides the probe for both registries, so a shell that restores a session before composing, and
   `ApplicationComposition.ASelectedHostedBackendIsInstalledWhenCredentialsArePresent`, can reach it.
2. **No endpoint credential value is checked in, and the endpoint default is empty rather than
   real.** Credentials are read from `SecretStore` on *every* call rather than captured at
   construction — a session that expires or a user who signs out must stop authorizing requests
   without the composition root being rebuilt, and a cached value would outlive the fact.
   `HostedGenerativeBackend.ReadsTheCredentialAtRequestTimeNotAtConstruction` pins that by filing a
   credential *after* the backend is built and then removing it again. A plaintext `http://`
   endpoint is refused at request-construction time rather than sent to, so a misconfiguration
   cannot put a credential on the wire in the clear.
3. **`offline` is genuinely usable, not inert (Requirement 12.5).** The stub holds no transport, no
   endpoint and no credential at all — a class with no route to the network cannot take one — and
   `ApplicationComposition.OfflineModeKeepsEveryNonGenerationOperationWorking` drives
   `timeline.add_track`, `timeline.read`, `project.info` and `media.list` plus the MCP endpoint's
   start/stop with the stub in force. Requirement 12.9's "never reach the network" and 12.3's
   single-undoable-edit property are tasks 10.6/10.7 and remain open.

**What the network seam does and does not cover.** It covers everything above the socket: request
construction (method, URL, headers, JSON body, and *where* the credential lands for each scheme),
credential loading, the HTTP-status → `ErrorCode` mapping for twelve statuses, malformed and
incomplete response bodies, provider-reported failure reasons, transport-error forwarding, and the
refusal paths (empty job id, unconfigured endpoint, plaintext endpoint) that must send nothing. It
also covers the *negative* claims: `tests/services/generative_backend_registry_test.cpp` interposes
the C library's `socket`, `connect`, `getaddrinfo` and `sendto` via `dlsym(RTLD_NEXT, …)` — the same
technique the offline interpreter suite uses — and asserts a zero count around the offline stub's
whole surface, with a companion case that opens a real socket to prove the interposers are live and
the zero is not vacuous. **It does not cover any real TLS handshake, certificate validation, DNS,
connection reset or wire timeout**, because this repository links no HTTP client library and the
hosted service is out of tree by Requirement 12.6. `makeUnavailableGenerativeHttpTransport()` is the
honest default: it reports `Unsupported` per request without contacting anything, so selecting
`hosted` on such a build yields a descriptive error rather than a link failure. Supplying a real
transport is a matter of implementing one interface, and nothing above it changes.

**10.7 is now done, so stage 10 stands at 10.1–10.8 done with only 10.9 remaining**, and the
stage-10 parent stays open. 10.7 extended
`tests/services/generative_atomicity_property_test.cpp` (existing target
`palmier_services_generative_atomicity_property_tests`, whose two earlier properties were left
untouched) with **Property 67: a failed or timed-out job leaves nothing behind** (Requirements 12.7,
12.10) — one new case, `GenerativeAtomicityProperties.AFailedOrTimedOutJobLeavesNothingBehind`. It
reuses 10.6's `GenerationRig` shape (tool surface → `McpToolExecutor` → `ToolRegistry` →
`GenerativeMediaCoordinator` → real gate/runner/client → 10.5's hosted backend over an injected
`GenerativeHttpTransport`), so the target now also compiles the registry, transport, hosted/BYOK
backends, tool surface and `.palmier` store sources. Three failure modes are injected through the
transport seam — a post-submission `failed` status, a 503 on the result fetch (the "retain no
partially retrieved media" half), and a job that never reaches a terminal status — and each is run
twice, once through the executor and once straight at the coordinator, because the executor's
invocation rollback would otherwise be indistinguishable from "nothing was ever applied". Each run
asserts no media-library entry (in the `MediaManager` **and** in `Project.assets`), no timeline clip,
no undo entry, an unchanged redo depth, and a byte-identical serialized project.
**The timeout carries no wall-clock wait**: the budget is generated across Requirement 12.10's
configurable 10–3600 s range and elapses on an injected `VirtualClock` that only the stalling
transport advances, so the whole case runs in about 50 ms, and the property asserts both that the
virtual clock crossed the budget and that real elapsed time did not. The
`GenerativeMediaCoordinator` place-before-import ordering fix is at HEAD, so the property passes as
written; a deliberate mutation of one of its assertions was confirmed to fail the test, so the
zero-change assertions are not vacuous.

**10.6 was done before it, so stage 10 stood at 10.1–10.6 and 10.8 done, with 10.7 and 10.9 remaining**,
and the stage-10 parent stays open. 10.6 added
`tests/services/generative_lifecycle_property_test.cpp` on the new
`palmier_services_generative_lifecycle_tests` target: Property 65 (a successful generation is one
undoable edit, Requirement 12.3) and Property 66 (invalid generation requests never reach the
network, Requirement 12.9), plus two non-vacuity cases —
`GenerativeLifecycleNetworkSeam.TheInterposersObserveARealSocketCall`, which arms the
`dlsym(RTLD_NEXT, …)` `socket`/`connect`/`getaddrinfo`/`sendto` counters and opens a real socket so
the zero Property 66 asserts cannot be vacuous, and
`GenerativeLifecycleNetworkSeam.TheGenerateToolIsTheOneRoutedEntryPoint`. The target assembles the
whole `generation.generate` path out of product sources — `McpToolExecutor`'s schema validation and
rollback policy, the shared `ToolRegistry`'s declared schema, `GenerativeMediaCoordinator` over the
real `AuthServiceGenerationGate` and `GenerativeClientRunner`, and 10.5's backend registry with its
hosted HTTPS client — with the injected `GenerativeHttpTransport` as the only route to a socket, and
`ProjectSession` supplying the one `TimelineEngine` and `MediaManager` so Property 65 can compare the
post-undo project to the pre-generation one as a serialized document rather than by spot checks.

**Two product gaps were found while writing 10.6, and both are source defects rather than test
problems. Neither is fixed.** They are the most valuable output of this round and must not be lost.

1. **`src/app/main.cpp` never wires `AppSettings`.** It constructs `ApplicationComposition` with a
   default-constructed `AppConfig` and never calls `AppSettings::load()` or
   `AppSettings::fromArgv()`. So the shipped `palmier-pro` executable **ignores the configuration
   file, the environment and the command line entirely**, and remote access cannot be enabled
   through the executable at all. Requirement 16.3's configuration surface is implemented and
   enforced — and tested at the `AppSettings` / `ApplicationComposition` level — but **unreachable
   from the binary a user actually runs.** Every `AppConfig` seam the tests drive (including
   `featureCredentials`, `generativeBackendId` and the remote-access fields) is therefore
   test-only in practice. The fix is in `main.cpp`, not in the settings layer.

2. **Requirement 12.9 is not fully satisfied for an unknown `trackId`.** A syntactically valid but
   **absent** track identifier passes the declared schema, and
   `GenerativeMediaCoordinator::generateAndPlace` **runs the generation and calls
   `library_.importAsset()` before `TimelineEnginePlacer::place` ever looks the track up**. So such
   a request *does* reach the network, and it leaves a media-library entry behind that the
   executor's rollback does not remove — rollback undoes engine commands only, and the import is
   not one. Property 66 **documents this as an explicit exclusion rather than asserting it**, which
   is why the property passes; the exclusion is the honest record of a real gap, not a weakened
   test. Closing it needs a source change moving track-existence validation ahead of job
   submission, after which Property 66's exclusion should be removed and the case asserted.

**Stage 12 is now partly started, and 12.6 is half done — it stays `[~]` and must not be ticked.**
Three of its six documents are checked in: `docs/BUILD.md` (the clean-checkout
configure/build/test/launch sequence, the per-distribution native package names, every `PALMIER_*`
option with default and effect, and the minimum host specification), `docs/MCP_CLIENTS.md` (the
Claude Code, Codex and Cursor client entries at `127.0.0.1:19789` and the Requirement 3.1 tool-list
confirmation) and `docs/REMOTE_ACCESS.md` (bind address, ≥32-character token generation, the
acknowledgement flag, TLS material and the tunnel alternative, the Origin allow-list, ≤32 maximum
sessions, the ≤3600 s idle timeout and the unencrypted-traffic warning). **Still outstanding on
12.6: `docs/TOOLS.md`, `docs/HARDWARE_ENCODE.md`, `docs/QUICKSTART.md`, and the `README.md` trim**
to an overview plus links into `docs/` so each checked name lives in exactly one file. Note that
task 12.7's consistency checker reads `docs/BUILD.md` and `docs/TOOLS.md`, so the missing
`TOOLS.md` blocks half of 12.7 as well. The stage-12 parent stays open; 12.1 and 12.2 are done,
12.6 and 12.7 are partial, and the rest of stage 12 is untouched. Stage 11 is untouched.

**Tasks 12.1 and 12.2 are done: `docs/UPSTREAM_PARITY.md` and `docs/PORT_BACKLOG.md` are checked in.**
The parity report carries the 22 tool-category rows of Requirement 13.1 and the 12 capability-area
rows of Requirement 13.2 — both name sets verified identical to the requirement text, in the same
order — with a build-order projection holding exactly the 31 `absent` and `partial` entries
(2 `must`, 19 `should`, 10 `later`), and `multicam` deliberately appearing in both tables as two
distinct entries identified by the (table, name) pair. The backlog carries the ten Requirement 14.2
changes with unique identifiers, and a `given`/`when`/`then` `check:` block on each of the nine
`port`/`adapt` entries and on none of the `not-applicable` one (PR 401), per Requirement 14.3. Both
documents were audited mechanically against every one of those rules before being committed and **no
defect was found**, which matters because task 12.3's `ReportParser` will re-check them in the suite.

> ### ⚠️ Follow-up owed on the parity report: the two generation rows are now stale
>
> `docs/UPSTREAM_PARITY.md` scores **`generate` (tool category)** and **`generation and upscaling`
> (capability area)** as `absent`, and both scores were correct *when written* but are **not correct
> now**. The report's `linux-ref` is `16274d51b77ac9ffcf8db49592d2a90411610ab5`, the commit before
> task 10.5; at that ref the only installed generative backend was the private offline stub in
> `ApplicationComposition.cpp`, which refused `submit`, `poll` and `fetchResult`, so no generation
> operation was reachable and `absent` was the honest reading. Task 10.5 was uncommitted
> working-tree state at the time and was deliberately excluded for that reason — the report's own
> "Known limits" section already predicts this exact re-scoring.
>
> **10.5 has now landed as `f0a7925`, so both rows should move from `absent` to `partial`:** the
> request path and backend selection are reachable — `services::GenerativeBackendRegistry`,
> `selectGenerativeBackend()`, the hosted and BYOK HTTPS clients and the `generation.generate` hook
> that asks the selected backend for `unmetPrecondition()` — while the model catalog (PR 406), the
> upscale mode (PR 396) and audio generation (PR 395) all remain absent and the **offline default
> still completes no generation**. Because at least one but not every operation is reachable, that
> is precisely the report's own definition of `partial`.
>
> **`linux-ref` in the document is therefore stale** and must be advanced when the rows are
> re-scored, along with the `linux-components` and `rationale` cells of both rows, the two rows'
> entries in the build-order projection (their priority stays `should`, so their position does not
> move, but the status counts in the prose beneath the list — currently 3 `present` / 11 `partial` /
> 20 `absent` — become 3 / 13 / 18), and the "Known limits" bullet that describes the pending
> re-score. The documents were **not** edited when 12.1/12.2 were committed; this is recorded as
> owed work rather than done work.

**One source fix came out of task 10.4: `MentionResolver`'s two refusal messages now state the
number of matching assets.** Requirement 11.7 asks for "an error that names the mention text **and
the number of matching assets**", and the previous wording named only the mention — the count was at
best implied by the length of the candidate list, and on the unmatched branch was not conveyed at
all. `toPreprocessorResult` now writes `"… was not found: @name (0 matching assets in the project
media library)"` and `"… matches more than one media item (N matching assets); please select one:
…"`, with `N` taken from `resolution.candidates.size()`. The change is **additive**: the status
classification, both error codes (`NotFound`, `FailedPrecondition`) and the candidate list are
untouched, and the substrings the pre-existing `tests/services/mention_resolver_test.cpp` asserts on
("not found", "select") were deliberately preserved, so no existing assertion had to be edited.

**Three new tools were added by task 10.1, and why.** Four of the twelve phrases mandated by
Requirement 11.1 had no tool to map to, and Property 60 requires every interpreter output to be
executable — so an interpreter that emitted an unroutable invocation could not have been correct.
Rather than drop the mandated phrases, the missing tool surface was added:

| New tool | Backing command | Phrases it makes executable |
| --- | --- | --- |
| `timeline.set_track_muted` | new `core::SetTrackMutedCommand` | `mute track N`, `unmute track N` |
| `edit.undo` | existing history stack | `undo` |
| `edit.redo` | existing history stack | `redo` |

`core::SetTrackMutedCommand` is a new undoable edit command in `src/core/EditCommands.{hpp,cpp}`;
`edit.undo` and `edit.redo` expose the already-existing history stack through the tool surface and
add no new edit semantics.

**Test count (authoritative): 1111 registered tests, `100% tests passed, 0 tests failed out of 1111`
in `build-nogui`, about 18 seconds of wall clock.** The two expected skips below are unchanged.
**The +1 over 1110 is task 10.7**: Property 67 on the existing
`palmier_services_generative_atomicity_property_tests` target (which now runs 3 cases).
The preceding **+4 over 1106 is task 10.6**: Properties 65 and 66 plus the two
`GenerativeLifecycleNetworkSeam` non-vacuity cases, all four on the new
`palmier_services_generative_lifecycle_tests` target.

> ### ⚠️ 1111 was verified in `build-nogui` only — `build-ui` could not be configured this round
>
> **Qt 6.8.3 was wiped from this host again and was deliberately not reinstalled**, so the
> `build-ui` tree could not be configured and the count was **not** cross-checked there. This is a
> genuine absence and not the `WrapOpenGL` false negative the warning above describes: `qmake6` is
> not on `PATH` **and** `/usr/local/lib/cmake/Qt6` does not exist, and a from-scratch configure
> stops at `cannot configure — missing required dependencies: Qt 6 (>= 6.2, components: Core Gui
> Quick Qml Widgets)`. FFmpeg survived (libavcodec **60.31.102** under `/usr/local`, restored from
> `/projects/sandbox/.ffmpeg-src/ffmpeg-6.1.2` by `make install` + `ldconfig`), and the
> `build-nogui` cache was confirmed to postdate that restore and to point at files that are really
> on disk, so the 1111 is a real build and not a stub one. **`build-ui` must be reconfigured and the
> count re-confirmed there once Qt is restored**, per the Qt steps in the restore recipe below.
> Because every test target is built in both trees, the expected `build-ui` count is also 1111.
> The 10.7 verification run found the same state: FFmpeg present under `/usr/local`, Qt still absent.

The preceding **+34 over the previous 1072 was task 10.5**: 27 on the new
`palmier_services_generative_backend_tests` target (the id set, the four selection outcomes, the
offline stub's rejection code / message / timing / zero-socket proof, request construction and
credential placement for both schemes, the read-at-request-time credential policy, the BYOK-vs-
`ByokCredentialManager` key-derivation agreement, a full submit → poll → fetch lifecycle over the
seam, the twelve-status error mapping, malformed-body handling, transport-error forwarding, and the
cancel semantics) and 7 added to `palmier_app_composition_tests` (the offline default, the
unknown-id fallback that still composes and still starts the endpoint, the credential-less `hosted`
demotion, the Requirement 12.4 rejection with no library/clip/undo entry, the Requirement 12.5
offline-availability sweep, the injected-backend override, and the selected-`hosted` path). The new
tests were run **five times in each tree** with no variation: 47/47 in `build-nogui` and 61/61 in
`build-ui` (the latter also sweeping the `RepositoryHygiene*` cases, because 10.5 adds four source
files that Property 68 must accept), five runs each, all green.

The preceding **+23 over 1049** was tasks 9.7, 10.8 and 9.8. **2 from 9.8** (the
Requirement 8.6 comparison, which skips here, plus the guard case that proves the gate is
conditional). **7 from 9.7**
(`ExportToolSchema.MatchesTheCoordinatorRanges`, five `ExportCoordinatorTest` cases covering the
tool's reported fields, its project-derived defaults, its agreement with the dialog for the same
request, its forwarding of admission rejections without creating a file, and its route through the
shared registry, plus
`ToolRegistrySchema.ExportPublishesTheRequirement72ArgumentsAndTheirRanges`) and **14 from 10.8**
(Property 68 itself plus its 13 `RepositoryHygieneChecker` controls). Both trees were **deleted and
configured from scratch** for this run, after a full dependency restore, so no stale cache could
misreport a dependency.

The two trees report the **same** count because every test target is built in both — `build-ui`
adds only the `palmier-pro` executable (`build-ui/bin/palmier-pro`), which registers no ctest test.
The difference is therefore zero, and any divergence between the trees should be read as a build
configuration problem rather than a UI-only test.

**Two** tests are reported as `Skipped` rather than run, in both trees, and both skips are expected:

1. `ExportCoordinatorValidate.RejectsAnUnwritableParentDirectory` — the test's own pre-existing
   `GTEST_SKIP()`, taken because the suite runs as **root**, and root bypasses the directory
   permission bits the case needs in order to make a parent unwritable. It is a property of the
   sandbox user, not of the code, and the test was left untouched.
2. `ExportHardwareSoftwareComparisonTest.HardwareAndSoftwareExportsOfTheFixtureAgreeOnFrameCountAndDuration`
   — task 9.8, skipped because this host has neither a vendor hardware encoder nor a software H.264
   encoder to compare against. Requirement 15.5 requires exactly this: skipped with a recorded
   reason naming what is absent, never failed. See the 9.8 paragraph above for the full reason text
   and for what remains unverified.

Earlier runs in this session reported **975** and **1029** from two concurrent agents. Of those two,
**1029 was the correct number** (it is now 1110, after 9.5, 9.6, 10.3, 10.4, 9.7, 10.8, 10.5 and
10.6); 975 was a tree in
which the FFmpeg paths had compiled as stubs against a stale cache, as described in the warning
above. The lesson stands: a count *below* the authoritative number is a stub build, not a
regression, and calls for a from-scratch reconfigure rather than acceptance.

> **This host's `libavcodec` has no H.264, HEVC or VP9 *encoder*, which blocks task 9.8.**
> The from-source FFmpeg 6.1.x under `/usr/local` (libavcodec 60.31.102) was built without
> `libx264`, `libx265`, `libvpx-vp9` and `libaom-av1`, and none of the vendor hardware encoders
> (`h264_vaapi`, `h264_nvenc`, QSV) are present either. Probing the library directly confirms it:
> `avcodec_find_encoder_by_name` returns null for all four external encoders, and
> `avcodec_find_encoder` returns null for `AV_CODEC_ID_H264`, `AV_CODEC_ID_HEVC` and
> `AV_CODEC_ID_VP9` — only built-in encoders such as `mpeg4` are available. Consequences to carry
> forward:
>
> - **No real H.264/HEVC/VP9 bytes can be produced on this sandbox.** Tests that need a coded
>   elementary stream in those codecs must assert on the encoder *selection* and on
>   test-owned traces (as Properties 38 and 33–37 do) rather than on decoded output.
> - **Task 9.8's hardware-versus-software output comparison cannot run here.** It needs the same
>   fixture encoded twice, once through a hardware encoder and once through software, and this host
>   has neither a hardware encoder nor a software H.264/HEVC/VP9 encoder to compare against. The
>   `PALMIER_SKIP_WITHOUT_HW` guard from task 9.1 is the right mechanism, and the test will skip
>   rather than fail here; it must be exercised on a host with a real encoder stack before 9.8 can
>   be considered verified rather than merely written.

**Property 19 flake fixed — root cause was the test's generator, not `ProjectSession`.**
`ProjectSessionPersistenceProperties.UnmodifiedAfterSaveUntilTheNextToolAppliedEdit` (task 2.3)
failed on a minority of seeds. Replaying the reported seed with shrinking disabled showed the real
failing assertion is the *pre-save* edit, `RC_ASSERT(...apply(before.command).changed())`, with the
engine reporting `clips must be ordered by timelineStart` — **not** anything about `modified()`,
the save thread or the revision guard. The generator draws tracks that legally overlap (an incoming
clip may overlap its predecessor by up to its own transition region), and `drawEditInvocation`
then offered `timeline.split_clip` at the target clip's midpoint. When the following clip starts
before that midpoint, `SplitClipCommand` inserts the right half after the left one and the track
would no longer be ordered by `timelineStart`, so `TimelineEngine` correctly refuses the command
and rolls back — the property, which asserts the drawn invocation succeeds, then failed. The
`std::jthread` save of task 2.2 was ruled out: the property observes completion deterministically
through `awaitSaveCompletions()`, which waits on the in-flight counter that each worker decrements
under the same mutex that publishes its completion record, so no ordering is left to chance.
The fix constrains the generator to invocations that are legal for the drawn project — the split
point is capped at the next clip's start, and `delete_clip` / `move_clip` only target clips whose
removal cannot leave the predecessor overlapping the successor beyond its transition region. No
property was weakened, no bound loosened, no sleep, retry or skip added, and `max_success` is
unchanged. The product behaviour the diagnosis relies on is now pinned by a new unit test,
`SplitClipCommand.SplitPastAnOverlappingSuccessorIsRejectedAndRolledBack` in
`tests/core/edit_commands_test.cpp` (the +1 in the test count).
Verification after the fix: the reported seed passes; the single property passed **30/30**
consecutive `ctest` runs (`--repeat until-fail:30`, each with a fresh random seed); ten independent
runs at `RC_PARAMS=max_success=1000` passed (about 10 000 generated cases); and the whole
`palmier_services_project_session_tests` file at `max_success=1000` passed.

**Earlier flake check on `PresentationRateWithinBoundsAndDropsUnderFivePercent` (Property 21):**
not reproducible, and still passing here. It was run 15 times in isolation and three times at
`max_success=1000` (roughly 5 000 generated cases across independent seeds), all passing; the test
is driven entirely by the injected `ManualClock`, so its outcome is a pure function of the
RapidCheck seed and the earlier failures were on pre-convergence revisions of the test. No bound
was loosened.

**Verification configuration.** Both trees configure with
`Palmier Pro Linux: all required dependencies located` and the following found: FFmpeg
(libavcodec 60.31.102, a from-source 6.1.x install under `/usr/local`), shaderc 2023.8.1,
lcms2 2.19, libsecret-1 0.21.4, OpenSSL 3.5.5, libpipewire-0.3 1.2.7, ALSA 1.2.7.2,
Vulkan (`/lib64/libvulkan.so`) and — for `build-ui` — Qt 6.8.3 at
`/usr/local/lib/cmake/Qt6`. The three vendor hardware codec paths (VAAPI, NVENC, QSV) are
`disabled (SDK not found)`; that is expected on this host and encoding falls back to software.
**As of the 10.6 verification run the Qt 6.8.3 half of that configuration is no longer present**
(see the `build-ui` blockquote above); everything else in the list was re-confirmed.

**The stack was lost again before the 9.7/10.8 verification run, and this time `/usr/local` was
completely empty** — no FFmpeg, no Qt, no shaderc. Both cheap gates failed up front
(`pkg-config --exists libavcodec` reported missing, `qmake6` was not on `PATH`), which is the
fastest way to detect it: check those two before trusting any build tree. The full restore below
was performed and reproduced the documented configuration exactly, including libavcodec
**60.31.102** — FFmpeg **6.1.2** configured
`--enable-shared --disable-static --disable-doc --disable-programs --disable-debug`, i.e. with no
`libx264`, `libx265`, `libvpx` or `libaom`, which is what keeps the encoder blockquote above true.
`xz` and `nasm` had to be installed before FFmpeg would unpack and build; they are not in the base
image. Both build trees were then deleted and configured from scratch.

Restoring this host from scratch takes these steps, in order:

```
# FFmpeg 6.1.x from source into /usr/local (discovered via pkg-config).
# xz and nasm first — the base image has neither, and without xz the tarball
# will not even unpack:
dnf install -y xz nasm
curl -sSLO https://ffmpeg.org/releases/ffmpeg-6.1.2.tar.xz && tar xf ffmpeg-6.1.2.tar.xz
cd ffmpeg-6.1.2 && ./configure --prefix=/usr/local --enable-shared --disable-static \
    --disable-doc --disable-programs --disable-debug && make -j"$(nproc)" && make install
# shaderc / lcms2 / libsecret / Vulkan come from dnf and match the versions above:
dnf install -y libshaderc-devel lcms2-devel libsecret-devel vulkan-loader-devel vulkan-headers
# Qt 6.8.3:
pip install aqtinstall
aqt install-qt linux desktop 6.8.3 linux_gcc_64 -m qtshadertools -O /opt/Qt
cp -a /opt/Qt/6.8.3/gcc_64/. /usr/local/
echo /usr/local/lib > /etc/ld.so.conf.d/zz-usr-local.conf && ldconfig
# dev packages:
dnf install -y alsa-lib-devel pipewire-devel \
    mesa-libGL-devel mesa-libEGL-devel libglvnd-devel \
    libxkbcommon-devel libxkbcommon-x11-devel libX11-devel libxcb-devel \
    xcb-util-devel xcb-util-image-devel xcb-util-keysyms-devel \
    xcb-util-renderutil-devel xcb-util-wm-devel xcb-util-cursor-devel \
    libXrender-devel libXi-devel mesa-dri-drivers xorg-x11-server-Xvfb
```

**Shortcut for the FFmpeg half:** an already-configured, already-built FFmpeg 6.1.2 tree survives at
`/projects/sandbox/.ffmpeg-src/ffmpeg-6.1.2` (outside the repository, so it is not affected by
`/usr/local` being wiped). When only FFmpeg is missing, `make install` in that directory followed by
`ldconfig` restores libavcodec 60.31.102 in seconds and skips the download, `dnf install xz nasm`,
`./configure` and the full compile. **Then verify with
`PKG_CONFIG_PATH=/usr/local/lib/pkgconfig pkg-config --exists libavcodec` and delete/reconfigure any
build tree whose `CMakeCache.txt` predates the restore** — a cache that predates it will report
`FFMPEG_FOUND=1` against files that were absent when it was written. Checking that the cache's mtime
postdates `/usr/local/lib/libavcodec.so` is a quick way to tell whether a full reconfigure is needed.

The `ldconfig` line is required, otherwise the Qt libraries under `/usr/local/lib` are installed but
not loadable at runtime. The **OpenGL/GLX packages are required for `find_package(Qt6)` to succeed
at all**, because Qt6Gui depends on `WrapOpenGL` — without them Qt reports as missing even when it
is fully installed.

- `build-nogui/` (`-DPALMIER_BUILD_UI=OFF`) remains the primary verification tree and covers the
  headless surface of core, gpu, media, services and app composition.
- `build-ui/` (`-DPALMIER_BUILD_UI=ON`) builds `src/ui` and the `palmier-pro` executable, and runs
  the same suite under `xvfb-run -a`. This keeps stage 11 unblocked.

`PKG_CONFIG_PATH=/usr/local/lib/pkgconfig` is required when configuring either tree, because
FFmpeg is a from-source install under `/usr/local` and is discovered through `pkg-config`.

```
rm -rf build-nogui build-ui        # after any dependency loss, never reuse the cache
PKG_CONFIG_PATH=/usr/local/lib/pkgconfig cmake -S . -B build-nogui -DPALMIER_BUILD_UI=OFF
PKG_CONFIG_PATH=/usr/local/lib/pkgconfig cmake -S . -B build-ui   -DPALMIER_BUILD_UI=ON
cmake --build build-nogui -j$(nproc) && ctest --test-dir build-nogui --output-on-failure
cmake --build build-ui -j$(nproc) && xvfb-run -a ctest --test-dir build-ui --output-on-failure
```

`build-nogui/`, `build-ui/`, `build/` and any other `build-*/` directory are local artifacts and
are not committed.

**One requirement conflict resolved in task 6.4:** the loopback fallback of Requirements 10.3 and
10.12 binds `127.0.0.1:19789`, not the configured port. `RemoteAccessGate::computeDecision` was
carrying the configured port through the fallback; the requirement text and design Property 52 both
name 19789, so the implementation was corrected. A configuration that never enabled remote access
still honours its configured port (Requirements 10.1, 16.3) — that is a different antecedent, and
both branches are asserted by Property 52.


---

**12.10 — the end-to-end test and the fixture generator are done, and Requirement 3.6 is now
genuinely covered end to end.** Four new files:

| File | What it is |
| --- | --- |
| `tests/e2e/editor_end_to_end_test.cpp` | the end-to-end test, on the new `palmier_e2e_tests` target |
| `tests/e2e/fixture_generator_main.cpp` | the build-time fixture generator, `palmier_e2e_fixture_generator` |
| `tests/support/SyntheticMedia.{hpp,cpp}` | a libav*-backed writer of REAL, decodable bytes, shared by the generator and by the injected export encode backend |

The subject is the assembled `app::ApplicationComposition` — its one `ProjectSession`, one
`ToolRegistry`, one `McpToolExecutor`, one `MediaImportService`, one `ExportCoordinator`, one
`AudioEngine` and one `ui::PreviewController`. Every step is an `executeTool` call by published tool
name; no handler, session or `Project` is hand-built in the test. The chain performed is
Requirement 3.6's order with task 12.10's extra steps woven in: `project.create` (1920x1080 / 30
fps) → `timeline.add_track` (video) → `media.import` of the generated fixture through the
**production** `media::ffmpegProbeBackend()` → `timeline.add_track` (audio) → `timeline.add_clip`
×2 → 30 consecutively presented playback frames, decoded from the fixture by the composition's own
`DecoderClipFrameProvider` over the production FFmpeg decode backend and composited by its own
`gpu::Compositor` → `project.save` → `project.open` → `timeline.export`.

**Requirement 3.6 is closed, and this corrects the task-9.9 overstatement.** 9.9's note claimed "the
headless sequence of Requirement 3.6 now completes end to end" while no test performed that
sequence; that claim is now true, and it is true because of this task rather than because of 9.9.
The clause that made 3.6 a requirement — "a file at the requested export path that the media engine
can probe and decode" — is asserted by reading the export output back through the product's own
entry points, `media::probeMediaFile` **and** `media::MediaDecoder::open`: the probe must report a
container, a decodable stream and a video stream at the requested export resolution; the decoder
must then yield exactly the planned frame count without an error before end of stream; and the
probed duration must equal the timeline duration to within one frame interval (Requirement 15.1).

**Fixtures are generated, never checked in** (task 12.10; Requirement 15.9).
`palmier_e2e_fixture_generator` runs from an `add_custom_command` and writes two files into
`<build>/tests/fixtures/`: `e2e_source_2s.mov` (exactly 2 s / 60 frames at 30 fps, one video and one
audio stream) and `e2e_reference.palmier` (written through the product serializer, with fixed UUID
literals). Every pixel and sample is a function of the frame index alone and no identifier or
timestamp is generated, so re-running the generator reproduces the media file **byte for byte**
(verified: identical MD5 across runs) and the document byte for byte for a given output directory —
the document embeds the absolute source path, which is the only thing in it that varies with where
it was generated. The build tree, not the source tree, is the output directory, so a build never
dirties the repository. Every
fixture access goes through a helper that FAILS with the fixture's full path — never skips — when it
is absent, unreadable, empty or not a regular file, and `EditorEndToEndFixtures` asserts both that
the generated source really probes as one-video-plus-one-audio media and that the reference document
really loads through `project.open`.

**On this host the export step of the production path skips, and the chain still runs.**
`timeline.export` accepts only H.264, HEVC and VP9 (Requirement 8.2), and this sandbox's libavcodec
6.1.2 carries no `libx264` / `libx265` / `libvpx-vp9` — only `h264_v4l2m2m` / `hevc_v4l2m2m` /
`*_vulkan`, none of which has a device here. So:

- `ChainThroughTheHostEncoder` leaves `AppConfig::exportOptions` fully default and therefore encodes
  through the production `media::ffmpegEncodeBackendFactory()`. It probes the availability of a
  software encode route the same way task 9.8 does and, finding none, reports itself **skipped**
  with a reason naming `libx264` (Requirement 15.5's idiom, as tasks 9.8 and 10.9 use it). Its
  assertions are therefore UNVERIFIED here and must be exercised on a host with a real encoder
  stack. It is the fourth expected skip in `build-nogui`.
- `ChainThroughAnInjectedEncodeBackend` runs the identical chain with exactly one substitution —
  `exportOptions.encodeFactory` — and **passes on this host**. The injected backend honours the
  export's own resolution, frame rate, container and audio configuration and substitutes only the
  codec, muxing the frames and audio blocks the export really submitted into a real container
  through an encoder the host really has (ProRes video + AAC audio in `mov` here). So the sequence
  and the probe-and-decode assertion run on every host, encoder or not.

`tests/support/SyntheticMedia.cpp` talks to libav* directly rather than reusing
`media::MediaEncoder` because the production backend hard-codes `AV_PIX_FMT_YUV420P` and takes its
encoder name from `gpu::softwareEncoderName()`; a fixture writer has to negotiate the pixel format
from the encoder (ProRes needs 10-bit 4:2:2) and the encoder from a candidate list. Everything else
about it mirrors `media/MediaEncoder.cpp`'s backend: the same RGBA→pix_fmt swscale conversion, the
same interleaved-float FIFO feeding a fixed-frame-size audio encoder, the same
send/receive/rescale/interleaved-write loop and the same flush-both-streams-then-trailer finish.

**Nothing in the new target sleeps, waits on wall time, or needs Qt, a display, a GPU, a vendor SDK,
a sound device or a network.** Playback is paced by a clock the test advances through
`ui::PreviewController::setAudioMasterClock` — the same public seam the composition root uses to
slew video to audio (Requirement 6.3) — because with the null audio sink the composed clock yields
nothing and pacing would otherwise fall back to the wall clock. Each pump advances that clock by one
frame interval, which presents one frame and drops none, so "at least 24 CONSECUTIVE frames" is
asserted as 30 presented and 0 dropped rather than raced for. Every file written is an absolute path
inside a per-process temp directory, and the MCP server is never started.

Suite: **1154 tests, 0 failures, 4 skips** in `build-nogui` (the three pre-existing skips —
`ExportCoordinatorValidate.RejectsAnUnwritableParentDirectory`,
`ExportHardwareSoftwareComparisonTest.HardwareAndSoftware…`,
`OfflineModeAvailability.ExportWithTheHostEncoder` — plus
`EditorEndToEndTest.ChainThroughTheHostEncoder`). No existing test was weakened, skipped or removed.

> ### ⚠️ Finding: `media.import` never registers the asset in `Project.assets`, so a document saved
> after an import cannot be re-opened
>
> This is what the end-to-end test found, and it is the one leg of Requirement 15.1 that does **not**
> pass. `project.open` of the document the chain itself just saved fails with:
>
> ```
> NotFound: could not open project '<...>.palmier':
>   Clip <id>: assetRef does not resolve to any entry in Project.assets
> ```
>
> `MediaImportService::import` registers the asset in `ProjectSession::mediaLibrary()` — the
> in-memory `core::MediaManager` view — and never appends it to `Project.assets`, which is the table
> `ProjectStore` serializes and the table `ProjectSession::openProject` rebuilds the library from
> (`libraryFor`). `core::AddClipCommand`, which `timeline.add_clip` drives, does not add it either.
> The generative path already solves exactly this problem: `PlaceGeneratedClipCommand` in
> `GenerativeMediaCoordinator.cpp` registers its asset in `project.assets` as part of the same
> command, precisely "so the clip's assetRef resolves (project validation rule)". The import + edit
> path has no equivalent, so the tool surface can be driven into a project state that
> `core::validateProject` rejects — which also means the `.palmier` round-trip of Requirement 3.5 is
> not reachable for any project built by importing media through the tools.
>
> **Requirement 3.6 is unaffected**: its chain stops at `project.save` and `timeline.export`, and
> both succeed — the save writes a document and the export produces a probeable, decodable file.
> Only Requirement 15.1's "re-opens the saved document" leg is blocked.
>
> The end-to-end test therefore performs the `project.open` call, records its outcome and PRINTS the
> failure, but does not assert on it, so the defect is visible in every run rather than hidden by an
> omitted step or enshrined as expected behaviour. Fixing it means changing stage-4 import/edit code
> (the natural fix being for the import or the add-clip command to register the asset in
> `project.assets`, mirroring `PlaceGeneratedClipCommand`), which is outside task 12.10's scope and
> is left for a decision. Once it is fixed, the four `seen.reopened` assertions already present in
> `expectChainSucceeded` become live with no further change.


### ✅ Defect fix — the finding above is fixed: `AddClipCommand` registers the clip's asset in `Project.assets` (no task ticked)

`Project.assets` is the only asset table a saved document carries: `ProjectStore` serializes it and
`ProjectSession::openProject` rebuilds the media library from it (`libraryFor`), while
`core::validateProject` requires every `Clip.assetRef` to resolve in it. `MediaImportService::import`
registers into `ProjectSession::mediaLibrary()` — the session-level `core::MediaManager` — only, so
`media.import` → `timeline.add_clip` → `project.save` wrote a document whose clips referenced
nothing and which `project.open` then rejected. Data loss for the user, and Requirement 15.1's
re-open leg blocked.

**The registration was put in `core::AddClipCommand::apply`**, mirroring `PlaceGeneratedClipCommand`
in `services/GenerativeMediaCoordinator.cpp`, and NOT in the import path. `AddClipCommand` is the
single choke point through which a clip carrying a new `assetRef` reaches a `Project` — the
`timeline.add_clip` tool, `ui::TimelineViewModel::addClip` (drag-and-drop) and the agent all go
through it, and it is the only command in `core/EditCommands.cpp` that introduces an `assetRef`
(delete/move/trim/split/reorder only ever carry an existing one forward). Registering there makes
the invariant hold by construction for **every** caller that places a clip, including one whose
asset was registered only in a session-level library, instead of fixing the one import path and
leaving the state reachable from the next one. Fixing it in `MediaImportService` would also have had
to reach the project through the engine to be undoable, turning an import into an undo-stack entry
and desynchronising `Project.assets` from `mediaLibrary()` on undo.

Properties of the fix: dedup is by `assetId` (`MediaAssetRef::operator==` compares identity only), so
a ref the table already resolves adds no second entry — a duplicate id would make the document
unopenable again, since `libraryFor` rejects one; `revert()` removes the entry **only** when that
apply added it (`assetAdded_`), so undo/redo round-trips exactly and undoing the second of two clips
sharing an asset keeps the entry the first created; a rejected add is unchanged from before, because
`TimelineEngine::apply` restores its whole snapshot on failure; the nil identity is never added,
because the library rebuild cannot catalogue it.

**Requirement 15.1's re-open leg is now genuinely asserted.** In `tests/e2e/editor_end_to_end_test.cpp`,
step (8) of `runChain()` goes through `invokeOk(kProjectOpen, …)` — the same hard assertion as every
other call in the chain — and the "reported, not asserted" gate, the `reopenSucceeded` /
`reopenError` fields and the explanatory comment are gone; the `trackCount` / `clipCount` /
`modified` assertions in `expectChainSucceeded` are now unconditional.

**Tests: 1160 total, from 1154** — same 4 expected skips, no existing test weakened, skipped or
changed. **No existing test had encoded the buggy behaviour**: the only pre-existing save/open
round-trip over a clip, `SessionToolsTest.ProjectSaveThenOpenReportsTheLoadedProjectAccurately`,
pushes its asset into `Project.assets` by hand before saving, so it never exercised the import path.
The 6 new cases were each confirmed to FAIL on the unfixed tree with exactly the reported
`assetRef does not resolve to any entry in Project.assets` error:

- `tests/core/edit_commands_test.cpp` (4, unit): the asset is registered and `validateProject`
  accepts the result, undo removes it and redo restores it; an already-registered asset is not
  duplicated and undoing the later add keeps it; a rejected add (overlap, and a missing track)
  leaves the table unchanged; a nil `assetRef` is not registered.
- `tests/services/session_media_track_tools_test.cpp` (2, service level, through the real tool
  surface + a real `MediaImportService`): `media.import` → `timeline.add_clip` → `project.save` →
  `project.open` round-trips, the document on disk loads and `validateProject` accepts it, and the
  reopened session's media library carries the asset with its resolved source path; and two clips
  over one imported asset register it exactly once and still re-open.



### ✅ Task 12.9 — the placeholder test deleted and Property 80 added in the same change

`tests/palmier_placeholder_property_test.cpp` and its `palmier_placeholder_tests` target are gone,
and `tests/docs/suite_hygiene_property_test.cpp` (Property 80, Requirement 15.6) arrives with them,
so the rule that forbids a placeholder lands with the removal rather than after it. The new source is
appended to the existing `palmier_docs_tests` target with `target_sources()`, as that block's own
instructions require; the only new build input is `PALMIER_SPEC_DIR="${PROJECT_SOURCE_DIR}/.kiro/specs"`
(the whole specs directory, not one spec, so a later feature's documents widen the component set with
no edit to the test).

**What the property asserts.** Six defects, each a pure function of the parsed `tests/CMakeLists.txt`,
the walked `tests/` tree and a discovered component set — no list of targets, sources or names is
written in the test, so a target or source added tomorrow is checked tomorrow:

1. a target registered with CTest that no `add_executable()` defines;
2. a registered target that compiles **no code of this product** — no `Palmier::` library other than
   the `Palmier::test_support` GoogleTest/RapidCheck bundle, and no source under `src/`. Excluding
   `test_support` is load-bearing: the deleted target linked exactly that and nothing else, so the
   clause read literally could not reject the one target it exists to reject. Compiling a `src/`
   source counts equally, because that is how most `palmier_services_*` targets are built;
3. a registered target none of whose own test sources references a documented component;
4. **the placeholder pattern**, both of design.md's conjuncts: a source that declares test cases,
   references no component of this product in code, and whose every owning target compiles no
   product code;
5. a test source no target compiles;
6. a source that declares test cases but is compiled only into targets CTest never registers — cases
   that never run assert nothing, the same defect in different clothes.

"A component named in this document" is read as the **intersection** of what a header under `src/`
declares (comment-stripped, so prose does not declare anything) and what the checked-in documents
name (`docs/*.md` plus every `requirements.md` / `design.md` under `.kiro/specs/`), filtered to names
of ≥4 characters carrying an interior case change — 225 names on this tree, from 96 headers and 12
documents. Two readings are documented in the file rather than assumed: the document set includes
`docs/` (otherwise `tests/app/platform_compatibility_test.cpp` — a real test of a real component
whose feature specification predates this one and is not checked in — would be condemned, though
`docs/BUILD.md` names `app::checkPlatformCompatibility` in the minimum-host table Requirement 16.1
mandates); and a reference must appear in **code**, not in a comment, which is what stops a future
placeholder from satisfying the clause by naming `TimelineEngine` in its header block.

**Tests: 1174 total, from 1160** — 3 removed with the placeholder, 17 added (Property 80 plus 16
`SuiteHygieneChecker` cases). Same 4 expected skips; no other test weakened, skipped or deleted. The
non-vacuity cases drive the same pure functions the scan uses: the deleted file's own shape,
reconstructed inline, must be flagged as a placeholder **and** as a target with no product code and
no named component; the same tautology source dropped into a target that links `Palmier::core` is
correctly *not* a placeholder but still fails the named-component clause, so the two clauses together
leave no way in; a registration of a non-existent target, an orphan source and an unregistered source
are each detected; and the CMake parser, the test-macro scanner and the component extractor have
their own cases, because a parser that silently found nothing would report a spotless suite. One case
asserts the checker is subject to its own rule — this file appears in the enumerated domain and is
outside the placeholder pattern only because `palmier_docs_tests` compiles product code, not because
anything is exempt.

### ⚠️ Suite hygiene — the reported `PreviewPlaybackProperties` flake was not reproducible and needs no fix

`PreviewPlaybackProperties.PresentationRateWithinBoundsAndDropsUnderFivePercent` was reported as
failing under `ctest -j$(nproc)` because it measures presentation rate against the real wall clock.
**It does not measure wall time at all**, and the reported failure did not reproduce:

- `tests/ui/preview_playback_property_test.cpp` contains no `<chrono>` include, no `steady_clock`,
  no `sleep` and no other real-time source. Its only clock is `ManualClock : public PlaybackClock`,
  injected into `PreviewController` at construction, advanced explicitly by the harness (one idle
  quantum per pump that found nothing due) and from inside the render seam (which is how composite
  latency is modelled). The presentation-rate windows are counted over *that* clock's stamps, so the
  measurement is a pure function of the generated inputs. This is the same seam
  `tests/e2e/editor_end_to_end_test.cpp` and `tests/services/offline_mode_availability_test.cpp`
  use, so there is no second injection to make.
- Empirically: 40 isolated runs of the property, 80 concurrent runs of the whole
  `palmier_ui_preview_playback_tests` binary at 2× core oversubscription, 11 full-suite runs at
  `-j$(nproc)` and 10 at `-j32` — zero failures of this test in any of them.

Because the property is already stated against injected time, there was nothing to convert, and the
bounds were left exactly as they are. **A different, genuinely load-sensitive test was found while
looking**: `ExportCoordinatorTest.ADestroyedCoordinatorCancelsAndLeavesNoPartialFile` failed 2 of 10
full-suite runs at `-j32` (4× oversubscription) and never at `-j$(nproc)`. It waits on a real worker
thread to reach its first frame within a wall-clock `kWaitBudget`, which is what a heavily
oversubscribed host misses. It is left untouched here — it is outside task 12.9 and outside the
reported defect — and is recorded so the next reader does not have to rediscover it.



### ✅ Task 12.3 — `tests/support/ReportParser` and the two report checkers

**12.3 is now ticked — 1208 tests, 4 expected skips (+34).** `tests/support/ReportParser.{hpp,cpp}`
parses both checked-in reports and checks them; `tests/docs/report_parser_test.cpp` runs it. All three
files are appended to the existing `palmier_docs_tests` target with `target_sources()`, as that block
instructs — no second `add_executable()`, no second `palmier_register_test()`, and no new compile
definition: `PALMIER_DOCS_DIR` was already defined there for this task, and `PALMIER_SPEC_DIR` (added
by 12.9) supplies `requirements.md`.

**The four functions the task names**, plus the value types: `parseParityReport` /
`parsePortBacklog` produce `ParityReport` (`ParityProvenance`, 34 `ParityEntry`, 31 `BuildOrderItem`)
and `PortBacklog` (`BacklogProvenance`, ten `BacklogEntry` each with an `AcceptanceCheck`);
`checkParityReport` / `checkPortBacklog` are pure functions from those values to
`std::vector<Defect>`. Dependency-free: standard library only — no YAML, no JSON, no regex engine and
nothing from `Palmier::`, so the target gained no link dependency.

**Totality is asserted, not claimed.** Every parse failure is a `Defect`, never an exception and never
a silent empty result: the empty document, unrelated prose, three truncations of each real document
and a table with the wrong column count all yield defects, and both parsers append a defect when they
find no entry table / no `### ` entry at all. That matters because `checkX(parse(doc)).empty()` is the
assertion 12.4 and 12.5 will make — a parser that threw would abort the suite and one that returned
nothing would make it pass vacuously. The clean-document cases therefore assert **what was parsed**
first (22 + 12 entries, 31 build-order items, ten backlog entries, nine complete acceptance checks,
each provenance field) and only then that the defect list is empty.

**All nine defect kinds are reachable and each is proven by a single edit to a real document**:
`MissingEntry` (a blanked row name; an `absent` entry dropped from the build-order list; a required
`PR` identifier absent), `DuplicateEntry` (a row written twice), `InvalidStatus` (`mostly` for a
status, `skip` for a disposition, `nearly` for a `status:` field), `InvalidPriority` (`urgent`; a
priority on a `present` row; a build-order item disagreeing with its table), `MissingRationale` (a
`-` rationale where 13.3 requires one; a 201-character one; a backlog entry with none),
`MissingCheck` (a `port` entry with no `check:`; a check missing its `then:`; a check on the
`not-applicable` entry, which 14.3 gives none), `MissingField` (a missing or non-`YYYY-MM-DD`
`comparison-date`; a missing or malformed `window`; a 201-character summary; a `macos-framework` with
no `linux-replacement`; a wrong column count), `DuplicateIdentifier` (two entries under one `PR`),
`OutOfOrder` (a `should` item ahead of a `must` one). The opposite direction is covered too: a
synthetic well-formed document of each kind yields **zero** defects, which is what rules out a
checker that always fails.

**The 22 tool categories, 12 capability areas and ten `PR` identifiers are not trusted to a
transcription.** They must be written down — the checker cannot derive them from the document it is
checking without checking nothing — so `requiredToolCategories()`, `requiredCapabilityAreas()` and
`requiredBacklogIdentifiers()` hold them, and three cases read the parenthesised lists straight out of
Requirements 13.1, 13.2 and 14.2 in `requirements.md` and compare. A slip in the transcription fails
the suite instead of quietly narrowing the check.

**Both checked-in documents pass with no structural defect**, so nothing in the grammar had to be
fixed. One **factual** staleness was found, which no structural check can see: `docs/PORT_BACKLOG.md`
said PR 399 was `in-progress` "because `docs/BUILD.md` does not exist yet — task 12.6 is unbuilt",
which task 12.6 has since made false. Both places that said it (the entry's `note` and the "Known
limits" list) now record that the document and its package table exist while the check itself has
still never been run on a clean checkout, which is the CI work of 12.11/12.12. `status: in-progress`
is unchanged and correct: Requirement 14.12 turns on whether the check has *passed*, not on whether
the code and documents exist. PR 404's note was re-verified and is still accurate —
`src/ui/MainWindow.cpp` has no `setMinimumSize` and no `QDockWidget`.

**One deliberate deviation from design.md's sketch.** The design writes
`Result<ParityReport> parseParityReport(...)`; the implementation instead follows
`tests/support/DocumentationChecker.hpp` (task 12.7), which design.md names as the same parser style,
and returns the report by value while appending to a `std::vector<Defect>&`. A `Result` would make a
malformed document an *alternative* to a parsed one, when what both requirements ask for is a partial
parse **plus** the list of what is wrong with it — a document missing one rationale must still yield
its other 33 entries so every remaining defect is reported in one run (Requirement 13.8's "report
each offending entry"). It also keeps the file free of `Palmier::core`.



### ✅ Tasks 12.4 and 12.5 — the quantified halves of the two report checks (Properties 69–72)

**12.4 and 12.5 are now ticked — 1214 tests, 4 expected skips (+6).**
`tests/docs/parity_report_property_test.cpp` (Properties 69, 70) and
`tests/docs/port_backlog_property_test.cpp` (Properties 71, 72) are appended to the existing
`palmier_docs_tests` target with `target_sources()` — no second `add_executable()`, no second
`palmier_register_test()`, and no new compile definition: `PALMIER_DOCS_DIR` and `PALMIER_SPEC_DIR`
were already attached there. Both files consume 12.3's pure functions unchanged, so a document cannot
pass the example-based checks and fail the quantified ones.

**They extend `report_parser_test.cpp` rather than restate it.** 12.3's half produces each defect by a
single **text edit** of a real document. These two **generate whole documents** to the grammar
`ReportParser.hpp` states and then mutate the **model** and re-render, so a mutation is a value
— `mutate(document, kind, choice)` is pure — and the property quantifies over (generated document) ×
(mutation kind) × (target entry and variant). Parity: 34 rows in a generated per-table order, drawn
statuses, components, priorities, rationale lengths across both ends of the 1–200 bound, all three
macOS-framework shapes, and a build-order projection derived from the rows. Backlog: the ten
Requirement 14.2 identifiers plus 0–30 further entries in a generated order, drawn summaries,
dispositions, statuses, components, notes and checks. Ten is the floor, not one: the check reports
each absent required identifier, and the empty-document case is 12.3's.

**All nine defect kinds are covered across the two files, and coverage does not depend on sampling.**
The Parity_Report grammar can express seven (`MissingEntry`, `DuplicateEntry`, `InvalidStatus`,
`InvalidPriority`, `MissingRationale`, `MissingField`, `OutOfOrder`) over 20 mutation kinds; the
Port_Backlog expresses the remaining two plus four shared ones (`MissingCheck`,
`DuplicateIdentifier`, `MissingEntry`, `MissingField`, `MissingRationale`, `InvalidStatus`) over 19.
Each file also carries one non-property test that drives **every** mutation kind × five choices on a
fixed document each run and asserts both that each is reported and that the kind set is still exactly
this grammar's — so a kind cannot silently stop being exercised, and the union is asserted to be all
nine.

**Four guards against a vacuous pass**, asserted on every generated case: (1) the checked-in document
is a case of every run — both properties assert it was found (non-zero bytes), that its tables /
entries / build-order projection / acceptance checks parsed to the counts the requirements fix, and
only then that it has no defects; (2) every generated document is compared **field for field** with
its model, not merely declared defect-free; (3) every mutant is asserted to still be a fully readable
document (entry and item counts equal the mutated model's, every name and heading read back, both
tables recognised, and the parser's "holds no entries at all" defect absent), so the asserted defect
is attributable to the injected fault; (4) the unmutated rendering of the same case is asserted clean.
Each mutation is asserted **by the offending entry's name**, which is what Requirements 13.8 and
14.11 actually oblige.

**One RapidCheck finding worth recording.** `rc::gen::inRange` scales its range with the `size`
parameter, which rises across a run, so a bare `inRange` draws only low indices in the early cases:
measured over a 100-case run, **five of the twenty parity mutation kinds were never drawn at all**.
Every categorical draw in both files therefore goes through
`rc::gen::resize(rc::kNominalSize, rc::gen::inRange(...))`, which is uniform over the whole range and
keeps `inRange`'s shrinking. After the change all 20 parity kinds and all 19 backlog kinds appear in a
single 100-case run. This applies to any categorical `inRange` draw elsewhere in the suite.

**Both checked-in documents pass unchanged** — no defect was found in `docs/UPSTREAM_PARITY.md` or
`docs/PORT_BACKLOG.md`, so neither document needed an edit, and no property was weakened to reach
green. No existing test or source was modified; the only edit outside the two new files is the
`target_sources()` block in `tests/CMakeLists.txt`.


### ✅ Task 12.8 — the quantified half of the documentation consistency check (Properties 76, 77)

**12.8 is now ticked — 1217 tests, 4 expected skips (+3).**
`tests/docs/documentation_consistency_property_test.cpp` is appended to the existing
`palmier_docs_tests` target with `target_sources()` — no second `add_executable()`, no second
`palmier_register_test()` and no new compile definition, since `PALMIER_DOCS_DIR` and
`PALMIER_OPTIONS_MANIFEST` were already attached by the 12.7 block, along with the live tool surface
(`ToolRegistry`, `ToolSchema`, `ProjectSession`, `Json`, `AppSettings`) it needs.

**It extends 12.7 rather than restating it.** 12.7's half checks the three checked-in documents and
produces each defect kind from a hand-written synthetic document. This half **generates whole
documentation sets** — a `docs/BUILD.md`, a `docs/TOOLS.md` and a `docs/REMOTE_ACCESS.md` — whose
option, tool, argument and settings **names are the running system's** (`palmier_options.txt`, the
`ToolRegistry::describe()` payload, `AppSettings::recognizedKeys()`) and whose **presentation is
drawn**: the number and titles of the `###` option groups and whether the first is titled at all,
which of three option-table / three argument-table / two settings-table column orders is used,
whether option types are backticked, and per tool the bold `Required` marker, the side the `*uuid*`
marker sits on, the argument order inside the table, the `| Field |`-table versus `Result:`-paragraph
result shape, the *(command result)* marker, and how many result fields are unconditional,
conditional and observed. Property 76 asserts every one of those documents is accepted; Property 77
mutates the **model** and re-renders, so `mutate(set, kind, choice)` is pure and the property
quantifies over (generated set) × (29 mutation kinds) × (target and variant).

**All six document-fault kinds are covered, and coverage does not depend on sampling.** The 29
mutations span the five name classes (option, tool, argument, result field, settings key) and the
rename / delete / add / retype / restructure fault classes, and produce `UndocumentedName`,
`UnknownDocumentedName`, `TypeMismatch`, `RequiredMismatch`, `OrderMismatch` and `SectionMissing` —
the whole vocabulary except `InputUnreadable`, which is a missing-input kind that 12.7 already owns.
Every mutation is asserted **by the offending name together with its document and its section**,
which is exactly what Requirement 16.8 obliges. A non-property test drives all 29 kinds × five
choices on a fixed set each run and asserts the produced kind set is still exactly that six, so a
kind cannot silently stop being exercised. All 29 appeared in a single 100-case run.

**Five guards against a vacuous pass**, asserted on every generated case: (1) the checked-in set is a
case of every run — both properties assert all three documents and the manifest were found (non-zero
bytes), that what was extracted has the real counts (as many options as the manifest lists, as many
tools as `tools/list` publishes, as many rows as the key table recognises, all non-zero), and only
then that it has no defects; (2) every generated document is read back **field for field** against its
model — every option's name, type and `###` section, every tool's name, published position,
no-arguments claim, argument names, types, uuid markers and required markings, every result field and
its conditionality, and every settings row's key, environment variable, flag and `##` section; (3) the
generator's own guarantees are asserted, not assumed (an argument-bearing tool and an argument-free
one, a uuid and a plain argument, a required and an optional one, a conditional field no payload
carried, both result shapes and a *(command result)* tool); (4) every mutant is asserted to still be a
fully readable set, so the reported defect is attributable to the injected fault — the four
**structural** faults (markers removed, region emptied, tool headings demoted, settings header
renamed) declare themselves and are instead asserted to produce exactly the `SectionMissing` the
extraction contract promises, rather than an empty and vacuously-consistent name set; (5) the
unmutated rendering of the same case is asserted clean. Two decoys ride along in every generated
`docs/BUILD.md` — a non-`PALMIER_` row inside the marked region and a whole option table outside it —
and are asserted **not** to have been extracted, so region scoping is quantified too. The guards were
verified to bite: perturbing the generated argument table's header cell fails Property 76 on the
readback (`0 == 5` arguments) rather than passing quietly.

**Requirement 16.8's third clause is asserted, not assumed.** Property 77 fingerprints (byte count
plus an FNV-1a hash) **every `.md` file under `docs/`** — enumerated from the directory, not listed —
before and after a full check run over the checked-in documents, on every one of the 100 cases. That
is deliberately wider than the three documents the checker reads: the obligation is that the check
leaves the documentation alone, and a document added later is covered with no edit here.

**Result fields, scope stated rather than hidden.** 12.7's twenty-tool scenario, and its assertion
that every registry-rendered payload is observed at least once, stay there and are not restated. This
file drives a compact **real** observation on every run — `project.create`, `project.info`,
`media.list`, `timeline.read` through the real registry over a real `ProjectSession` — which anchors
the checked-in `docs/TOOLS.md` result-field claims to real payloads here too, while the generated
cases quantify over result-field shapes with generated observations.

**Every categorical draw goes through `rc::gen::resize(rc::kNominalSize, rc::gen::inRange(...))`**,
for the reason measured in 12.4: a bare `inRange` scales with RapidCheck's `size` and silently fails
to draw whole categories.

**All three checked-in documents pass unchanged** — no inconsistency was found in `docs/BUILD.md`,
`docs/TOOLS.md` or `docs/REMOTE_ACCESS.md`, so no document needed an edit and no property was
weakened to reach green. No existing test or source was modified; the only edit outside the new file
is the `target_sources()` block in `tests/CMakeLists.txt`.


### ✅ Tasks 12.11 and 12.12 — the CI test-result artifact and the gated `l4-validation` lane

**12.11 and 12.12 are now ticked — still 1217 tests, 4 expected skips.** They landed together because
both edit `.github/workflows/ci.yml`. No test was added, removed or weakened; the only C++ change is a
measurement *print* in the existing hardware/software comparison test.

**12.11.** The test step became `xvfb-run -a ctest --test-dir build --output-on-failure --output-junit
ctest-results.xml 2>&1 | tee build/ctest.log` under `set -o pipefail` — load-bearing, because without
it `tee` returns 0 and a failing suite would pass the job. Two new steps run with `if: always()` (task
12.11's "unconditionally"): one generates the summary and appends it to `$GITHUB_STEP_SUMMARY`, one
uploads `ctest.log`, `ctest-results.xml`, `ctest-summary.md` and `build/Testing/**` as the
`ctest-results` artifact. Publishing on green runs is not decoration: Requirement 15.5's evidence —
which hardware tests skipped and the reason each recorded — is only observable on runs that passed.

> **The skip reason is NOT in the JUnit `<skipped>` element, and that is the whole difficulty of
> 12.11.** CTest classifies these tests as skipped through `SKIP_REGULAR_EXPRESSION`, so it writes
> `<skipped message="SKIP_REGULAR_EXPRESSION_MATCHED"/>` — the same constant for every skipped test,
> naming no cause. The text a test recorded with `GTEST_SKIP() << reason` is inside `<system-out>`, as
> `<file>:<line>: Skipped` followed by the reason, which may run over several lines. A generator that
> read the attribute would produce a summary in which all four expected skips read
> `SKIP_REGULAR_EXPRESSION_MATCHED`: the letter of "reports skipped", and none of Requirement 15.5.
> `scripts/ctest_summary.py` parses `<system-out>`, keeps the `file:line`, and preserves multi-line
> reasons verbatim — the comparison test's reason is a bulleted list naming both missing halves.

**Verified against real output, not by inspection.** A full `--output-junit` run of this tree
(1.1 MB of XML) through the generator yields `Passed 1213 | Failed 0 | Skipped 4 | Total 1217`, one
table row per test for all 1217, and the four actual recorded reasons: the unwritable-parent-directory
test's *"this user can write to a read-only directory…"*, the comparison test's two-bullet *"no
hardware encoder: … PALMIER_HAVE_NVENC, PALMIER_HAVE_VAAPI and PALMIER_HAVE_QSV are all undefined"* /
*"no software encoder to compare against: … no software H.264 encoder (\"libx264\")"*, and the
`OfflineModeAvailability` and `EditorEndToEndTest` host-encoder reasons. The rendered summary is 108 KB,
comfortably inside the 1 MiB step-summary limit. **A ctest quirk worth knowing**: with `--test-dir`, a
relative `--output-junit` path resolves against the *build* directory, not the working directory.

**12.12.** `l4-validation` is a third job, gated three ways because no hosted runner has the device:
`if: github.event_name == 'workflow_dispatch' && inputs.run_l4_validation` (a new `workflow_dispatch`
input defaulting to false, so neither a push, a pull request, nor a plain dispatch reaches it),
`runs-on: [self-hosted, linux, x64, nvidia-l4]`, and no `needs:` from anything. It installs nothing —
the driver, `ffnvcodec` and an FFmpeg with `h264_nvenc`/`libx264` are host state — but checks those
prerequisites up front with `nvidia-smi` and `pkg-config --exists ffnvcodec`, and asserts the
configuration summary reports NVENC *enabled (SDK found)* before measuring anything. The concurrency
group gained `${{ github.event_name }}` so a push cannot cancel a long L4 dispatch on the same ref.

**Where the three recorded values come from.** `export_hardware_software_comparison_test.cpp` already
exports Requirement 8.5's exact fixture (300 frames, 1920×1080, 30 fps, H.264) through the production
encode path and gates itself on real hardware, so rather than duplicate it, its hardware run now
prints `PALMIER_L4_ENCODER_NAME`, `PALMIER_L4_ELAPSED_MS` (a `steady_clock` bracket around `begin()` →
`awaitCompletion()`), `PALMIER_L4_OUTPUT_BYTES`, plus the fallback flag, reason and frame count,
between two sentinel lines. `scripts/l4_validation_report.py` lifts that block out of the `ctest -V`
log, writes the values to `$GITHUB_OUTPUT`, and only then applies Requirement 8.10's verdict.

**Requirement 8.10's ordering is enforced on both sides**: the test prints the block *before* it
asserts anything about the values, and the script writes the job outputs *before* it decides, so a run
that fails for a wrong encoder, a true software-fallback flag or a 0-byte output still retains its
measurements as job output. Three further details a reader should not have to infer:

1. **`ctest` exits 0 for a skip**, so it cannot be the verdict. A log with no measurement block exits
   the script 2 with a summary saying nothing was validated — a skip on the L4 host is a failed
   validation, not a pass. This path is verified against a *real* skip log from this host.
2. **A comparison failure that is not one of Requirement 8.10's three conditions** — frame counts or
   durations disagreeing — would leave the measurements passing, so a separate `if: always()` step
   fails the job on the recorded `ctest` exit status.
3. **Job outputs are written with `$GITHUB_OUTPUT`'s heredoc form**, not `key=value`, so a value that
   ever grows a newline cannot forge another output.

**What could not be run here, stated plainly.** This host has no GPU, no `ffnvcodec`, no `nvidia-smi`,
no software H.264 encoder and no `xvfb-run`, so: the measurement block has never been *emitted* by a
real hardware export (the test still skips, and its new code compiles but does not execute); the
`l4-validation` job itself is unexecuted; and the `xvfb-run` wrapping of the GUI half of the suite
remains unexercised. The parsing side is covered instead — `scripts/test_l4_validation_report.py`
(17 cases) drives the pass, wrong-encoder, software-fallback, 0-byte, no-block and unreadable-log
paths, and one of its cases reads
`tests/services/export_hardware_software_comparison_test.cpp` and asserts the sentinels and every key
the script parses are the ones that file actually prints. That check plus
`scripts/test_ctest_summary.py` (15 cases) run as a CI step, since they are the only guard against a
lane that cannot run here drifting out of sync. Neither script is registered with CTest: they are CI
tooling, not part of the Verification_Suite, and Requirement 15.6 governs what is registered.

**`docs/HARDWARE_ENCODE.md` disagreed with the implementation and the document was fixed** — it was
the document that was out of date, not the code. It ended with *"Not yet implemented: the automated
`l4-validation` CI job of task 12.12 … the three values are read from the test output rather than
emitted as job outputs"*, which is now false. Its L4 section states the CI lane and its three gates,
the emitted key for each recorded value and the job output it becomes, the `ctest`-exits-0-on-skip
trap, and the artifact contents; its operator command gained `-DPALMIER_BUILD_TESTS=ON`, the `tee` and
the report-script invocation, so the documented procedure is exactly the job's. Nothing else in the
document needed changing: the fixture (300 frames, 1920×1080, 30 fps), the codec (`h264_nvenc`), the
three recorded values and the three failure conditions already matched. No documentation checker reads
this file — 12.7 and 12.8 cover `BUILD.md`, `TOOLS.md` and `REMOTE_ACCESS.md` only — so the agreement
was established by reading, and is now pinned in the one place a machine can check it: the
key-consistency case above.


---

**The owed parity re-score is done, and the honest answer is not the one the owed-work note above
predicted: both generation rows stay `absent`.** `docs/UPSTREAM_PARITY.md` now records
`linux-ref: 3516ba02574782c348294897bdeab93ff8185b3f` and `comparison-date: 2026-08-11`, and the
`linux-components` and `rationale` cells of `generate` (tool category) and `generation and
upscaling` (capability area) were rewritten — but the **statuses did not move**, so the build-order
projection is untouched and the status counts stay **3 `present` / 11 `partial` / 20 `absent`**, not
the 3 / 13 / 18 the note above anticipated. Every count was re-derived mechanically from the tables
rather than trusted: 34 rows (table 1 = 3 `present` / 7 `partial` / 12 `absent`; table 2 =
0 / 4 / 8), a 31-item projection holding exactly the `absent` and `partial` rows with 2 `must`,
19 `should` and 10 `later`, correctly sorted and consecutively numbered, `generate` still at
position 12 and `generation and upscaling` still at 20, and every priority-bearing rationale inside
1–200 characters.

**Why the prediction failed — a false premise, not a definitional disagreement.** The note above,
and the report's own "Known limits" bullet, both said the rows should become `partial` because "the
offline default still completes no generation" — which implies that a *non-default* configuration
does. It does not. Task 10.5's work is real and reachable (`GenerativeBackendRegistry` compiles all
three backends in, `ApplicationComposition` calls `selectGenerativeBackend()`, `main.cpp` now wires
`AppSettings` so `--generative-backend hosted|byok` is honoured by the shipped binary, and the
`generation.generate` hook asks the selected backend for `unmetPrecondition()` first), but
**this tree implements no HTTPS transport**: `services::GenerativeHttpTransport` is a declared seam
whose only product implementation is `makeUnavailableGenerativeHttpTransport()`, and no CMake option
or `#ifdef` supplies another (`grep` for `: public GenerativeHttpTransport` finds one product class
and six test doubles). `transportFor()` therefore installs the unavailable transport in every
non-injected composition, so `hosted` and `byok` fail at `submit` with `Unsupported` having sent no
bytes. **No configuration of this tree completes a generation** — prompt-to-video, prompt-to-image,
catalog-driven model choice (PR 406), upscale (PR 396) and audio generation (PR 395) are all
unperformable — and the report's `partial` requires **at least one reachable operation**, where
every row it scores `partial` cites a user operation that *completes* (`media`: import and list;
`project settings`: settable at create, readable by `project.info`; `MCP and agent chat`:
`initialize`, `tools/list`, `tools/call`). A request path that is always refused is not such an
operation, which is exactly what the `absent` definition means by "a component may exist and still
score `absent`". Implementing that one interface is the single change that would make both rows
`partial`; until then the honest score is unchanged and only the *reason* has moved on, from "the
only installed backend refuses" to "no transport is implemented".

**One test was corrected, not weakened, by advancing `comparison-date`.**
`ParityCheckFalsifiability.DetectsAMissingProvenanceFieldAndAMalformedDate` anchored its mutation on
the literal string `comparison-date: 2026-08-04`, so an ordinary re-score broke it — the anchor was
absent and the malformed-date defect was never produced (a test that would have started passing
vacuously had its `EXPECT_NE(at, npos)` guard not caught it). The anchor is now read back out of the
parsed document (`report.provenance.comparisonDate`, asserted non-empty first); the mutation and the
assertion are unchanged, so the case still proves the checker rejects a non-`YYYY-MM-DD` date. The
two remaining `2026-08-04` / `16274d51…` literals in `tests/docs/` belong to *synthetic* documents
built inside the tests and are deliberately left alone.

**Two other rows were re-checked because `linux-ref` moved, and neither changed.** `git diff` of
`src/` between the old and new ref touches only the generative backends, `main.cpp`/`AppSettings`,
and `core::AddClipCommand` (which now registers a placed clip's asset in `Project.assets` — a
document-integrity fix that adds no operation). **`src/ui/` is byte-identical**, so every row citing
task 11.2 or 11.3 stands as written. The `main.cpp` wiring incidentally closes the first of the two
product gaps recorded from task 10.6 above: the shipped executable no longer ignores the
configuration file, the environment and the command line. The second gap — Requirement 12.9's
unknown-`trackId` case reaching the network and leaving a media-library entry behind, documented as
an explicit exclusion in Property 66 — was still open at this point in the document's history.
**RESOLVED**: see the "Both known post-stage-11 defects fixed and verified" note after Stage 11's
entry below, which closes it via `ITimelinePlacement::trackExists()` (commit `52b153d`).

**Task 12.13 is done and stage 12 is closed: `1217` registered tests, `100% tests passed, 0 tests
failed out of 1217` in `build-nogui`, about 15 seconds of wall clock, with the four expected skips
unchanged** (`ExportCoordinatorValidate.RejectsAnUnwritableParentDirectory`,
`ExportHardwareSoftwareComparisonTest.HardwareAndSoftware…`,
`OfflineModeAvailability.ExportWithTheHostEncoder`, `EditorEndToEndTest.ChainThroughTheHostEncoder`
— all four for the same absent-encoder reason). 12.1 through 12.12 were verified `[x]` before 12.13
and the stage-12 parent were ticked, and every document 12.6 lists is present in `docs/`.
**Stage 11 is untouched and remains blocked**: Qt 6 is absent from this host, so 11.1–11.12 stay
`[~]`/`[ ]`, the stage-11 parent stays open, and nothing above implies otherwise. The one remaining
`must`-priority parity gap — a GUI user still cannot edit at all, because `MainWindow` is a
placeholder — is stage 11's work, not stage 12's.


### ✅ Stage 11 — GUI assembly implemented and verified in CI (Qt was unavailable in every prior sandbox)

Tasks 11.1–11.12 are complete. Every prior sandbox that touched this spec lacked Qt 6, so stage 11
had never been built or run anywhere before this round; it was written from careful reading of the
existing view-model / composition-root APIs with **zero local compilation**, then verified for real
by pushing to `feat/end-to-end-editor-integration` and reading the GitHub Actions `Build & Test
(ubuntu-24.04)` job's actual compiler and `ctest` output — the same job stage 0 through 10 already
rely on, with Qt genuinely present on that runner image.

**What was built:** `ui::GuiToolGateway` (11.1) — one method per gesture, each building the exact
`Json` arguments `services::ToolRegistry` declares and calling `executeTool(name, args,
InvocationSource::Gui)`, the same path the MCP endpoint and the in-app agent use. `MainWindow`
(11.2) — the five-dock shell (Media Browser, Timeline, Preview central, Inspector, Agent Chat
tabbed with Inspector), the five menus in order, and the three status-bar notices. `TimelinePanel`
+ a second `PreviewView` constructor (11.3) so the shell binds the composition's one shared
`PreviewController` rather than constructing a second one (Requirement 1.1). `TimelineViewModel`,
`InspectorViewModel` and `MediaBrowserViewModel` (11.4) gained an optional `GuiToolGateway*` — every
existing Qt-free unit test that constructs a view-model directly and expects direct-engine semantics
keeps working unmodified, because the gateway is only installed by the real shell.
`ui::ProjectFileActions` (11.5) — the `PendingIntent` unsaved-changes state machine (Save
continues only if the write succeeds, Discard proceeds without writing, Cancel abandons the
operation with no state change), and `ui::ExportDialog` (11.6) — which talks directly to
`ExportCoordinator::begin()/pump()/cancel()` rather than through the blocking `timeline.export` tool
call, matching that handler's own documented "the GUI does not take this path" contract.
`app::ComponentConstructionError` (11.7) wraps `ApplicationComposition` construction in `main.cpp`
in try/catch on both the Qt and no-Qt paths; every Requirement 1.1 accessor was confirmed by reading
the header to already return a non-null stable reference unconditionally.

**Tests written:** `tests/ui/shell_layout_property_test.cpp` (11.8, Property 1: panel reachability
across window sizes 1024×640..3840×2160). `tests/services/edit_equivalence_property_test.cpp`
extended (11.9, Property 2: GUI/MCP/agent produce identical project state) with a fourth path
through a real `GuiToolGateway`. `tests/core/timeline_undo_redo_roundtrip_property_test.cpp`
extended (11.10, Property 3: undo restores the immediately prior state) driving edits through the
gateway and undo/redo through the engine directly. `tests/ui/shell_unit_test.cpp` (11.11): five
panels visible, five menus in order with enabled actions, Undo/Redo disabled at empty history,
notices persistent across refresh ticks, one test per (trigger × outcome) of the unsaved-changes
prompt via injected `UiPrompts` (not a real `QFileDialog`), and a parameterized `StartupGuardTest`
across the Requirement 1.1 component names — the last of these is honestly documented in its own
comment as synthetic, since `ApplicationComposition` is designed to never actually throw.

**CI verification — five real, distinct defects found and fixed, none of them findable without a
compiler:**

1. **`GuiToolGateway.hpp` declared every method returning the bare unqualified name `Json`**, with
   nothing in scope to resolve it to `palmier::services::Json` (the header *includes*
   `services/Json.hpp` fully, but never qualifies or aliases the name it uses). `GuiToolGateway.cpp`
   had a `using services::Json;`, but it sat inside the file's anonymous namespace, so it never
   reached the enclosing `palmier::ui` namespace where the method bodies live below it. Fixed by
   hoisting the `using`-declaration to `palmier::ui` namespace scope in both files. This one bug
   cascaded into `InspectorViewModel.cpp`'s `addEffect`/`trimStart`/`trimEnd`, which bind
   `gateway_->method(...)` to `toCommandResult`'s `const Result<services::Json>&` parameter — fixing
   the root cause fixed all of it in one commit.
2. **`MainWindow::buildDocks()`'s media-import validator lambda returned the wrong type** —
   `media::validateMediaImport(path)`'s `Result<media::MediaInfo>` — where
   `MediaBrowserViewModel::ImportValidator` requires `Result<MediaAssetRef>(const
   std::filesystem::path&)`. Fixed by actually performing the validation and, on success, minting a
   `MediaAssetRef` for the imported path, mirroring the pattern the existing
   `media_browser_viewmodel_test.cpp`'s `acceptingValidator()` helper already used.
3. **Both new Qt test files declared `QMainWindow::findChildren<QDockWidget*>()`'s result as
   `std::vector<QDockWidget*>`**; Qt returns `QList<QDockWidget*>`. `shell_unit_test.cpp` also called
   `window.statusBar()` without including `<QStatusBar>`, leaving it an incomplete type at the call
   site. Both are simple type/include corrections.
4. **`ProjectFileActions::open()` called the open-source file-picker prompt BEFORE checking for
   unsaved changes**, with the unsaved-changes check only reachable afterward, inside `openPath()`.
   This meant: a Discard choice's confirmation prompt never ran if the test's file picker dismissed
   first; a Cancel choice could never actually prevent reaching the open-source picker (the check ran
   after the picker, not before); and a Save choice never got the chance to write before the picker
   appeared. Fixed by running the unsaved-changes resolution in `open()` itself before the picker,
   matching the already-correct ordering `newProject()` used; `openPath()`'s own documented contract
   (still resolving unsaved changes when called directly, e.g. from a future recent-files entry) is
   preserved via a shared private `loadFromPath()` helper so the prompt never runs twice for one
   gesture. This was caught by three failing `ProjectFileActionsPromptTest` cases in 11.11's own test
   file, and would not have been caught by code reading alone.
5. **The CI pipeline itself was broken before any of this code was reached**: `apt-get install
   nv-codec-headers` failed with "Unable to locate package" on every push to this branch for the
   three weeks preceding this session, confirmed unrelated to any code change by checking
   `gh run list` history. Root-caused to the package being genuinely absent from the current
   `ubuntu-24.04` runner image's resolved archive snapshot (ruled out a stale-apt-cache explanation by
   forcing a clean `apt-get update` first — the package was still unresolvable). Fixed by building
   `nv-codec-headers` from its pinned upstream source tag (`n13.0.19.0`) instead of via `apt`, since
   the package only ever installs a handful of headers and a `.pc` file with nothing to compile — this
   keeps the NVENC path (Requirement 8.1) actually compiling in CI rather than silently dropping
   vendor-SDK coverage.

**Final CI status: the GitHub Actions run is RED overall** (`gh run list` reports `completed
failure` for run `32011651687` / commit `de99261`), **not green.** To be precise about what that
red status does and does not mean: the `Build & Test (ubuntu-24.04)` job's `Build` step — which
compiles and links every Stage 11 source, all of `src/`, and the full test suite — succeeds
completely, and `ctest` reports `99% tests passed, 1 test failed out of 1239`. But `ctest`'s
non-zero exit on that one failure is what fails the `Test` step, which fails the job, which fails
the run. "Stage 11 compiles, links and its own tests pass" is a true and verified statement; "CI is
green" is not, and the run should not be read as a passing check.

The one failing test, `EditorEndToEndTest.ChainThroughTheHostEncoder`, is confirmed via `git log` to
be **pre-existing and unrelated to stage 11** — last touched by task-12.10/an unrelated core-fix
commit, weeks before this session, and it references nothing stage 11 added. It fails with a
probed-duration mismatch (~1006.9 s decoded from a 2 s timeline) that only manifests on a host with
a real software H.264 encoder present — every prior sandbox recorded in this document lacked one
and always recorded this test as a `GTEST_SKIP()`, so its assertions were previously "written but
never executed" (see the 12.10 progress note above). This CI run is the first time a real encoder
has been present for this test, and it surfaced a genuine, previously-unverified defect in the
encoder/duration-probing path (traced as far as: `media::probeMediaFile` trusts FFmpeg's
`AVFormatContext::duration` verbatim via `Duration::fromMicroseconds(ctx->duration)` in
`src/media/MediaProbe.cpp`, and that container-level value is wrong for the `.mov` file this export
produces, even though the video stream itself decodes the correct frame count) — outside stage 11's
scope (GUI assembly). Left as a known, documented, separate defect rather than fixed here; the CI
run for this branch will continue to show red on this one test until it is investigated and fixed
in a follow-up covering Stage 9 (export) / Stage 12 (the end-to-end test) rather than Stage 11.

### ✅ Both known post-stage-11 defects fixed and verified — CI is now fully green

The two issues the note above left open — `EditorEndToEndTest.ChainThroughTheHostEncoder`'s
probed-duration bug and Requirement 12.9's unknown-`trackId` gap — were investigated and fixed in a
follow-up round, each with its own commit, verified through the same GitHub Actions CI used
throughout this stage.

**`ChainThroughTheHostEncoder`'s root cause (commit `720edd3`):**
`FfmpegEncodeBackend::encode()` (`src/media/MediaEncoder.cpp`) computed the video `AVFrame`'s `pts`
by rescaling the frame's presentation time against `stream_->time_base`. But `avcodec_send_frame()`
interprets `AVFrame::pts` against `codec_->time_base`, not the stream's — and
`avformat_write_header()` (called once, before any frame is encoded) rewrites `AVStream::time_base`
for many muxers, MOV/MP4 included, to whatever timescale the container format prefers, which need not
equal `codec_->time_base`. So by the time `encode()` ran per frame, `stream_->time_base` could
already differ from the value the pts calculation assumed. This fed x264 timestamps scaled for the
wrong clock: the frame COUNT stayed exactly right (60/60, matching the plan — which is why every
earlier assertion in the test, including the decoded-frame-count check, passed), but every frame's
presentation time was wrong, and the muxed/probed container duration inflated to ~1006.9 s for a 2 s
timeline. The CI log's own x264 stats confirmed this precisely: 60 total frames encoded (`I:1 P:16
B:43`), but `kb/s:1.18` against a requested 2000 kb/s target proves x264 itself received grossly
wrong per-frame timing, not merely a probe misreading. Fixed by rescaling against `codec_->time_base`
instead — the same pattern the audio path already used correctly (which only ever rescales the
*output packet* from `ctx->time_base` to `stream->time_base` via `av_packet_rescale_ts`, and never
scales the *input frame's* pts against the stream's time base).

**Requirement 12.9's unknown-`trackId` gap (commit `52b153d`):**
`GenerativeMediaCoordinator::generateAndPlace` ran the generation (a network call) and imported the
resulting asset into the media library before `TimelineEnginePlacer::place` ever checked whether the
requested track existed — so a syntactically valid but unknown `trackId` reached the network and left
an orphaned library entry that the tool executor's rollback does not remove (rollback undoes engine
commands only; the import is not one). Fixed by adding `ITimelinePlacement::trackExists(const Uuid&)`
— implemented by `TimelineEnginePlacer` via a cheap `engine_.snapshot()` scan — and calling it from
`generateAndPlace` immediately after the source-range check and before the entitlement gate, the
runner, or the library are ever touched, so an unknown track is now refused with `NotFound` before
anything downstream runs. Both `ITimelinePlacement` test mocks were updated with a `trackExists()`
override; a new unit test (`GenerativeMediaCoordinatorTest
.UnknownTrackRejectedBeforeGeneratingOrTouchingLibrary`) asserts zero gate/runner/placement calls and
an unchanged library for this case; and `tests/services/generative_lifecycle_property_test.cpp`'s
Property 66 (`GenerativeLifecycleProperties.InvalidGenerationRequestsNeverReachTheNetwork`) — which
had documented this exact case as an explicit, honest exclusion since task 10.6 — now generates and
asserts it like every other pre-submission rejection, and the file's header prose was updated from
"does NOT cover" to describing the case as covered.

**Verification: CI run `32056009190` (commit `52b153d`) is fully green.** Both jobs passed
(`Configure without vendor SDKs`: 1m2s; `Build & Test (ubuntu-24.04)`: 9m14s), and the downloaded
`ctest-summary.md` confirms **1239 passed, 0 failed, 1 skipped, 1240 total.** The sole skip,
`ExportHardwareSoftwareComparisonTest.HardwareAndSoftwareExportsOfTheFixtureAgreeOnFrameCountAndDuration`,
records its own reason (`no hardware-encode-capable device reported for H.264 on "Software (CPU)"`)
and is the same pre-existing, expected, documented skip this branch has always had on a runner with
no real GPU — unrelated to either fix. `EditorEndToEndTest.ChainThroughTheHostEncoder` now passes
in 0.39 s. No other test regressed. This is the first fully green CI run in this document's history
for the `feat/end-to-end-editor-integration` branch: Stage 11 is complete, and both of the defects
that were surfaced only once a real toolchain could compile and run this tree are now closed.

### ✅ The GUI is now visually verified, not only assertion-verified (`ci.yml` launch smoke test)

Every claim above about stage 11 rested on assertions over widget state: `ShellUnitTest` constructs a
real `MainWindow` under `xvfb` and checks its structure, but nothing ran the shipped **executable**
through `main.cpp`'s launch sequence — `AppSettings` resolution, the platform gate,
`ApplicationComposition` construction, the MCP server start, `window.show()`, a live Qt event loop —
and no human or agent had ever seen the running editor. A binary can pass every widget test and still
fail to start on a missing runtime plugin or block on a modal raised before the window is shown.

`ci.yml`'s `build-and-test` job now closes that gap with a **launch smoke test** (three steps at the
end of the job, so it reuses the already-built binary rather than costing a second full build). It
starts `palmier-pro` on a private `Xvfb` display with a clean `HOME` and a non-default MCP port (so an
occupied 19789 cannot raise the blocking "MCP server unavailable" warning before the window is shown),
then asserts three things rather than merely capturing: the process survives startup, a window titled
`Palmier Pro` maps within 30 s, and the captured frame is not a nearly-flat colour (the signature of a
window that mapped but painted nothing). The screenshot and the editor's own stderr are published as a
`launch-smoke-test` artifact. All three steps carry `if: always()` so a failing `ctest` cannot hide the
launch verdict.

**First run (`32361381399`) passed and the screenshot was inspected.** The editor mapped window id
`2097158` and painted **4696 distinct colours**, and the capture shows the whole of stage 11 working:
the five menus in their documented order (File, Edit, Playback, Export, Help); the Media Browser dock
with its Import Media button, Library and Clip Versions lists; the Preview canvas (black, correct for
an empty timeline); the Inspector reading "No clip selected" with **Agent Chat tabbed beside it**, which
is exactly the designed arrangement; the Timeline dock with Play/Pause/Stop, `00:00:00.000`, one track
row, and **Undo/Redo correctly greyed out on an empty history**; and **all three status notices**
present at once — the no-GPU/CPU-processing fallback, audio-output-unavailable, and the PipeWire/ALSA
detail. Every one of those was previously only an assertion in `shell_unit_test.cpp`; they are now
observable. The editor's own stderr contained nothing but two benign PipeWire "can't load config
client.conf" lines, consistent with a runner that has no sound card.
