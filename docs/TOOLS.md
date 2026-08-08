<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Tool reference

This is the Requirement 16.4 document: every tool `tools/list` returns, with its description, each
argument's JSON type and required/optional marking, and every field of a success result.

Every name below was read out of `src/services/ToolRegistry.cpp` (the registrations, the argument
declarations and the result objects each handler builds) and `src/services/ToolSchema.cpp` (how a
declaration becomes a published schema and a validator). It is a **checked** document: task 12.7's
consistency checker compares the names here two-way against the live `ToolRegistry::describe()` and
each `ToolSchema`, so a tool, argument or result field that this file and the running system disagree
about fails the Verification_Suite.

For how to reach the endpoint at all, see [`MCP_CLIENTS.md`](MCP_CLIENTS.md).

## Rules that hold for every tool

**The published schema is the validator.** Each tool declares its arguments once as a `ToolSchema`;
`inputSchema` is rendered from that declaration and the same declaration is enforced before the
handler runs. So the schema in `tools/list` accepts exactly the argument objects the tool accepts.

**`additionalProperties` is `false` everywhere.** An undeclared member is rejected with
`unknown field '<name>' is not accepted by this tool` — including on the five tools that take no
arguments at all, whose schema is an empty object schema.

**An optional argument may be omitted, never sent as `null`.** Absence is accepted; a `null` fails
the type check for the declared type.

**Types are exact.** An `integer` argument rejects a fractional payload; a `number` argument accepts
either. A `string` marked *uuid* below publishes `"format": "uuid"` and rejects anything
`core::Uuid::parse` will not accept.

**No project open is a refusal, not a partial edit.** Every tool except `project.create` and
`project.open` answers `no project is open: cannot execute tool '<name>'` before it parses an
argument, applies a command or touches the media library, so nothing has changed when that error
comes back.

**`status` versus `noOp`.** Every tool that applies an `EditCommand` reports either
`"status": "applied"` or the pair `"noOp": true` and `"indication": "<engine message>"` — a request
that was legal but changed nothing. The two are mutually exclusive and only one of them appears.
Tools carrying this pair are marked *(command result)* below.

**Three tools have no built-in implementation** and depend on a collaborator the composition root
installs. Absent it they return `Unsupported`, after the no-project check:
`media.import is not available: no media import service is configured`,
`generation.generate is not available: no generative backend is configured`,
`timeline.export is not available: no export engine is configured`. The session and media-listing
tools do have built-in implementations, which the GUI may override (for example to interpose a save
destination prompt) without changing their arguments or results.

The order below is the order `tools/list` publishes, which is the registration order in
`buildDefaultToolRegistry`.

### Extraction contract for task 12.7

The consistency checker reads this file mechanically, so the layout below is a contract, not a style
preference. Changing it changes what the checker sees.

- A **tool section** is an `## ` heading whose only content is the backticked tool name. The prose
  sections (`Rules that hold for every tool`, `Next`) carry no backticked dotted name and are
  therefore not tool sections.
- **Arguments** come from the table whose first header cell is `Argument`: the backticked name in the
  first cell, the JSON type in the `Type` cell, and `**yes**` / `no` in the `Required` cell. The
  `*uuid*` marker in a `Type` cell means the published schema carries `"format": "uuid"`. A tool that
  accepts no arguments must say `No arguments.` on a line of its own — silence would be
  indistinguishable from an omission.
- **Result fields** come from the table whose first header cell is `Field`, or from a paragraph that
  begins with the word `Result`. Either way a field is written as a backticked name followed
  immediately by its type in parentheses — `` `durationNs` (integer) `` — because that is the shape
  the checker matches. Grouping several names under one parenthesised type hides all but the last
  from the checker, so each field carries its own.
- A field is **conditional** — exempt from "some invocation must return it" — when its note, its
  `Notes` cell, or the sentence declaring it says when it is present (`present only`, `present when`,
  `only when`, `absent`). A conditional field declared outside the `Result` paragraph, as
  `media.import`'s resolution fields are, must sit in a sentence that states its condition; otherwise
  the checker will not read it as a field at all.
- `*(command result)*` stands for the `status` / `noOp` + `indication` trio described above. All
  three count as documented and all three are conditional, since a single invocation shows only one
  arm.
- A name mentioned in ordinary prose is never read as a field or an argument, because it is not
  followed by a parenthesised type. Prose is free.

## `timeline.read`

Read the current project timeline (tracks, clips, effects, transitions).

No arguments.

Result — the whole project value:

