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
`unknown field '<name>' is not accepted by this tool` — including on the six tools that take no
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

**Four tools have no built-in implementation** and depend on a collaborator the composition root
installs. Absent it they return `Unsupported`, after the no-project check:
`media.import is not available: no media import service is configured`,
`generation.generate is not available: no generative backend is configured`,
`generation.list_models is not available: no model catalog is configured`,
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

## `project.set_settings`

Change the current project's frame rate, canvas and/or colour space after creation, accepting the
same ranges `project.create` accepts (usable-editor Phase 3 task 10; Requirement 7).

| Argument | Type | Required | Accepted values |
|---|---|---|---|
| `fps` | number | no | 1..240 |
| `width` | integer | no | 16..7680; must be given together with `height` |
| `height` | integer | no | 16..4320; must be given together with `width` |
| `colorSpace` | string | no | the same closed set `project.create`'s `colorSpace` accepts |

Every argument is optional, but at least one of `fps`, `width`/`height` or `colorSpace` must be
given. A field left out is left exactly as it was; any combination that IS supplied changes in one
undoable edit. No clip is touched by an `fps` change: every clip's timeline position and source
range is stored as an absolute duration with no embedded frame rate, so there is nothing to migrate.

Result: *(command result)*. `fps` (object, present only when changed), `canvas` (object, present
only when changed) and `colorSpace` (string, present only when changed) are each reported only when
that particular setting was the one changed.

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

Result: `assets` (array of `{ "assetId": string, "sourcePath": string, "displayName": string,
"tags": array of string }`, where `displayName` is the source path's filename) and `count`
(integer).

## `media.set_tags`

Replace a media asset's tag list wholesale (usable-editor tasks.md task 15; no dedicated
Requirement). Not an undoable edit: tags are informational project state, like an asset's
`sourcePath`, not a timeline edit — they do not appear in the undo/redo history.

| Argument | Type | Required | Accepted values |
|---|---|---|---|
| `assetId` | string *uuid* | **yes** | must name an asset in `Project.assets` |
| `tags` | array | **yes** | the asset's complete new tag list (strings); replaces any existing tags |

Result: `assetId` (string).

## `timeline.add_track`

Append a video or audio track after the last existing track of that kind.

| Argument | Type | Required | Accepted values |
|---|---|---|---|
| `kind` | string | **yes** | `video`, `audio`, `text`, `caption` |

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

## `timeline.set_clip_properties`

Change a clip's opacity or audio gain after it has been created.

| Argument | Type | Required | Accepted values |
|---|---|---|---|
| `clipId` | string *uuid* | **yes** | |
| `opacity` | number | no | 0.0 .. 1.0 |
| `gain` | number | no | >= 0, unity = 1.0 |
| `centerX` | number | no | -10.0 .. 10.0; picture centre across the canvas, 0.5 is the middle |
| `centerY` | number | no | -10.0 .. 10.0; picture centre down the canvas |
| `scale` | number | no | 0.001 .. 100.0; multiplies the aspect-preserving fitted size |
| `rotationDegrees` | number | no | -360.0 .. 360.0, clockwise about the picture's own centre |
| `flipHorizontal` | boolean | no | mirror left to right |
| `flipVertical` | boolean | no | mirror top to bottom |
| `fadeInNs` | integer | no | >= 0; ramp up over this long at the clip's leading edge |
| `fadeOutNs` | integer | no | >= 0; ramp down over this long at the trailing edge |
| `speed` | number | no | 0.01 .. 100.0; playback rate, 2.0 plays twice as fast |

Result: `clipId` (string), `opacity` (number), `gain` (number), *(command result)*.

An absent argument leaves that property unchanged, and a call naming none is an accepted
no-op. `opacity` and `gain` appear in the result only when they were given.

**Placement.** A clip's default placement fits it to the canvas preserving aspect ratio and
centres it, which is what `centerX` 0.5, `centerY` 0.5, `scale` 1.0 mean. `scale` multiplies
that fitted size, so 0.5 is a half-size inset suitable for picture-in-picture. Scale is
uniform: upstream Palmier Pro carries independent `width` and `height` and so also permits a
non-uniform stretch, whereas a single factor cannot distort a source by accident. A
split-screen half is expressed here by cropping the source with `crop_transform` and moving
what remains with `centerX`.

