// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/MarkerEditCommands.cpp — adding and removing timeline markers.
//
// Mirrors AddTrackCommand/RemoveTrackCommand in core/TrackEditCommands.cpp exactly: an
// append that can only fail on a duplicate identifier, and a removal that captures the
// removed value and its index so revert() is an exact inverse.

#include "core/MarkerEditCommands.hpp"

#include <algorithm>
#include <utility>

#include "core/EditCommandSupport.hpp"
#include "core/Error.hpp"
#include "core/Project.hpp"

namespace palmier {

using namespace palmier::detail;  // NOLINT(google-build-using-namespace)

// ===========================================================================
// AddMarkerCommand
// ===========================================================================

AddMarkerCommand::AddMarkerCommand(Duration timelineNs, std::string label)
    : markerId_(Uuid::generateV4()), timelineNs_(timelineNs), label_(std::move(label)) {}

AddMarkerCommand::AddMarkerCommand(Uuid markerId, Duration timelineNs, std::string label)
    : markerId_(markerId), timelineNs_(timelineNs), label_(std::move(label)) {}

Result<void> AddMarkerCommand::apply(Project& project) {
    if (markerId_.isNil()) {
        return err(invalidArgument("AddMarkerCommand: marker id must not be nil"));
    }
    const bool exists =
        std::any_of(project.markers.begin(), project.markers.end(),
                   [&](const Marker& m) { return m.id == markerId_; });
    if (exists) {
        return err(failedPrecondition("AddMarkerCommand: marker " + idLabel(markerId_) +
                                      " is already present in the project"));
    }

    Marker marker;
    marker.id = markerId_;
    marker.timelineNs = timelineNs_;
    marker.label = label_;
    project.markers.push_back(std::move(marker));
    applied_ = true;
    return ok();
}

Result<void> AddMarkerCommand::revert(Project& project) {
    if (!applied_) {
        return err(failedPrecondition("AddMarkerCommand: revert before a successful apply"));
    }
    auto it = std::find_if(project.markers.begin(), project.markers.end(),
                           [&](const Marker& m) { return m.id == markerId_; });
    if (it == project.markers.end()) {
        return err(notFound("AddMarkerCommand: marker " + idLabel(markerId_) + " not found"));
    }
    project.markers.erase(it);
    return ok();
}

// ===========================================================================
// RemoveMarkerCommand
// ===========================================================================

RemoveMarkerCommand::RemoveMarkerCommand(Uuid markerId) : markerId_(markerId) {}

Result<void> RemoveMarkerCommand::apply(Project& project) {
    auto it = std::find_if(project.markers.begin(), project.markers.end(),
                           [&](const Marker& m) { return m.id == markerId_; });
    if (it == project.markers.end()) {
        return err(notFound("RemoveMarkerCommand: marker " + idLabel(markerId_) +
                            " not found in the current project"));
    }

    index_ = static_cast<std::size_t>(std::distance(project.markers.begin(), it));
    removed_ = *it;
    project.markers.erase(it);
    return ok();
}

Result<void> RemoveMarkerCommand::revert(Project& project) {
    if (!removed_) {
        return err(failedPrecondition("RemoveMarkerCommand: revert before a successful apply"));
    }
    const std::size_t index = std::min(index_, project.markers.size());
    project.markers.insert(project.markers.begin() + static_cast<std::ptrdiff_t>(index),
                           *removed_);
    return ok();
}

}  // namespace palmier
