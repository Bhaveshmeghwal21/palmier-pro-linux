// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/ProjectValidation.hpp — validation of the project data model.
//
// The design attaches validation rules to the data model (design.md Data Models
// "Validation rules"). This header exposes those rules as fallible, UI-agnostic
// checks returning Result<void>: an ok Result means the entity satisfies every
// rule; an error Result carries a coarse ErrorCode plus a human-readable message
// naming the offending field/value. These are pure predicates with no side
// effects, usable identically by the UI, the MCP server, persistence (task 5.x),
// and the TimelineEngine (task 3.x).
//
// Rules enforced:
//   Clip    — sourceOut > sourceIn; opacity in [0, 1]; gain >= 0; a non-negative
//             transitionIn duration when present.
//   Track   — every contained Clip is valid.
//   Project — timelineFps > 0; canvas.width > 0 && canvas.height > 0; version is a
//             known, supported schema version; every track is valid; and every
//             Clip.assetRef resolves to an entry in Project.assets.

#ifndef PALMIER_CORE_PROJECTVALIDATION_HPP
#define PALMIER_CORE_PROJECTVALIDATION_HPP

#include "core/Clip.hpp"
#include "core/Project.hpp"
#include "core/Result.hpp"
#include "core/Track.hpp"

namespace palmier {

/// Validate a clip's intrinsic rules (independent of any containing project):
/// sourceOut > sourceIn, opacity in [0, 1], gain >= 0, and — if present — a
/// non-negative transitionIn duration.
[[nodiscard]] Result<void> validateClip(const Clip& clip);

/// Validate a track: currently that every contained clip is intrinsically valid.
/// Asset resolution is a project-level rule (see validateProject).
[[nodiscard]] Result<void> validateTrack(const Track& track);

/// Validate an entire project against every data-model rule. Returns the first
/// violation encountered (fields are checked project-level first, then per track
/// and per clip, then asset resolution).
[[nodiscard]] Result<void> validateProject(const Project& project);

} // namespace palmier

#endif // PALMIER_CORE_PROJECTVALIDATION_HPP
