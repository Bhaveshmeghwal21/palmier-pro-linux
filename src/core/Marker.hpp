// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/Marker.hpp — a named point on the timeline (upstream parity: the `timeline`
// tool category's marker gap).
//
// A Marker is deliberately the simplest thing that closes the gap: a stable id, a
// timeline position, and a label. It does not belong to a track or a clip — it
// marks a MOMENT on the shared timeline, the same way a marker in every other NLE
// does, so it survives a clip being moved, trimmed or deleted around it.
//
// Persisted at Project level (schema 1.9) beside clipGroups, following the same
// additive-and-ignorable pattern MediaAssetRef::tags established at 1.4: a project
// carrying no "markers" key at all (every 1.0-1.8 document) reads with an empty
// list, and an older reader that does not know the key simply ignores it. Markers
// change nothing about how a clip renders or plays, so there is nothing for an
// older reader to silently get wrong by skipping them — unlike an effect kind or a
// track kind, dropping a marker loses an annotation, not a picture.

#ifndef PALMIER_CORE_MARKER_HPP
#define PALMIER_CORE_MARKER_HPP

#include <string>

#include "core/Duration.hpp"
#include "core/Uuid.hpp"

namespace palmier {

struct Marker {
    Uuid id;                                 ///< Stable per-marker identity.
    Duration timelineNs = Duration::zero();  ///< Absolute timeline position.
    std::string label;                       ///< Human-readable annotation; may be empty.
};

}  // namespace palmier

#endif  // PALMIER_CORE_MARKER_HPP