| Field | Type | Notes |
|---|---|---|
| `id` | string | project UUID |
| `name` | string | |
| `timelineFps` | object | `{ "numerator": int, "denominator": int }` |
| `canvas` | object | `{ "width": int, "height": int }` |
| `colorSpace` | string | e.g. `Rec.709` |
| `version` | string | document schema version |
| `tracks` | array | see below |
| `assets` | array | each `{ "assetId": string, "sourcePath": string }` |
| `durationNs` | integer | timeline duration in nanoseconds |

Each `tracks` entry: `id` (string), `kind` (`"video"` or `"audio"`), `muted` (bool), `locked`
(bool), `clips` (array).

Each `clips` entry: `id`, `assetId`, `sourcePath` (strings), `timelineStartNs`, `sourceInNs`,
`sourceOutNs`, `durationNs` (integers), `opacity`, `gain` (numbers), `effects` (array), and
`transitionIn` — either an object or JSON `null`, never absent.

Each `effects` entry: `id` (string), `type` (string), `parameters` (object of name → number).
A transition object: `id` (string), `kind` (string), `durationNs` (integer).

## `project.create`

Create a new project with the given name, frame rate, canvas and colour space, and make it the
current project. One of the two tools exempt from the no-project refusal.

| Argument | Type | Required | Accepted values |
|---|---|---|---|
| `name` | string | **yes** | 1–128 characters |
| `fps` | number | **yes** | 1–240 |
| `width` | integer | **yes** | 16–7680 pixels |
| `height` | integer | **yes** | 16–4320 pixels |
| `colorSpace` | string | no | `sRGB`, `Rec.709`, `Rec.2020`, `Rec.2100 PQ`, `Rec.2100 HLG`, `Display P3`, `Linear sRGB`; defaults to `Rec.709` |

A whole `fps` becomes the exact rational `n/1`. The three broadcast pull-down rates are recognised
within half a thousandth and become their exact NTSC ratios (`29.97` → `30000/1001`); any other
fractional value is taken to a thousandth (`n/1000`).

Result: `projectId` (string), `name` (string), `fps` (object), `canvas` (object), `colorSpace`
(string), `modified` (bool), `documentPath` (string or `null` — a new project has never been
written).

## `project.open`

Open a `.palmier` document and make it the current project. The second tool exempt from the
no-project refusal.

| Argument | Type | Required | Accepted values |
|---|---|---|---|
| `path` | string | **yes** | 1–4096 characters |

The open is all-or-nothing: a missing, unreadable, malformed or unsupported-schema document is
reported with the path and the reason, and the project that was current stays current with its
document path, modified flag and undo history intact.

Result: `projectId` (string), `name` (string), `trackCount` (integer), `clipCount` (integer),
`documentPath` (string or `null`), `modified` (bool).

## `project.save`

Write the current project to a `.palmier` document (defaults to its recorded document path).

| Argument | Type | Required | Accepted values |
|---|---|---|---|
| `path` | string | no | 1–4096 characters; omitted means the recorded document path |

Omitting `path` on a project that has never been written is a `FailedPrecondition`, not a guess:
`project.save needs a destination: the project has no recorded document path, so 'path' is required`.

Result: `documentPath` (string — the path actually written), `bytesWritten` (integer), `modified`
(bool — normally `false`; `true` only when the project was edited *while* the write was running, in
which case the written document is still valid and the session is legitimately still dirty).

## `project.info`

Report the current project's identity, settings, counts, modified state, document path and undo
depth.

No arguments.

Result: `projectId` (string), `name` (string), `fps` (object), `canvas` (object), `colorSpace`
(string), `trackCount` (integer), `clipCount` (integer), `assetCount` (integer), `durationNs`
(integer), `modified` (bool), `documentPath` (string or `null`), `undoDepth` (integer).

`assetCount` is read from the media library, which is the same source `media.list` reads, so the two
always agree.

## `media.import`

Probe, validate and register a media file as an asset of the current project's media library.

| Argument | Type | Required | Accepted values |
|---|---|---|---|
| `path` | string | **yes** | 1–4096 characters |

Result: `assetId` (string), `sourcePath` (string), `containerFormat` (string), `durationMs`
(integer), `hasVideo` (bool), `hasAudio` (bool), `duplicate` (bool — the file was already
registered).

`width` (integer) and `height` (integer) are present only for an asset carrying a decodable video
stream, and `fps` (number) only when a frame rate was determined. They are **absent**, not `null`,
otherwise, so a caller can distinguish an audio-only asset from an unknown one.

## `media.list`

List the assets registered in the current project's media library.

No arguments.