**Speed.** `speed` rescales how much timeline the clip occupies for the same span of source: at
2.0 it plays twice as fast and is half as long, at 0.5 half as fast and twice as long. Its
audio changes with it, pitch included, the way a tape played faster does; there is no pitch
correction. Trimming, splitting and rippling all keep working in their own space, so a split
one timeline second into a clip at speed 2 cuts the source two seconds in.

**Fades.** `fadeInNs` and `fadeOutNs` ramp a clip's own edges, attenuating both its picture
and its audio by the identical curve, computed from one function so the two cannot drift
apart. They are independent of `timeline.add_transition`: a transition belongs to the boundary
between two clips and is stored on the incoming one, whereas a fade belongs to one clip's own
edges and needs no neighbour, so a clip may carry both and is then attenuated by both. A fade
longer than half the clip is clamped, so two overlapping ramps cannot produce a clip that
never reaches full brightness.

Both properties can also be supplied to `timeline.add_clip`. This tool exists because they
could previously *only* be supplied there, which made them write-once: adjusting a composite's
blend or a music bed's level meant deleting the clip and adding it again, losing its identifier
along with any effects, transition or text style attached to it.


## `timeline.apply_layout`

Arrange several clips on the canvas in a named layout.

| Argument | Type | Required | Accepted values |
|---|---|---|---|
| `clipIds` | array | **yes** | clip UUID strings, in the order they should be placed; at least one |
| `layout` | string | **yes** | `side_by_side`, `picture_in_picture`, `grid` |

Result: `clipCount` (integer), *(command result)*.

One edit covering every named clip, so undoing an arrangement restores all of their placements
together. Refused with the project unchanged if any named clip does not exist, checked before the
first clip is moved so a bad identifier cannot leave a half-applied arrangement.

| Layout | Arrangement |
|---|---|
| `side_by_side` | Equal columns across the middle, each scaled to `1/n`. A single clip is left filling the frame. |
| `picture_in_picture` | The first clip keeps the frame; the rest are quarter-size insets walking the corners clockwise from the top right. A fifth clip returns to the first corner rather than shrinking further. |
| `grid` | The squarest grid that holds them all, filled left to right and top to bottom, every cell at one uniform scale. |

Every layout positions and scales only, so each participant keeps its aspect ratio and is
letterboxed within its share of the canvas. A full-bleed split screen, where each half fills its
side edge to edge, additionally needs the source cropped with a `crop_transform` effect: scaling
is uniform and cannot stretch a 16:9 source into a half-width box.

## `timeline.set_keyframes`

Animate one of a clip's properties over the timeline.

| Argument | Type | Required | Accepted values |
|---|---|---|---|
| `clipId` | string *uuid* | **yes** | |
| `property` | string | **yes** | `opacity`, `gain`, `centerX`, `centerY`, `scale`, `rotationDegrees` |
| `keyframes` | array | **yes** | objects, each with `timeNs` (integer >= 0) and `value` (number), plus an optional `interpolation` of `linear` or `smooth` |

Result: `clipId` (string), `property` (string), `keyframeCount` (integer), *(command result)*.

Replaces the named property's whole keyframe list, so sending the same list twice leaves the same
animation and a caller never has to reason about the identity of a keyframe it did not create. **An
empty array clears the animation**, returning the property to the clip's static value, which is the
only way back.

`timeNs` is a **timeline** position, not an offset into the clip, so it is independent of the clip's
speed: changing a clip's speed does not redistribute its keyframes. Keyframes given out of order are
sorted, and two sharing an instant collapse to the last given, because a property cannot hold two
values at one moment.

Outside the keyframed span the nearest value holds — before the first keyframe the first value,
after the last the last — so a property never falls to an implied zero beyond the keyframes you set.

A property with no keyframes uses the clip's static value from `timeline.set_clip_properties`, so an
unanimated clip behaves exactly as it did before this tool existed. Keyframed `opacity` multiplies
with any edge fade and any transition rather than replacing them.

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

## `timeline.ripple_delete`

Delete a clip and shift every later clip on the same track earlier by the removed clip's duration,
closing the gap it leaves (usable-editor Phase 3 task 8; Requirement 5.1).

