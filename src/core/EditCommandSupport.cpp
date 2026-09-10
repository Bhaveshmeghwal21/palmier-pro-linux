// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/EditCommandSupport.cpp — definitions of the helpers the edit commands share.
//
// Split out of core/EditCommands.cpp. Declared in EditCommandSupport.hpp; see there
// for why these needed promoting out of an anonymous namespace while the command
// members did not.

#include "core/EditCommandSupport.hpp"

#include <algorithm>
#include <utility>

namespace palmier::detail {

Track* findTrack(Project& project, const Uuid& trackId) {
    for (Track& track : project.tracks) {
        if (track.id == trackId) {
            return &track;
        }
    }
    return nullptr;
}

std::optional<ClipLocation> findClip(Project& project, const ClipId& clipId) {
    for (Track& track : project.tracks) {
        for (std::size_t i = 0; i < track.clips.size(); ++i) {
            if (track.clips[i].id == clipId) {
                return ClipLocation{&track, i};
            }
        }
    }
    return std::nullopt;
}

bool trackOrderedAndNonOverlapping(const Track& track) {
    const std::vector<Clip>& clips = track.clips;
    for (std::size_t i = 1; i < clips.size(); ++i) {
        const Clip& previous = clips[i - 1];
        const Clip& current = clips[i];

        if (current.timelineStart < previous.timelineStart) {
            return false;  // not sorted by timelineStart
        }

        const Duration allowedOverlap =
            current.transitionIn.has_value() ? current.transitionIn->duration : Duration::zero();
        const Duration overlap = previous.timelineEnd() - current.timelineStart;
        if (overlap > allowedOverlap) {
            return false;  // overlaps outside a transition region
        }
    }
    return true;
}

Duration clampDuration(Duration value, Duration lo, Duration hi) {
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

std::string idLabel(const Uuid& id) { return id.isNil() ? std::string{"<nil>"} : id.toString(); }

}  // namespace palmier::detail
