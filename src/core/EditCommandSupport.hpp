// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/EditCommandSupport.hpp — the internal helpers the edit commands share.
//
// core/EditCommands.cpp held all 24 command implementations plus their shared
// helpers in one 1583-line translation unit. The commands are now grouped one unit
// per subject, which is only possible because they are DECLARED in the public
// core/EditCommands.hpp: their member definitions already have external linkage and
// moved untouched. The helpers did not — they sat in two anonymous namespaces — so
// the ones used by more than one subject are declared here instead.
//
// Private by intent: it lives beside its .cpp files, `detail` says so in the name,
// and nothing outside src/core includes it. The public contract is unchanged and
// remains core/EditCommands.hpp.

#ifndef PALMIER_CORE_EDITCOMMANDSUPPORT_HPP
#define PALMIER_CORE_EDITCOMMANDSUPPORT_HPP

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/Clip.hpp"
#include "core/Duration.hpp"
#include "core/Effect.hpp"
#include "core/Error.hpp"
#include "core/Project.hpp"
#include "core/Track.hpp"
#include "core/Uuid.hpp"

namespace palmier::detail {

// Locate a track by id. Returns nullptr when no track carries the id.
Track* findTrack(Project& project, const Uuid& trackId);

// A located clip: the track that holds it and its index within that track.
struct ClipLocation {
    Track* track = nullptr;
    std::size_t index = 0;
};

// Locate a clip by id across every track. Returns std::nullopt when not found.
std::optional<ClipLocation> findClip(Project& project, const ClipId& clipId);

// True iff a track's clips are ordered by timelineStart and do not overlap
// outside the incoming (later) clip's explicit transition region. This mirrors
// the per-track portion of TimelineEngine's checkTimelineInvariants so a command
// can reject an offending edit itself with a clear, domain-specific message
// before the engine's generic invariant check would.
bool trackOrderedAndNonOverlapping(const Track& track);

// Clamp `value` into the closed interval [lo, hi]. Precondition: lo <= hi.
Duration clampDuration(Duration value, Duration lo, Duration hi);

// A short, stable label for a clip id in error messages.
std::string idLabel(const Uuid& id);

}  // namespace palmier::detail

#endif  // PALMIER_CORE_EDITCOMMANDSUPPORT_HPP
