// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/Track.hpp — one lane of the multi-track timeline.
//
// A Project holds an ordered list of Tracks; each Track holds a list of Clips
// (design.md Data Models). A track is a Video, Audio or Text lane (the last
// added by usable-editor task 12; Requirement 9 — see core/TextStyle.hpp for why
// text needs its own track kind), may be muted or locked, and owns its clips.
// The design's clip-ordering invariant requires a track's clips to be sorted by
// timelineStart and non-overlapping outside explicit transition regions; that
// invariant is enforced by the TimelineEngine (task 3.x) and checked by
// ProjectValidation, uniformly across every kind of track.

#ifndef PALMIER_CORE_TRACK_HPP
#define PALMIER_CORE_TRACK_HPP

#include <string>
#include <vector>

#include "core/Clip.hpp"
#include "core/Uuid.hpp"

namespace palmier {

/// Whether a track carries picture, sound, or on-screen text (Requirement 9).
enum class TrackKind {
    Video,
    Audio,
    Text,
};

struct Track {
    Uuid              id;
    TrackKind         kind = TrackKind::Video;
    /// Human-readable lane label shown in the timeline panel. Empty means "no
    /// explicit label"; the panel then falls back to a kind-and-index caption.
    /// Persisted from schema 1.1 onward and optional on read (default "").
    std::string       name;
    bool              muted = false;
    bool              locked = false;
    std::vector<Clip> clips; ///< Sorted by timelineStart, non-overlapping.
};

} // namespace palmier

#endif // PALMIER_CORE_TRACK_HPP