| Argument | Type | Required | Accepted values |
|---|---|---|---|
| `clipId` | string *uuid* | **yes** | |

How far the track closes up is not a caller's choice: it is exactly the removed clip's own duration.
Clips on other tracks keep their absolute positions.

Result: `clipId` (string), *(command result)*.

## `timeline.ripple_trim`

Trim a clip's edge and shift every later clip on the same track by the change in duration, so the
trim leaves neither a gap nor an overlap (usable-editor Phase 3 task 8; Requirement 5.2).

| Argument | Type | Required | Accepted values |
|---|---|---|---|
| `clipId` | string *uuid* | **yes** | |
| `edge` | string | **yes** | `start`, `end` |
| `boundaryNs` | integer | **yes** | any; the command *clamps* it into the legal range rather than rejecting it |
| `sourceDurationNs` | integer | no | ≥ 0; defaults to the clip's current out-point |

`end` moves the out-point and leaves the clip's leading edge fixed, so later clips move by the
change in duration. `start` moves the in-point and shifts the clip's leading edge by the same
amount, leaving the trailing edge — and therefore every later clip — where it was.

When the trimmed clip belongs to a `clipGroups` entry, the identical source-time trim is applied to
every other member of that group and each member's own track is rippled the same way, so grouped
multicam angles that started aligned stay aligned (upstream PR 397). A grouped clip that cannot
absorb the same trim while keeping one frame inside its source refuses the whole operation, leaving
the project unchanged.

Result: `clipId` (string), *(command result)*.

## `timeline.close_gap`

Close the gap immediately following a clip by shifting later clips on the same track earlier,
leaving every clip's duration unchanged (usable-editor Phase 3 task 8; Requirement 5.5).

| Argument | Type | Required | Accepted values |
|---|---|---|---|
| `clipId` | string *uuid* | **yes** | |

Only positions change; no duration or source range is touched. The operation is refused, with the
project unchanged, when the clip is the last on its track or when its successor already begins at or
before the clip's end — in both cases there is no gap to close.

Result: `clipId` (string), *(command result)*.

## `timeline.add_effect`

Append an effect to a clip's effect chain.

| Argument | Type | Required | Accepted values |
|---|---|---|---|
| `clipId` | string *uuid* | **yes** | |
| `type` | string | **yes** | `brightness`, `contrast`, `blur`, `crop_transform`, `color_grade`, `invert_colors`, `tone_curve`, `lut`, `custom` |
| `parameters` | object | no | named numeric effect parameters; non-numeric members are ignored |

An unknown `type` is rejected rather than silently becoming `custom`.

Result: `effectId` (string), *(command result)*.

### Effect parameters

Each effect type reads a fixed set of numeric parameters. **A name the effect does not read is
silently ignored** — there is no error — so a misspelled or invented parameter renders as if it
had never been passed. The defaults below are what an absent parameter falls back to.

| Type | Parameter | Default | Meaning |
|---|---|---|---|
| `brightness` | `amount` | 0.0 | Added to each channel, as a fraction of full scale. |
| `contrast` | `amount` | 0.0 | Applied as `1.0 + amount`, so `0.15` is +15% and `1.0` is +100%. |
| `blur` | `radius` | 0.0 | Blur radius in source pixels. |
| `crop_transform` | `cropLeft` | 0.0 | Left edge of the visible rectangle. |
| | `cropTop` | 0.0 | Top edge. |
| | `cropRight` | 1.0 | Right edge. |
| | `cropBottom` | 1.0 | Bottom edge. |
| `color_grade` | `liftR` `liftG` `liftB` | 0.0 | Per-channel lift; the legacy scalar `lift` applies to all three. |
| | `gammaR` `gammaG` `gammaB` | 1.0 | Per-channel gamma. |
| | `gainR` `gainG` `gainB` | 1.0 | Per-channel gain. |
| | `saturation` | 1.0 | 0.0 is monochrome, 1.0 unchanged. |
| `invert_colors` | — | | No parameters. |
| `tone_curve` | *(written by `timeline.edit_curve_point`)* | | Do not set these by hand. |
| `lut` | *(the cube file is set by `timeline.set_effect_resource`)* | | An unreadable path renders un-graded. |
| `custom` | *(any)* | | Carries parameters for a kernel this build does not implement. |

