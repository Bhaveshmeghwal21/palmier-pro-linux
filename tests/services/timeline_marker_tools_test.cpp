// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/services/timeline_marker_tools_test.cpp — unit tests for `timeline.add_marker`
// and `timeline.remove_marker` (upstream parity: the `timeline` tool category's marker
// gap), mirroring session_media_track_tools_test.cpp's own track-tool tests exactly.
// Added to the same target rather than growing that file, which sits at the 1000-line
// ceiling.

#include "services/ToolRegistry.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "core/EditCommands.hpp"
#include "core/Project.hpp"
#include "core/Uuid.hpp"
#include "services/Json.hpp"
#include "services/ProjectSession.hpp"

namespace palmier::services {
namespace {

Json invokeOk(const ToolRegistry& registry, std::string_view tool, const Json& args) {
    Result<Json> result = registry.invoke(tool, args);
    EXPECT_TRUE(result.isOk()) << tool << ": " << result.error().toString();
    return result.isOk() ? result.value() : Json::object();
}

void invokeErr(const ToolRegistry& registry, std::string_view tool, const Json& args) {
    const Result<Json> result = registry.invoke(tool, args);
    EXPECT_TRUE(result.isError()) << tool << " unexpectedly succeeded";
}

Json markerArgs(std::int64_t timelineNs, const std::string& label) {
    Json args = Json::object();
    args.set("timelineNs", timelineNs);
    args.set("label", label);
    return args;
}

TEST(TimelineMarkerTools, AddMarkerReportsTheGeneratedIdAndPersistsTheMarker) {
    ProjectSession     session;
    const ToolRegistry registry = buildDefaultToolRegistry(session);

    const Json result = invokeOk(registry, "timeline.add_marker", markerArgs(1500, "Sync point"));
    ASSERT_NE(result.find("markerId"), nullptr);
    EXPECT_FALSE(result.stringOr("markerId").empty());

    const Project project = session.engine().snapshot();
    ASSERT_EQ(project.markers.size(), 1u);
    EXPECT_EQ(project.markers[0].id.toString(), result.stringOr("markerId"));
    EXPECT_EQ(project.markers[0].timelineNs.nanoseconds(), 1500);
    EXPECT_EQ(project.markers[0].label, "Sync point");
}

TEST(TimelineMarkerTools, AddMarkerDefaultsTheLabelToEmpty) {
    ProjectSession     session;
    const ToolRegistry registry = buildDefaultToolRegistry(session);

    Json args = Json::object();
    args.set("timelineNs", static_cast<std::int64_t>(0));
    invokeOk(registry, "timeline.add_marker", args);

    ASSERT_EQ(session.engine().snapshot().markers.size(), 1u);
    EXPECT_EQ(session.engine().snapshot().markers[0].label, "");
}

TEST(TimelineMarkerTools, AddMarkerAcceptsAnExplicitMarkerId) {
    ProjectSession     session;
    const ToolRegistry registry = buildDefaultToolRegistry(session);
    const Uuid         explicitId = Uuid::generateV4();

    Json args = markerArgs(0, "explicit");
    args.set("markerId", explicitId.toString());
    const Json result = invokeOk(registry, "timeline.add_marker", args);

    EXPECT_EQ(result.stringOr("markerId"), explicitId.toString());
    EXPECT_EQ(session.engine().snapshot().markers[0].id, explicitId);
}

TEST(TimelineMarkerTools, AddMarkerRejectsANegativePosition) {
    ProjectSession     session;
    const ToolRegistry registry = buildDefaultToolRegistry(session);

    invokeErr(registry, "timeline.add_marker", markerArgs(-1, "before zero"));
    EXPECT_TRUE(session.engine().snapshot().markers.empty());
}

TEST(TimelineMarkerTools, RemoveMarkerDropsExactlyTheNamedMarker) {
    ProjectSession     session;
    const ToolRegistry registry = buildDefaultToolRegistry(session);

    const Json a = invokeOk(registry, "timeline.add_marker", markerArgs(0, "a"));
    const Json b = invokeOk(registry, "timeline.add_marker", markerArgs(1000, "b"));

    Json removeArgs = Json::object();
    removeArgs.set("markerId", b.stringOr("markerId"));
    const Json removed = invokeOk(registry, "timeline.remove_marker", removeArgs);
    EXPECT_EQ(removed.stringOr("markerId"), b.stringOr("markerId"));

    const Project project = session.engine().snapshot();
    ASSERT_EQ(project.markers.size(), 1u);
    EXPECT_EQ(project.markers[0].id.toString(), a.stringOr("markerId"));
}

TEST(TimelineMarkerTools, RemoveMarkerRejectsAnUnknownIdAndChangesNothing) {
    ProjectSession     session;
    const ToolRegistry registry = buildDefaultToolRegistry(session);
    invokeOk(registry, "timeline.add_marker", markerArgs(0, "kept"));

    Json args = Json::object();
    args.set("markerId", Uuid::generateV4().toString());
    invokeErr(registry, "timeline.remove_marker", args);

    ASSERT_EQ(session.engine().snapshot().markers.size(), 1u);
    EXPECT_EQ(session.engine().snapshot().markers[0].label, "kept");
}

TEST(TimelineMarkerTools, MarkerToolsAreUndoableThroughTheOrdinaryEditPath) {
    ProjectSession     session;
    const ToolRegistry registry = buildDefaultToolRegistry(session);

    invokeOk(registry, "timeline.add_marker", markerArgs(0, "undo me"));
    EXPECT_EQ(session.engine().undoDepth(), 1u);

    ASSERT_TRUE(session.engine().undo().isOk());
    EXPECT_TRUE(session.engine().snapshot().markers.empty());

    ASSERT_TRUE(session.engine().redo().isOk());
    ASSERT_EQ(session.engine().snapshot().markers.size(), 1u);
    EXPECT_EQ(session.engine().snapshot().markers[0].label, "undo me");
}

TEST(TimelineMarkerTools, TimelineReadReportsEveryMarker) {
    ProjectSession     session;
    const ToolRegistry registry = buildDefaultToolRegistry(session);

    invokeOk(registry, "timeline.add_marker", markerArgs(1000, "one"));
    invokeOk(registry, "timeline.add_marker", markerArgs(2000, "two"));

    const Json read = invokeOk(registry, "timeline.read", Json::object());
    const Json* markers = read.find("markers");
    ASSERT_NE(markers, nullptr);
    ASSERT_TRUE(markers->isArray());
    ASSERT_EQ(markers->asArray().size(), 2u);
    EXPECT_EQ(markers->asArray()[0].intOr("timelineNs"), 1000);
    EXPECT_EQ(markers->asArray()[0].stringOr("label"), "one");
    EXPECT_EQ(markers->asArray()[1].intOr("timelineNs"), 2000);
    EXPECT_EQ(markers->asArray()[1].stringOr("label"), "two");
}

}  // namespace
}  // namespace palmier::services