Result: `assets` (array of `{ "assetId": string, "sourcePath": string, "displayName": string }`,
where `displayName` is the source path's filename) and `count` (integer).

## `timeline.add_track`

Append a video or audio track after the last existing track of that kind.

| Argument | Type | Required | Accepted values |
|---|---|---|---|
| `kind` | string | **yes** | `video`, `audio` |

The 64-tracks-per-kind cap is enforced by the command, not the schema, because it counts the
project's existing tracks.

Result: `trackId` (string), `kind` (string), `index` (integer — where the track was inserted),
`trackCount` (integer), `status` (`"applied"`).

## `timeline.remove_track`

Remove a track and every clip on it, preserving the order of the remaining tracks.

| Argument | Type | Required | Accepted values |
|---|---|---|---|
| `trackId` | string *uuid* | **yes** | |

Result: `trackId` (string), `trackCount` (integer), `clipCount` (integer — the project total after
the removal), `status` (`"applied"`).

## `timeline.set_track_muted`

Mute or unmute a track, leaving its clips and every other track untouched. Undone by exactly one
`edit.undo` like any other edit.

| Argument | Type | Required | Accepted values |
|---|---|---|---|
| `trackId` | string *uuid* | **yes** | |
| `muted` | boolean | **yes** | |

Result: `trackId` (string), `muted` (bool), *(command result)*.

## `timeline.add_clip`

Add a clip referencing an asset onto a track at a timeline position.

| Argument | Type | Required | Accepted values |
|---|---|---|---|
| `trackId` | string *uuid* | **yes** | |
| `assetId` | string *uuid* | **yes** | |
| `sourceOutNs` | integer | **yes** | must be greater than `sourceInNs` |
| `sourcePath` | string | no | informational source path/locator |
| `clipId` | string *uuid* | no | generated when omitted |
| `timelineStartNs` | integer | no | ≥ 0; defaults to 0 |
| `sourceInNs` | integer | no | defaults to 0 |
| `opacity` | number | no | 0–1; defaults to 1.0 |
| `gain` | number | no | ≥ 0; defaults to 1.0 |

`sourceOutNs > sourceInNs` is a relation between two arguments, which the schema vocabulary cannot
express, so it is enforced by the handler: `sourceOutNs must be greater than sourceInNs`.

Result: `clipId` (string), *(command result)*.

## `timeline.delete_clip`

Delete a clip by id from whichever track holds it.

| Argument | Type | Required |
|---|---|---|
| `clipId` | string *uuid* | **yes** |

Result: `clipId` (string), *(command result)*.

## `timeline.move_clip`

Move a clip to a new timeline start on its track (rejects overlaps).

| Argument | Type | Required | Accepted values |
|---|---|---|---|
| `clipId` | string *uuid* | **yes** | |
| `timelineStartNs` | integer | **yes** | ≥ 0 |

Whether the destination overlaps another clip depends on project state and is enforced by the
command.

Result: `clipId` (string), `timelineStartNs` (integer), *(command result)*.

## `timeline.trim_clip`

Trim a clip's start or end edge to a new source boundary.

| Argument | Type | Required | Accepted values |
|---|---|---|---|
| `clipId` | string *uuid* | **yes** | |
| `edge` | string | **yes** | `start`, `end` |
| `boundaryNs` | integer | **yes** | any; the command *clamps* it into the legal range rather than rejecting it |
| `sourceDurationNs` | integer | no | ≥ 0; defaults to the clip's current out-point |

Result: `clipId` (string), *(command result)*.

## `timeline.split_clip`

Split a clip at an interior playhead into two contiguous clips.

| Argument | Type | Required | Accepted values |
|---|---|---|---|
| `clipId` | string *uuid* | **yes** | |
| `playheadNs` | integer | **yes** | ≥ 0, and strictly inside the clip (enforced by the command) |

Result: `clipId` (string — the left clip), `rightClipId` (string, present when the split produced
one), `status` (`"applied"`). This tool always reports `status`; it has no `noOp` form.

## `timeline.reorder_clips`

Reorder a track's clips into a new sequence (preserves clip count).

| Argument | Type | Required | Accepted values |
|---|---|---|---|
| `trackId` | string *uuid* | **yes** | |
| `order` | array | **yes** | clip UUID strings, a permutation of the track's current clips |

The schema constrains `order` to an array; that every entry is a UUID string is checked by the
handler and that the entries are a permutation of the track's clips by the command.

Result: `trackId` (string), *(command result)*.

## `timeline.add_effect`

Append an effect to a clip's effect chain.

| Argument | Type | Required | Accepted values |
|---|---|---|---|
| `clipId` | string *uuid* | **yes** | |
| `type` | string | **yes** | `brightness`, `contrast`, `blur`, `crop_transform`, `color_grade`, `invert_colors`, `custom` |
| `parameters` | object | no | named numeric effect parameters; non-numeric members are ignored |

An unknown `type` is rejected rather than silently becoming `custom`.

Result: `effectId` (string), *(command result)*.

## `timeline.add_transition`

Set a clip's incoming transition (crossfade, wipe, slide, fade, ...).

| Argument | Type | Required | Accepted values |
|---|---|---|---|
| `clipId` | string *uuid* | **yes** | |
| `kind` | string | **yes** | `crossfade`, `dip_to_color`, `wipe`, `slide`, `fade` |
| `durationNs` | integer | **yes** | ≥ 0 |

Result: `transitionId` (string), *(command result)*.

## `edit.undo`

Revert the most recently applied edit; reports a no-op when the undo history is empty.

No arguments.

Result: `undone` (bool), `indication` (string — the engine's message), `undoDepth` (integer),
`redoDepth` (integer).

An empty history is a **success** with `undone: false` and the engine's indication, not an error, so
a caller can tell "there was nothing to undo" from "the undo failed".

## `edit.redo`

Re-apply the most recently undone edit; reports a no-op when the redo history is empty.

No arguments.

Result: `redone` (bool), `indication` (string), `undoDepth` (integer), `redoDepth` (integer). The
empty-history rule is the same as `edit.undo`'s.

## `generation.generate`

Trigger generative media (image/video) from a prompt and place it. Hook-backed: requires a
configured generative backend, and an unmet account/credential precondition is reported as a
`FailedPrecondition` naming what is missing.

| Argument | Type | Required | Accepted values |
|---|---|---|---|
| `prompt` | string | **yes** | 1–2000 characters |
| `model` | string | **yes** | a model id the configured backend serves |
| `trackId` | string *uuid* | **yes** | the track to place the generated clip on |
| `mediaType` | string | no | `video`, `image`; defaults to `video` |
| `params` | object | no | model-specific **string** parameters; non-string members are ignored |
| `framePosition` | integer | no | ≥ 0, frames from timeline start; defaults to 0 |
| `sourceInTicks` | integer | no | ≥ 0, nanoseconds; defaults to 0 |
| `sourceOutTicks` | integer | no | ≥ 1, nanoseconds, greater than `sourceInTicks` |

Whether `model` is one the backend serves is an open, backend-defined set, and
`sourceOutTicks > sourceInTicks` is a cross-field relation; neither is expressible in the schema, so
both are enforced downstream.

Result: `assetId` (string), `sourcePath` (string), `clipId` (string), `timelineStartTicks`
(integer).

## `timeline.export`

Render the timeline to an output file at a selected container, codec, resolution, frame rate and bit
rate. Hook-backed: requires a configured export engine.

| Argument | Type | Required | Accepted values |
|---|---|---|---|
| `outputPath` | string | **yes** | at least 1 character |
| `format` | string | **yes** | published as a free string; the coordinator accepts `mp4`, `mov`, `mkv`, `webm` |
| `width` | integer | no | 128–3840; defaults to the project canvas width |
| `height` | integer | no | 128–2160; defaults to the project canvas height |
| `codec` | string | no | `h264`, `hevc`, `vp9`; defaults to `vp9` for a `webm` container and `h264` otherwise |
| `fps` | number | no | 1–120; defaults to the project timeline frame rate |
| `bitrateKbps` | integer | no | 100–200000; defaults to 8000 |
| `includeAudio` | boolean | no | defaults to `true` |
| `preferHardware` | boolean | no | defaults to `true` |
| `overwrite` | boolean | no | defaults to `false`, which preserves an existing destination |

Replacing an existing file is never implied: without `overwrite: true` an existing destination is
preserved and the request refused. The container vocabulary, the parent directory's existence and
writability, and whether the destination already exists are all checked by the coordinator, which
owns the message naming the parameter.

Result:

| Field | Type | Notes |
|---|---|---|
| `outputPath` | string | |
| `framesEncoded` | integer | |
| `plannedFrames` | integer | |
| `encoderName` | string | the one selected encoder, e.g. `h264_nvenc` or `libx264` |
| `usedHardwareEncode` | boolean | |
| `usedSoftwareFallback` | boolean | never `true` together with `usedHardwareEncode` |
| `fallbackReason` | string | present only when a fallback reason was recorded |
| `containsAudio` | boolean | |
| `audioFrames` | integer | present only when `containsAudio` is `true` |
| `durationNs` | integer | |
| `projectModified` | boolean | always `false` — the worker runs on a value-copy snapshot |

`preferHardware: true` is a request, not a guarantee. Which encoder actually runs, and what a
`fallbackReason` can say, is [`HARDWARE_ENCODE.md`](HARDWARE_ENCODE.md).

## Next

- Reach the endpoint: [`MCP_CLIENTS.md`](MCP_CLIENTS.md)
- Drive the tools end to end: [`QUICKSTART.md`](QUICKSTART.md)