There is no `temperature`, `tint`, `scale`, `rotation` or `opacity` among these. Clip opacity is
a property of the clip, set by `timeline.add_clip` or `timeline.set_clip_properties`.

#### `crop_transform` coordinates

`cropLeft`/`cropTop`/`cropRight`/`cropBottom` are **absolute normalized edges** of the visible
rectangle, not insets: `{cropLeft: 0.5, cropRight: 1.0}` keeps the right half, and the defaults
`0, 0, 1, 1` keep everything. Everything outside the rectangle becomes transparent, so it shows
whatever is on a lower track or the canvas's clear colour.

The coordinates are relative to the **source frame**, and the source is fitted to the canvas
before compositing, so the same values land in the same place on the canvas whatever the source's
resolution. A right-half crop of a 1920x1080 source and of a 3840x2160 source both occupy canvas
x 0.500..1.000.

> **Divergence from upstream Palmier Pro.** Upstream's `crop` names the same four edges but means
> **insets from each edge**, where all zeros keep the full source. The two conventions are
> inverses of each other: `{left: 0.5, right: 0.05}` keeps the middle in upstream and would be an
> almost-empty rectangle here, while this port's defaults (`0, 0, 1, 1`) would be read upstream as
> cropping everything but a sliver. An agent written against upstream's semantics must convert.

## `timeline.remove_effect`

Remove one effect from a clip's effect chain by id (usable-editor Phase 3 task 9; Requirement 6.1).

| Argument | Type | Required | Accepted values |
|---|---|---|---|
| `clipId` | string *uuid* | **yes** | |
| `effectId` | string *uuid* | **yes** | |

Refused, with the project unchanged, when the clip or the named effect is not found.

Result: `clipId` (string), `effectId` (string), *(command result)*.

## `timeline.reorder_effects`

Reorder a clip's effect chain (usable-editor Phase 3 task 9; Requirement 6.1).

| Argument | Type | Required | Accepted values |
|---|---|---|---|
| `clipId` | string *uuid* | **yes** | |
| `order` | array | **yes** | effect UUID strings, a permutation of the clip's current effects |

The schema constrains `order` to an array; that every entry is a UUID string is checked by the
handler and that the entries are a permutation of the clip's effects by the command. The rendered
result of the effect chain depends on this order (Requirement 6.4): unlike `timeline.reorder_clips`,
no field of an effect is recomputed by the reorder itself, since an effect carries no timeline
geometry of its own.

Result: `clipId` (string), *(command result)*.

## `timeline.set_effect_parameter`

Set, or insert, a named numeric parameter on one of a clip's effects (usable-editor Phase 3 task 9;
Requirement 6.1).

| Argument | Type | Required | Accepted values |
|---|---|---|---|
| `clipId` | string *uuid* | **yes** | |
| `effectId` | string *uuid* | **yes** | |
| `parameter` | string | **yes** | |
| `value` | number | **yes** | |

Refused, with the project unchanged, when the clip or the named effect is not found. A parameter
that did not previously exist on the effect is created; undoing removes it entirely rather than
leaving it at `0`.

Result: `clipId` (string), `effectId` (string), *(command result)*.

## `timeline.edit_curve_point`

Add, move or remove one control point on a tone-curve effect's master, red, green or blue curve, as
a single undoable edit (monitoring-and-grading Requirement 5.7).

| Argument | Type | Required | Accepted values |
|---|---|---|---|
| `clipId` | string *uuid* | **yes** | |
| `effectId` | string *uuid* | **yes** | |
| `channel` | string | **yes** | `master`, `red`, `green`, `blue` |
| `operation` | string | **yes** | `add`, `move`, `remove` |
| `index` | integer | for `move`/`remove` | ≥ 0 |
| `x` | number | for `add`/`move` | 0.0 – 1.0 |
| `y` | number | for `add`/`move` | 0.0 – 1.0 |

`add` appends a point; `index` is ignored, because rendering sorts the points by `x` regardless, so
an insertion position would be a distinction without a difference. A point's index *is* its identity
for a later `move` or `remove`, which is why those two require one and refuse rather than defaulting
to point `0`.

`remove` renumbers the points after the removed one so the indices stay contiguous, which matters
because a curve is read as a contiguous run and stops at the first gap. Undo restores the whole
channel — including the numbering — in one step.

