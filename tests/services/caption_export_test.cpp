// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/services/caption_export_test.cpp — services::CaptionExport (usable-editor
// task 13; Requirement 10.3's sidecar export mode).

#include "services/CaptionExport.hpp"

#include <gtest/gtest.h>

#include "core/Project.hpp"
#include "core/Track.hpp"

namespace palmier::services {
namespace {

constexpr Duration ms(std::int64_t v) { return Duration::fromMilliseconds(v); }

Clip makeCaptionCue(Duration timelineStart, Duration duration, std::string text) {
    Clip clip;
    clip.id = Uuid::generateV4();
    clip.timelineStart = timelineStart;
    clip.sourceIn = Duration::zero();
    clip.sourceOut = duration;
    clip.captionText = std::move(text);
    return clip;
}

Project makeProjectWithOneCaptionTrack() {
    Project project;
    project.id = Uuid::generateV4();
    project.name = "test";
    Track track;
    track.id = Uuid::generateV4();
    track.kind = TrackKind::Caption;
    project.tracks.push_back(std::move(track));
    return project;
}

TEST(ProjectHasCaptions, FalseForAProjectWithNoCaptionTrackAtAll) {
    Project project;
    project.id = Uuid::generateV4();
    Track videoTrack;
    videoTrack.id = Uuid::generateV4();
    videoTrack.kind = TrackKind::Video;
    project.tracks.push_back(std::move(videoTrack));

    EXPECT_FALSE(projectHasCaptions(project));
}

TEST(ProjectHasCaptions, FalseForAnEmptyCaptionTrack) {
    Project project = makeProjectWithOneCaptionTrack();
    EXPECT_FALSE(projectHasCaptions(project));
}

TEST(ProjectHasCaptions, FalseForAMutedCaptionTrackEvenWithCues) {
    Project project = makeProjectWithOneCaptionTrack();
    project.tracks[0].muted = true;
    project.tracks[0].clips.push_back(makeCaptionCue(ms(0), ms(1000), "Hello"));
    EXPECT_FALSE(projectHasCaptions(project));
}

TEST(ProjectHasCaptions, TrueForANonMutedCaptionTrackWithAtLeastOneCue) {
    Project project = makeProjectWithOneCaptionTrack();
    project.tracks[0].clips.push_back(makeCaptionCue(ms(0), ms(1000), "Hello"));
    EXPECT_TRUE(projectHasCaptions(project));
}

TEST(RenderSrt, EmptyProjectRendersAnEmptyDocument) {
    Project project = makeProjectWithOneCaptionTrack();
    EXPECT_EQ(renderSrt(project), "");
}

TEST(RenderSrt, OneCueRendersTheStandardFourLineBlock) {
    Project project = makeProjectWithOneCaptionTrack();
    project.tracks[0].clips.push_back(makeCaptionCue(ms(0), ms(1500), "Hello, world!"));

    const std::string srt = renderSrt(project);
    EXPECT_EQ(srt,
             "1\r\n"
             "00:00:00,000 --> 00:00:01,500\r\n"
             "Hello, world!\r\n"
             "\r\n");
}

TEST(RenderSrt, TimestampsCrossMinuteAndHourBoundariesCorrectly) {
    Project project = makeProjectWithOneCaptionTrack();
    // 1h 2m 3.456s = 3723456 ms.
    project.tracks[0].clips.push_back(
        makeCaptionCue(ms(3723456), ms(1000), "Late cue"));

    const std::string srt = renderSrt(project);
    EXPECT_NE(srt.find("01:02:03,456 --> 01:02:04,456"), std::string::npos);
}

TEST(RenderSrt, MultipleCuesAreOrderedByTimelineStartAndNumberedFromOne) {
    Project project = makeProjectWithOneCaptionTrack();
    // Pushed out of order; renderSrt must sort by timelineStart.
    project.tracks[0].clips.push_back(makeCaptionCue(ms(5000), ms(1000), "Second"));
    project.tracks[0].clips.push_back(makeCaptionCue(ms(0), ms(1000), "First"));

    const std::string srt = renderSrt(project);
    const std::size_t firstPos = srt.find("First");
    const std::size_t secondPos = srt.find("Second");
    ASSERT_NE(firstPos, std::string::npos);
    ASSERT_NE(secondPos, std::string::npos);
    EXPECT_LT(firstPos, secondPos);

    // "First" is cue 1, "Second" is cue 2.
    EXPECT_NE(srt.find("1\r\n00:00:00,000"), std::string::npos);
    EXPECT_NE(srt.find("2\r\n00:00:05,000"), std::string::npos);
}

TEST(RenderSrt, SkipsCluesOnAMutedCaptionTrack) {
    Project project = makeProjectWithOneCaptionTrack();
    project.tracks[0].muted = true;
    project.tracks[0].clips.push_back(makeCaptionCue(ms(0), ms(1000), "Hidden"));

    EXPECT_EQ(renderSrt(project), "");
}

TEST(RenderSrt, SkipsNonCaptionTracksEvenIfPresent) {
    Project project = makeProjectWithOneCaptionTrack();
    project.tracks[0].clips.push_back(makeCaptionCue(ms(0), ms(1000), "Visible"));

    Track videoTrack;
    videoTrack.id = Uuid::generateV4();
    videoTrack.kind = TrackKind::Video;
    Clip videoClip;
    videoClip.id = Uuid::generateV4();
    videoClip.timelineStart = ms(0);
    videoClip.sourceIn = Duration::zero();
    videoClip.sourceOut = ms(1000);
    videoTrack.clips.push_back(videoClip);
    project.tracks.push_back(std::move(videoTrack));

    const std::string srt = renderSrt(project);
    EXPECT_NE(srt.find("Visible"), std::string::npos);
    // Exactly one cue was rendered (the video clip contributed nothing).
    EXPECT_EQ(srt.find("2\r\n"), std::string::npos);
}

// Requirement 10.3: burn-in (gpu::Compositor::gatherVisibleCaptionCues, via
// Clip::timelineStart/timelineEnd()) and the sidecar (this file, via the
// IDENTICAL fields) must agree on timing. This test proves the sidecar's own
// timestamps are exactly timelineStart/timelineEnd() with no other adjustment,
// which is what makes that agreement structural rather than coincidental.
TEST(RenderSrt, TimestampsAreExactlyTimelineStartAndTimelineEndWithNoAdjustment) {
    Project project = makeProjectWithOneCaptionTrack();
    const Clip cue = makeCaptionCue(ms(1234), ms(2000), "Exact");
    ASSERT_EQ(cue.timelineStart, ms(1234));
    ASSERT_EQ(cue.timelineEnd(), ms(3234));
    project.tracks[0].clips.push_back(cue);

    const std::string srt = renderSrt(project);
    EXPECT_NE(srt.find("00:00:01,234 --> 00:00:03,234"), std::string::npos);
}

}  // namespace
}  // namespace palmier::services
