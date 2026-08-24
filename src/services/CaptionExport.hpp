// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/CaptionExport.hpp — sidecar subtitle file rendering (usable-editor
// task 13; Requirement 10.3's second export mode, alongside burn-in).
//
// A Project's TrackKind::Caption clips are turned into an SRT (SubRip) text
// document — every editor and player Requirement 10 could plausibly need to
// interoperate with reads SRT, and it has no binary/container dependencies,
// unlike VTT's WebVTT-specific header conventions or a burned-in track's own
// video pixels. This module holds ONLY the pure text rendering; where the file
// is written (path, naming convention, whether it is written at all for a
// given export) is ExportCoordinator's decision, not this module's.

#ifndef PALMIER_SERVICES_CAPTION_EXPORT_HPP
#define PALMIER_SERVICES_CAPTION_EXPORT_HPP

#include <string>

#include "core/Project.hpp"

namespace palmier::services {

/// True iff `project` has at least one non-muted TrackKind::Caption track
/// carrying at least one caption cue — the condition ExportCoordinator uses to
/// decide whether a sidecar file (and a burned-in caption layer) has anything
/// to contain at all.
[[nodiscard]] bool projectHasCaptions(const Project& project) noexcept;

/// Render every non-muted TrackKind::Caption track's clips, across the whole
/// project, into one SRT document. Cues are numbered in ascending
/// timelineStart order (ties broken by track index, then by clip id, for a
/// deterministic and stable ordering), each with its start/end HH:MM:SS,mmm
/// timestamps (SRT's own comma-separated-milliseconds convention) computed
/// from timelineStart and timelineEnd() exactly as the burned-in layer's own
/// visibility window is computed by Compositor::clipAt — so the two outputs
/// can never disagree about when a cue is on screen (Requirement 10.3's "the
/// two SHALL agree on timing").
///
/// A caption clip carrying no captionText (which ProjectValidation forbids)
/// is skipped rather than emitted as an empty cue, mirroring
/// Compositor::gatherVisibleCaptionCues' own defensive posture.
[[nodiscard]] std::string renderSrt(const Project& project);

}  // namespace palmier::services

#endif  // PALMIER_SERVICES_CAPTION_EXPORT_HPP