Refused, with the project unchanged, when the clip or effect is not found, or when `index` names a
point the chosen curve does not have.

Result: `clipId` (string), `effectId` (string), `channel` (string), `operation` (string), *(command result)*.

## `timeline.set_effect_resource`

Point one of a clip's effects at an external resource file, such as a `.cube` LUT, or clear it with an
empty path (monitoring-and-grading Requirement 7.9).

| Argument | Type | Required | Accepted values |
|---|---|---|---|
| `clipId` | string *uuid* | **yes** | |
| `effectId` | string *uuid* | **yes** | |
| `path` | string | **yes** | a filesystem path, or `""` to clear |

The path is **not** checked for existence. A referenced LUT that is missing or unreadable leaves the
effect in the chain and renders the clip un-graded, reporting the path — it does not fail the project
open, drop the effect, or block editing. Refusing an unreadable path here would instead make a project
that opens on one machine un-editable on another.

Clearing the path removes the LUT without removing the effect. Undo restores the previous path,
including when that path was empty — which is the common case, since an effect just added has none.

Result: `clipId` (string), `effectId` (string), `path` (string), *(command result)*.

## `timeline.add_transition`

Set a clip's incoming transition (crossfade, wipe, slide, fade, ...).

| Argument | Type | Required | Accepted values |
|---|---|---|---|
| `clipId` | string *uuid* | **yes** | |
| `kind` | string | **yes** | `crossfade`, `dip_to_color`, `wipe`, `slide`, `fade` |
| `durationNs` | integer | **yes** | ≥ 0 |

Result: `transitionId` (string), *(command result)*.

The transition region is `[clip.timelineStart, clip.timelineStart + durationNs)` — at the
**incoming** clip's leading edge — and it is the one place two clips on a track may overlap.

**Arranging an overlap takes three calls, in this order.** `timeline.add_clip` refuses an
overlapping placement with *"clips overlap outside an explicit transition region"*, and a
transition can only be attached to a clip that already exists, so add the clip contiguously
first, attach the transition, then move it back over its neighbour:

```jsonc
{"tool": "timeline.add_clip",       "args": {"timelineStartNs": 2000000000, "...": "..."}}
{"tool": "timeline.add_transition", "args": {"clipId": "<id>", "kind": "crossfade",
                                             "durationNs": 1000000000}}
{"tool": "timeline.move_clip",      "args": {"clipId": "<id>", "timelineStartNs": 1000000000}}
```

What each kind does over the region, where *p* runs 0 to 1 across it:

| Kind | Behaviour |
|---|---|
| `crossfade` | The incoming clip's coverage ramps 0 to 1, dissolving over whatever is beneath. |
| `fade` | Identical arithmetic; with no overlapping neighbour this is a fade up from the render target's clear colour. |
| `dip_to_color` | The outgoing clip fades out over the first half and the incoming rises over the second, so the frame passes through the clear colour at the midpoint. |
| `wipe` | A hard vertical edge travels left to right, revealing the incoming clip. |
| `slide` | The incoming clip travels in from the right edge at full coverage. |

An overlap is not required: a transition on a clip with no neighbour blends against the clear
colour, which is what makes `fade` useful at the head of a track. `dip_to_color` dips to the
render target's clear colour because `Transition` carries no colour field of its own yet.

## `timeline.add_text_clip`

Add a text clip — a title or lower third — onto a text track at a timeline position
(usable-editor Phase 4 task 12; Requirement 9). The clip carries no source media: it is placed
through the same `AddClipCommand` every media clip uses, but with no asset reference, and it must
land on a `TrackKind::Text` track (create one with `timeline.add_track` and `kind: "text"`).

| Argument | Type | Required | Accepted values |
|---|---|---|---|
| `trackId` | string *uuid* | **yes** | must name a text track |
| `clipId` | string *uuid* | no | generated when omitted |
| `timelineStartNs` | integer | no | ≥ 0; default 0 |
| `durationNs` | integer | **yes** | ≥ 1 |
| `content` | string | **yes** | |
| `fontFamily` | string | no | default `sans-serif`; substituted and reported when unavailable |
| `pointSize` | number | no | ≥ 1; default 24 |
| `colorR` | number | no | 0..1; default 1 |
| `colorG` | number | no | 0..1; default 1 |
| `colorB` | number | no | 0..1; default 1 |
| `colorA` | number | no | 0..1; default 1 |
| `alignment` | string | no | `left`, `center`, `right`; default `center` |
| `x` | number | no | 0..1 (normalized anchor); default 0.5 |
| `y` | number | no | 0..1 (normalized anchor); default 0.5 |

