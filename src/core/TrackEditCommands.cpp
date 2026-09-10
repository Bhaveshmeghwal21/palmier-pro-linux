// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/TrackEditCommands.cpp — the track edits: add, remove and mute.
//
// Split out of core/EditCommands.cpp, which held all 24 command implementations in
// one 1583-line translation unit. Every definition below was moved rather than
// rewritten. The commands are declared in the public core/EditCommands.hpp, which
// is unchanged; the helpers they share are in core/EditCommandSupport.hpp.

#include "core/EditCommands.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "core/EditCommandSupport.hpp"
#include "core/Error.hpp"
#include "core/Project.hpp"
#include "core/Track.hpp"

namespace palmier {

using namespace palmier::detail;  // NOLINT(google-build-using-namespace)

namespace {

// The tool-surface spelling of a track kind, used in error messages so a
// rejected `kind` argument is named the way the caller supplied it.
std::string_view trackKindLabel(TrackKind kind) {
    switch (kind) {
        case TrackKind::Audio:
            return "audio";
        case TrackKind::Text:
            return "text";
        case TrackKind::Caption:
            return "caption";
        case TrackKind::Video:
            return "video";
    }
    return "video";
}

}  // namespace

// ===========================================================================
// AddTrackCommand
// ===========================================================================

AddTrackCommand::AddTrackCommand(TrackKind kind) : kind_(kind), trackId_(Uuid::generateV4()) {}

AddTrackCommand::AddTrackCommand(TrackKind kind, Uuid trackId) : kind_(kind), trackId_(trackId) {}

Result<void> AddTrackCommand::apply(Project& project) {
    if (trackId_.isNil()) {
        return err(invalidArgument("AddTrackCommand: track id must not be nil"));
    }
    if (findTrack(project, trackId_) != nullptr) {
        // Requirement 3.3: the returned identifier is unique within the project.
        return err(failedPrecondition("AddTrackCommand: track " + idLabel(trackId_) +
                                      " is already present in the project"));
    }

    // Count the tracks of this kind and remember the position just past the last
    // one so the new lane is appended at that kind's tail (Requirement 3.3).
    std::size_t ofKind = 0;
    std::size_t insertAt = project.tracks.size();
    for (std::size_t i = 0; i < project.tracks.size(); ++i) {
        if (project.tracks[i].kind == kind_) {
            ++ofKind;
            insertAt = i + 1;
        }
    }

    // Requirement 3.8: a request that would exceed the per-kind cap is rejected
    // and names the offending argument.
    if (ofKind >= kMaxTracksPerKind) {
        return err(outOfRange(
            std::string("AddTrackCommand: 'kind' ") + std::string(trackKindLabel(kind_)) +
            " already holds " + std::to_string(ofKind) + " tracks; at most " +
            std::to_string(kMaxTracksPerKind) + " tracks of one kind are allowed"));
    }

    Track track;
    track.id = trackId_;
    track.kind = kind_;
    project.tracks.insert(project.tracks.begin() + static_cast<std::ptrdiff_t>(insertAt),
                          std::move(track));
    index_ = insertAt;
    return ok();
}

Result<void> AddTrackCommand::revert(Project& project) {
    if (!index_) {
        return err(failedPrecondition("AddTrackCommand: revert before a successful apply"));
    }
    auto it = std::find_if(project.tracks.begin(), project.tracks.end(),
                           [&](const Track& t) { return t.id == trackId_; });
    if (it == project.tracks.end()) {
        return err(notFound("AddTrackCommand: track " + idLabel(trackId_) + " not found"));
    }
    project.tracks.erase(it);
    return ok();
}

// ===========================================================================
// RemoveTrackCommand
// ===========================================================================

RemoveTrackCommand::RemoveTrackCommand(Uuid trackId) : trackId_(trackId) {}

std::optional<std::size_t> RemoveTrackCommand::removedClipCount() const noexcept {
    if (!removed_) {
        return std::nullopt;
    }
    return removed_->clips.size();
}

Result<void> RemoveTrackCommand::apply(Project& project) {
    auto it = std::find_if(project.tracks.begin(), project.tracks.end(),
                           [&](const Track& t) { return t.id == trackId_; });
    if (it == project.tracks.end()) {
        // Requirement 3.8: an unknown track identifier changes nothing.
        return err(notFound("RemoveTrackCommand: track " + idLabel(trackId_) +
                            " not found in the current project"));
    }

    index_ = static_cast<std::size_t>(std::distance(project.tracks.begin(), it));
    removed_ = *it;            // the track and every clip on it
    project.tracks.erase(it);  // erase preserves the order of the rest
    return ok();
}

Result<void> RemoveTrackCommand::revert(Project& project) {
    if (!removed_) {
        return err(failedPrecondition("RemoveTrackCommand: revert before a successful apply"));
    }
    const std::size_t index = std::min(index_, project.tracks.size());
    project.tracks.insert(project.tracks.begin() + static_cast<std::ptrdiff_t>(index), *removed_);
    return ok();
}

// ===========================================================================
// SetTrackMutedCommand
// ===========================================================================

SetTrackMutedCommand::SetTrackMutedCommand(Uuid trackId, bool muted)
    : trackId_(trackId), muted_(muted) {}

Result<void> SetTrackMutedCommand::apply(Project& project) {
    auto it = std::find_if(project.tracks.begin(), project.tracks.end(),
                           [&](const Track& t) { return t.id == trackId_; });
    if (it == project.tracks.end()) {
        // Same rule as every other track-addressed edit (Requirement 3.8): an
        // unknown identifier changes nothing.
        return err(notFound("SetTrackMutedCommand: track " + idLabel(trackId_) +
                            " not found in the current project"));
    }
    prior_ = it->muted;
    it->muted = muted_;
    return ok();
}

Result<void> SetTrackMutedCommand::revert(Project& project) {
    if (!prior_) {
        return err(failedPrecondition("SetTrackMutedCommand: revert before a successful apply"));
    }
    auto it = std::find_if(project.tracks.begin(), project.tracks.end(),
                           [&](const Track& t) { return t.id == trackId_; });
    if (it == project.tracks.end()) {
        return err(notFound("SetTrackMutedCommand: track " + idLabel(trackId_) +
                            " not found in the current project"));
    }
    it->muted = *prior_;
    return ok();
}

}  // namespace palmier
