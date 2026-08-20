// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/Project.hpp — the authoritative in-memory representation of a project.
//
// The Project is the single source of truth the UI, MCP server, and in-app agent
// all operate on (design.md Data Models / Timeline Engine). It aggregates the
// timeline settings and content:
//   * id          — stable project identity.
//   * name        — human-readable project name.
//   * timelineFps — the timeline's frame rate (must be > 0).
//   * canvas      — the output canvas resolution (width and height must be > 0).
//   * colorSpace  — the working/target color space (defaults to Rec.709).
//   * tracks      — the ordered multi-track lanes of clips.
//   * assets      — the table of referenced media; every Clip.assetRef resolves here.
//   * clipGroups  — clips that a later multicam ripple trim will keep in sync;
//                   reserved by schema 1.1 and not interpreted by any edit today.
//   * version     — the .palmier schema version (must be a supported version).
//
// Validation rules are enforced by ProjectValidation (validateProject): a valid
// timelineFps and canvas, a supported schema version, and every clip's assetRef
// resolving into `assets`, plus each clip's own intrinsic rules.

#ifndef PALMIER_CORE_PROJECT_HPP
#define PALMIER_CORE_PROJECT_HPP

#include <string>
#include <vector>

#include "core/ClipGroup.hpp"
#include "core/ColorSpace.hpp"
#include "core/FrameRate.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/Resolution.hpp"
#include "core/SchemaVersion.hpp"
#include "core/Track.hpp"
#include "core/Uuid.hpp"

namespace palmier {

struct Project {
    Uuid                       id;
    std::string                name;
    FrameRate                  timelineFps;
    Resolution                 canvas;
    ColorSpace                 colorSpace = defaultColorSpace();
    std::vector<Track>         tracks;
    std::vector<MediaAssetRef> assets;
    std::vector<ClipGroup>     clipGroups; ///< Reserved for multicam ripple trim.
    SchemaVersion              version = SchemaVersion::current();
};

} // namespace palmier

#endif // PALMIER_CORE_PROJECT_HPP