Refused, with the project unchanged, when the resulting style is not internally well-formed (a
non-positive `pointSize`, or a colour/position value outside its declared range would already be
rejected by the declared bounds above).

Result: `clipId` (string), *(command result)*.

## `timeline.set_text_content`

Change a text clip's displayed string (usable-editor Phase 4 task 12; Requirement 9.2).

| Argument | Type | Required | Accepted values |
|---|---|---|---|
| `clipId` | string *uuid* | **yes** | must name a text clip |
| `content` | string | **yes** | |

Refused, with the project unchanged, when the named clip does not exist or is not a text clip.

Result: `clipId` (string), *(command result)*.

## `timeline.set_text_style`

Change a text clip's font, size, colour, alignment and/or screen position (usable-editor Phase 4
task 12; Requirement 9.2). Every argument but `clipId` is optional; a field left out is left exactly
as it was, and any combination that IS supplied changes in one undoable edit.

| Argument | Type | Required | Accepted values |
|---|---|---|---|
| `clipId` | string *uuid* | **yes** | must name a text clip |
| `fontFamily` | string | no | |
| `pointSize` | number | no | ≥ 1 |
| `colorR` | number | no | 0..1 |
| `colorG` | number | no | 0..1 |
| `colorB` | number | no | 0..1 |
| `colorA` | number | no | 0..1 |
| `alignment` | string | no | `left`, `center`, `right` |
| `x` | number | no | 0..1 (normalized anchor) |
| `y` | number | no | 0..1 (normalized anchor) |

Refused, with the project unchanged, when the named clip does not exist or is not a text clip.

Result: `clipId` (string), *(command result)*.

## `timeline.add_caption_cue`

Add a caption cue onto a caption track at a timeline position (usable-editor Phase 4 task 13;
Requirement 10). The cue carries no source media and no styling of its own: it is placed through
the same `AddClipCommand` every media/text clip uses, but with no asset reference, and it must land
on a `TrackKind::Caption` track (create one with `timeline.add_track` and `kind: "caption"`).

| Argument | Type | Required | Accepted values |
|---|---|---|---|
| `trackId` | string *uuid* | **yes** | must name a caption track |
| `clipId` | string *uuid* | no | generated when omitted |
| `timelineStartNs` | integer | no | ≥ 0; default 0 |
| `durationNs` | integer | **yes** | ≥ 1 |
| `text` | string | **yes** | must not be empty |

Result: `clipId` (string), *(command result)*.

## `timeline.set_caption_text`

Change a caption cue's displayed text.

| Argument | Type | Required | Accepted values |
|---|---|---|---|
| `clipId` | string *uuid* | **yes** | must name a caption cue |
| `text` | string | **yes** | must not be empty |

Refused, with the project unchanged, when the named clip does not exist, is not a caption cue, or
`text` is empty.

Result: `clipId` (string), *(command result)*.

## `timeline.retime_caption_cue`

Change a caption cue's start position and/or duration in one undoable edit. At least one of
`timelineStartNs` or `durationNs` must be given; either changes in the same single edit.

| Argument | Type | Required | Accepted values |
|---|---|---|---|
| `clipId` | string *uuid* | **yes** | must name a caption cue |
| `timelineStartNs` | integer | no | ≥ 0 |
| `durationNs` | integer | no | ≥ 1 |

Refused, with the project unchanged, when neither argument is given, when the resulting duration
would be non-positive, or when the new timing would overlap another cue on the same track.

Result: `clipId` (string), *(command result)*.

## `timeline.remove_caption_cue`

Remove a caption cue by id from whichever track holds it.

| Argument | Type | Required | Accepted values |
|---|---|---|---|
| `clipId` | string *uuid* | **yes** | |

