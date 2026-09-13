// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shared fixtures for the edit-command unit tests, split out of
// edit_commands_test.cpp so no one of those files exceeds the repository's
// 1000-line ceiling. `inline` and in a named namespace: an anonymous namespace in a
// header would give every translation unit its own copy of each helper.
//
// Each partition brings these into scope with a using-directive, so the test bodies
// that were moved are byte-identical to what they were in the single file.

#ifndef PALMIER_TESTS_CORE_EDIT_COMMANDS_TEST_SUPPORT_HPP
#define PALMIER_TESTS_CORE_EDIT_COMMANDS_TEST_SUPPORT_HPP

#include "core/EditCommands.hpp"
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include "core/Clip.hpp"
#include "core/ColorSpace.hpp"
#include "core/FrameRate.hpp"
#include "core/Project.hpp"
#include "core/ProjectValidation.hpp"
#include "core/Resolution.hpp"
#include "core/TextStyle.hpp"
#include "core/TimelineEngine.hpp"
#include "core/ToneCurve.hpp"
#include "core/Track.hpp"
#include "core/Transition.hpp"
#include "core/Uuid.hpp"

namespace palmier::testsupport::editcommands {

// --- Fixtures --------------------------------------------------------------

inline Clip makeClip(ClipId id, Duration timelineStart, Duration sourceIn, Duration sourceOut) {
    Clip clip;
    clip.id = id;
    clip.assetRef = MediaAssetRef(Uuid::generateV4(), "mem://asset");
    clip.timelineStart = timelineStart;
    clip.sourceIn = sourceIn;
    clip.sourceOut = sourceOut;
    return clip;
}

// A project with a single empty video track, returning the track id.
inline Project makeProjectWithOneTrack(Uuid& trackIdOut) {
    Project project;
    project.id = Uuid::generateV4();
    project.name = "test";
    Track track;
    track.id = Uuid::generateV4();
    track.kind = TrackKind::Video;
    trackIdOut = track.id;
    project.tracks.push_back(std::move(track));
    return project;
}

// A project with a single empty TEXT track, returning the track id (usable-editor
// task 12; Requirement 9).
inline Project makeProjectWithOneTextTrack(Uuid& trackIdOut) {
    Project project;
    project.id = Uuid::generateV4();
    project.name = "test";
    Track track;
    track.id = Uuid::generateV4();
    track.kind = TrackKind::Text;
    trackIdOut = track.id;
    project.tracks.push_back(std::move(track));
    return project;
}

// A text clip: no assetRef (default-constructed, nil/"invalid" — exactly what
// AddClipCommand's own asset-registration step is written to skip), timing like
// any other clip, and a populated TextStyle.
inline Clip makeTextClip(ClipId id, Duration timelineStart, Duration duration,
                         std::string content) {
    Clip clip;
    clip.id = id;
    clip.timelineStart = timelineStart;
    clip.sourceIn = Duration::zero();
    clip.sourceOut = duration;
    TextStyle style;
    style.content = std::move(content);
    clip.textStyle = std::move(style);
    return clip;
}

// A project with a single, otherwise-empty CAPTION track (usable-editor task
// 13; Requirement 10), mirroring makeProjectWithOneTextTrack exactly.
inline Project makeProjectWithOneCaptionTrack(Uuid& trackIdOut) {
    Project project;
    project.id = Uuid::generateV4();
    project.name = "test";
    Track track;
    track.id = Uuid::generateV4();
    track.kind = TrackKind::Caption;
    trackIdOut = track.id;
    project.tracks.push_back(std::move(track));
    return project;
}

// A caption cue: no assetRef, timing like any other clip, and a plain
// captionText — no styling, mirroring makeTextClip exactly minus the style.
inline Clip makeCaptionCue(ClipId id, Duration timelineStart, Duration duration, std::string text) {
    Clip clip;
    clip.id = id;
    clip.timelineStart = timelineStart;
    clip.sourceIn = Duration::zero();
    clip.sourceOut = duration;
    clip.captionText = std::move(text);
    return clip;
}

constexpr Duration ms(std::int64_t v) { return Duration::fromMilliseconds(v); }

// --- AddClipCommand --------------------------------------------------------

}  // namespace palmier::testsupport::editcommands

#endif  // PALMIER_TESTS_CORE_EDIT_COMMANDS_TEST_SUPPORT_HPP
