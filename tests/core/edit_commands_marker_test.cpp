// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for AddMarkerCommand and RemoveMarkerCommand (upstream parity: the
// `timeline` tool category's marker gap), mirroring edit_commands_track_test.cpp's own
// AddTrackCommand/RemoveTrackCommand tests exactly.

#include "core/EditCommands.hpp"

#include <gtest/gtest.h>

#include "edit_commands_test_support.hpp"

namespace palmier {
namespace {

using namespace palmier::testsupport::editcommands;  // NOLINT(google-build-using-namespace)

Project makeEmptyProject() {
    Project project;
    project.id = Uuid::generateV4();
    project.name = "markers";
    return project;
}

// --- AddMarkerCommand -------------------------------------------------------

TEST(AddMarkerCommand, AppendsAMarkerWithTheGivenPositionAndLabel) {
    Project project = makeEmptyProject();

    AddMarkerCommand cmd(ms(1500), "Interview starts");
    ASSERT_TRUE(cmd.apply(project).isOk());

    ASSERT_EQ(project.markers.size(), 1u);
    EXPECT_EQ(project.markers[0].id, cmd.markerId());
    EXPECT_EQ(project.markers[0].timelineNs, ms(1500));
    EXPECT_EQ(project.markers[0].label, "Interview starts");
}

TEST(AddMarkerCommand, GeneratesAFreshIdentifierWhenNoneIsSupplied) {
    Project project = makeEmptyProject();
    AddMarkerCommand a(ms(0), "a");
    AddMarkerCommand b(ms(0), "b");
    EXPECT_NE(a.markerId(), b.markerId());
}

TEST(AddMarkerCommand, AcceptsAnExplicitIdentifier) {
    Project project = makeEmptyProject();
    const Uuid explicitId = Uuid::generateV4();

    AddMarkerCommand cmd(explicitId, ms(2000), "");
    ASSERT_TRUE(cmd.apply(project).isOk());

    EXPECT_EQ(cmd.markerId(), explicitId);
    EXPECT_EQ(project.markers[0].id, explicitId);
}

TEST(AddMarkerCommand, RejectsADuplicateIdentifierWithoutMutatingTheProject) {
    Project project = makeEmptyProject();
    const Uuid markerId = Uuid::generateV4();
    AddMarkerCommand first(markerId, ms(0), "first");
    ASSERT_TRUE(first.apply(project).isOk());

    AddMarkerCommand second(markerId, ms(5000), "second");
    const Result<void> result = second.apply(project);

    EXPECT_TRUE(result.isError());
    ASSERT_EQ(project.markers.size(), 1u);
    EXPECT_EQ(project.markers[0].label, "first");
}

TEST(AddMarkerCommand, LeavesExistingTracksAndClipsUntouched) {
    Uuid trackId;
    Project project = makeProjectWithOneTrack(trackId);
    const ClipId a = Uuid::generateV4();
    project.tracks[0].clips = {makeClip(a, ms(0), ms(0), ms(1000))};

    AddMarkerCommand cmd(ms(500), "note");
    ASSERT_TRUE(cmd.apply(project).isOk());

    ASSERT_EQ(project.tracks.size(), 1u);
    ASSERT_EQ(project.tracks[0].clips.size(), 1u);
    EXPECT_EQ(project.tracks[0].clips[0].id, a);
}

TEST(AddMarkerCommand, RevertRemovesExactlyTheAddedMarker) {
    Project project = makeEmptyProject();
    AddMarkerCommand existing(ms(0), "keep");
    ASSERT_TRUE(existing.apply(project).isOk());

    AddMarkerCommand added(ms(1000), "undo me");
    ASSERT_TRUE(added.apply(project).isOk());
    ASSERT_EQ(project.markers.size(), 2u);

    ASSERT_TRUE(added.revert(project).isOk());

    ASSERT_EQ(project.markers.size(), 1u);
    EXPECT_EQ(project.markers[0].label, "keep");
}

TEST(AddMarkerCommand, RevertBeforeApplyIsRejected) {
    Project project = makeEmptyProject();
    AddMarkerCommand cmd(ms(0), "never applied");
    EXPECT_TRUE(cmd.revert(project).isError());
}

TEST(AddMarkerCommand, ApplyRevertApplyReproducesTheSameMarker) {
    Project project = makeEmptyProject();
    AddMarkerCommand cmd(ms(3000), "redo me");

    ASSERT_TRUE(cmd.apply(project).isOk());
    ASSERT_TRUE(cmd.revert(project).isOk());
    ASSERT_TRUE(cmd.apply(project).isOk());  // redo

    ASSERT_EQ(project.markers.size(), 1u);
    EXPECT_EQ(project.markers[0].id, cmd.markerId());
    EXPECT_EQ(project.markers[0].timelineNs, ms(3000));
    EXPECT_EQ(project.markers[0].label, "redo me");
}

// --- RemoveMarkerCommand -----------------------------------------------------

TEST(RemoveMarkerCommand, RemovesTheNamedMarkerAndLeavesOthersInOrder) {
    Project project = makeEmptyProject();
    AddMarkerCommand a(ms(0), "a");
    AddMarkerCommand b(ms(1000), "b");
    AddMarkerCommand c(ms(2000), "c");
    ASSERT_TRUE(a.apply(project).isOk());
    ASSERT_TRUE(b.apply(project).isOk());
    ASSERT_TRUE(c.apply(project).isOk());

    RemoveMarkerCommand remove(b.markerId());
    ASSERT_TRUE(remove.apply(project).isOk());

    ASSERT_EQ(project.markers.size(), 2u);
    EXPECT_EQ(project.markers[0].label, "a");
    EXPECT_EQ(project.markers[1].label, "c");
}

TEST(RemoveMarkerCommand, RejectsAnUnknownIdentifierWithoutMutatingTheProject) {
    Project project = makeEmptyProject();
    AddMarkerCommand a(ms(0), "a");
    ASSERT_TRUE(a.apply(project).isOk());

    RemoveMarkerCommand remove(Uuid::generateV4());
    const Result<void> result = remove.apply(project);

    EXPECT_TRUE(result.isError());
    ASSERT_EQ(project.markers.size(), 1u);
}

TEST(RemoveMarkerCommand, RevertReinsertsTheMarkerAtItsFormerIndex) {
    Project project = makeEmptyProject();
    AddMarkerCommand a(ms(0), "a");
    AddMarkerCommand b(ms(1000), "b");
    AddMarkerCommand c(ms(2000), "c");
    ASSERT_TRUE(a.apply(project).isOk());
    ASSERT_TRUE(b.apply(project).isOk());
    ASSERT_TRUE(c.apply(project).isOk());

    RemoveMarkerCommand remove(b.markerId());
    ASSERT_TRUE(remove.apply(project).isOk());
    ASSERT_TRUE(remove.revert(project).isOk());

    ASSERT_EQ(project.markers.size(), 3u);
    EXPECT_EQ(project.markers[0].label, "a");
    EXPECT_EQ(project.markers[1].label, "b");
    EXPECT_EQ(project.markers[1].id, b.markerId());
    EXPECT_EQ(project.markers[2].label, "c");
}

TEST(RemoveMarkerCommand, RevertBeforeApplyIsRejected) {
    Project project = makeEmptyProject();
    RemoveMarkerCommand cmd(Uuid::generateV4());
    EXPECT_TRUE(cmd.revert(project).isError());
}

}  // namespace
}  // namespace palmier