Result: `clipId` (string), *(command result)*.

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
| `mediaType` | string | no | `video`, `image`, `audio`; defaults to `video` |
| `mode` | string | no | `generate`, `upscale`; defaults to `generate` |
| `sourceAssetId` | string *uuid* | no | the clip to upscale (`mode: "upscale"`), or the clip audio is generated FROM rather than from `prompt` alone |
| `targetWidth` | integer | no | ≥ 1, requested output width in pixels for `mode: "upscale"` |
| `targetHeight` | integer | no | ≥ 1, requested output height in pixels for `mode: "upscale"` |
| `requestedDurationTicks` | integer | no | ≥ 1, nanoseconds, requested audio duration for `mediaType: "audio"` |
| `params` | object | no | model-specific **string** parameters; non-string members are ignored |
| `framePosition` | integer | no | ≥ 0, frames from timeline start; defaults to 0 |
| `sourceInTicks` | integer | no | ≥ 0, nanoseconds; defaults to 0 |
| `sourceOutTicks` | integer | no | ≥ 1, nanoseconds, greater than `sourceInTicks` |

Whether `model` is one the backend serves is an open, backend-defined set, and
`sourceOutTicks > sourceInTicks` is a cross-field relation; neither is expressible in the schema, so
both are enforced downstream. The model catalog (`generation.list_models`) is what makes `model`,
`mode: "upscale"` and an audio duration range checkable at all: naming a model absent from the
catalog, a mode a named model does not serve, or an audio duration outside a named model's declared
range is refused before submission, naming the offending value.

Result: `assetId` (string), `sourcePath` (string), `clipId` (string), `timelineStartTicks`
(integer).

## `generation.list_models`

List every model in the generation model catalog, grouped by provider (usable-editor Phase 2 task 7;
PR 406).

No arguments.

Result: `providers` (object) — each member key is a provider name and its value is an array of that
provider's models. Each model entry carries the fields id, mediaType and servesUpscale and, only for an
audio-generation model, minDurationTicks and maxDurationTicks (integer nanosecond values).

## `timeline.transcribe_to_captions`

Transcribe a clip's audio and place the resulting segments as caption cues on a caption track
(usable-editor Phase 4 task 13; Requirement 10.4). Hook-backed: requires a configured recognizer
backend; absent one, refused by name — hand-authored caption editing through the other four caption
tools remains available either way (Requirement 10.5).

| Argument | Type | Required | Accepted values |
|---|---|---|---|
| `sourceClipId` | string *uuid* | **yes** | must name a clip with decodable audio |
| `captionTrackId` | string *uuid* | **yes** | must name a caption track |

A clip with no detectable audio track produces zero cues and `status: "no_audio_found"` rather than
an error (mirroring `services::TranscriptionService`'s own Requirement 4.4 indication) — a caption
track with nothing to transcribe is a normal outcome, not a failure.

Result: `status` (string — `"transcribed"` or `"no_audio_found"`), `cuesAdded` (integer).

## `timeline.capture_frame`

Write the frame at a given timeline position to an image file (usable-editor tasks.md task 14; no
dedicated Requirement). Hook-backed: requires a configured image encoder.

| Argument | Type | Required | Accepted values |
|---|---|---|---|
| `outputPath` | string | **yes** | at least 1 character |
| `positionNs` | integer | **yes** | ≥ 0 |

The written image is rendered through the SAME `gpu::Compositor` instance the live preview renders
through, so it matches the preview frame at that position rather than merely resembling it.

Result: `outputPath` (string).

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

Any `TrackKind::Caption` cue in the project is burned into the exported video unconditionally (no
argument gates this) and is also written out as a sidecar SubRip (`.srt`) file next to `outputPath`
(same base name, `.srt` extension); the two derive their timing from the identical clip fields, so
they cannot disagree about when a cue is on screen. A project with no caption cues writes no
sidecar file and reports no `captionsSidecarPath`.

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
| `captionsSidecarPath` | string | present only when the exported project had at least one non-muted caption cue and the sidecar `.srt` file was written successfully alongside `outputPath` |

`preferHardware: true` is a request, not a guarantee. Which encoder actually runs, and what a
`fallbackReason` can say, is [`HARDWARE_ENCODE.md`](HARDWARE_ENCODE.md).

## Next

- Reach the endpoint: [`MCP_CLIENTS.md`](MCP_CLIENTS.md)
- Drive the tools end to end: [`QUICKSTART.md`](QUICKSTART.md)
