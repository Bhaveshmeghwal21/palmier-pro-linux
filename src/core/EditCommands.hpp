// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/EditCommands.hpp - the concrete, undoable timeline editing commands (task 3.3).
//
// These are the EditCommand subclasses that realize the interactive editing
// operations of the Timeline_Editor (design.md Component 1; Requirement 2). Every
// one of them mutates a Project through the same apply()/revert() contract the
// engine drives, so the UI, the MCP server, and the in-app agent all share one
// undoable, observable editing path (design.md "All mutations flow through a
// Command object").
//
// This header is an umbrella over one header per command family. It used to declare all
// 27 commands itself, which is how it reached a thousand lines, and it carried a summary
// list that had gone stale -- the list described 8 of them. Each command is now documented
// where it is declared, and the families are:
//
//   * core/ClipEditCommands.hpp            - clip placement, trimming and splitting
//   * core/RippleEditCommands.hpp          - edits that shift everything after them
//   * core/TrackEditCommands.hpp           - adding, removing and muting tracks
//   * core/EffectEditCommands.hpp          - effects, per-clip properties, transforms and transitions
//   * core/TextCaptionEditCommands.hpp     - text clips and caption cues
//   * core/ProjectSettingsEditCommands.hpp - project-wide settings
//   * core/MarkerEditCommands.hpp          - adding and removing timeline markers
//
// Atomicity / invariants: the TimelineEngine snapshots the project before every
// apply() and rolls back on failure or on any timeline-invariant violation (no
// negative/zero-length clips; each track ordered by timelineStart and
// non-overlapping outside a transition region). Each command is written to
// leave the project unchanged on the failure paths it detects itself, and to
// produce an invariant-satisfying result on success. Every apply() captures the
// exact prior state it needs so a subsequent revert() is an exact inverse and so
// redo (a second apply() from the restored state) reproduces the same result.

#ifndef PALMIER_CORE_EDITCOMMANDS_HPP
#define PALMIER_CORE_EDITCOMMANDS_HPP

#include "core/ClipEditCommands.hpp"
#include "core/RippleEditCommands.hpp"
#include "core/TrackEditCommands.hpp"
#include "core/EffectEditCommands.hpp"
#include "core/TextCaptionEditCommands.hpp"
#include "core/ProjectSettingsEditCommands.hpp"
#include "core/MarkerEditCommands.hpp"

#endif  // PALMIER_CORE_EDITCOMMANDS_HPP
