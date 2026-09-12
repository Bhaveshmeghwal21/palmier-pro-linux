// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/core/clip_speed_test.cpp — a clip's playback rate.
//
// Speed rescales how much timeline a clip occupies for a given span of source, and the whole feature
// is one identity and its inverse: a clip at speed 2 consumes two seconds of source per second of
// timeline. What makes it tractable is that every reader of a clip's length goes through
// Clip::duration(), so the overlap rules, the ripple operations, the compositor's clip lookup and
// the UI all get the rescaled length without knowing speed exists.
//
// Only three places convert a timeline instant into a source instant and therefore need speed by
// name: clipSourcePosition, the audio mix's source window, and SplitClipCommand's offset. The split
// is exercised here because it is the one whose previous comment stated outright that the two
// offsets coincide "because the model has no speed change".

#include <gtest/gtest.h>

#include "core/Clip.hpp"
#include "core/ClipGeometry.hpp"
#include "core/Duration.hpp"
#include "core/EditCommands.hpp"
#include "core/Error.hpp"
#include "core/Project.hpp"
#include "core/ProjectValidation.hpp"
#include "core/Result.hpp"
#include "core/Uuid.hpp"
#include "media/DecoderClipFrameProvider.hpp"

namespace palmier {
namespace {

Clip makeClip(double startSeconds, double sourceInSeconds, double sourceOutSeconds, double speed) {
    Clip c;
    c.id = Uuid::generateV4();
    c.assetRef = MediaAssetRef(Uuid::generateV4(), "mem://asset");
    c.timelineStart = Duration::fromSeconds(startSeconds);
    c.sourceIn = Duration::fromSeconds(sourceInSeconds);
    c.sourceOut = Duration::fromSeconds(sourceOutSeconds);
    c.speed = speed;
    return c;
}

/// A one-track project holding `clip`, with its asset registered so validation has no unrelated
/// reason to refuse the project.
Project projectWith(Clip clip) {
    Track track;
    track.id = Uuid::generateV4();
    track.kind = TrackKind::Video;
    track.clips.push_back(std::move(clip));

    Project project;
    project.id = Uuid::generateV4();
    project.name = "speed";
    project.timelineFps = FrameRate::fps30();
    project.canvas = Resolution::hd1080();
    project.assets.push_back(track.clips.front().assetRef);
    project.tracks.push_back(std::move(track));
    return project;
}

double seconds(Duration d) { return static_cast<double>(d.ticks()) / 1e9; }

// --- the arithmetic ---------------------------------------------------------

TEST(ScaleDuration, ScalesAndRoundsToTheNearestTick) {
    EXPECT_NEAR(seconds(scaleDuration(Duration::fromSeconds(4.0), 0.5)), 2.0, 1e-9);
    EXPECT_NEAR(seconds(scaleDuration(Duration::fromSeconds(4.0), 2.0)), 8.0, 1e-9);
}

TEST(ScaleDuration, AnUnusableFactorYieldsZeroRatherThanANegativeDuration) {
    EXPECT_EQ(scaleDuration(Duration::fromSeconds(4.0), 0.0).ticks(), 0);
    EXPECT_EQ(scaleDuration(Duration::fromSeconds(4.0), -1.0).ticks(), 0);
}

TEST(IsUsableSpeed, RejectsZeroAndNegatives) {
    EXPECT_TRUE(isUsableSpeed(1.0));
    EXPECT_TRUE(isUsableSpeed(0.01));
    EXPECT_FALSE(isUsableSpeed(0.0));
    EXPECT_FALSE(isUsableSpeed(-2.0));
}

TEST(SpeedMappings, TimelineAndSourceDeltasAreInverses) {
    for (const double speed : {0.25, 0.5, 1.0, 2.0, 4.0}) {
        const Duration timeline = Duration::fromSeconds(3.0);
        const Duration source = sourceDeltaForTimeline(timeline, speed);
        EXPECT_NEAR(seconds(timelineLengthForSource(source, speed)), 3.0, 1e-6)
            << "at speed " << speed;
    }
}

// --- the clip's length -----------------------------------------------------

TEST(ClipSpeed, DurationIsTheSourceSpanRescaled) {
    EXPECT_NEAR(seconds(makeClip(0.0, 6.0, 10.0, 1.0).duration()), 4.0, 1e-9);
    EXPECT_NEAR(seconds(makeClip(0.0, 6.0, 10.0, 2.0).duration()), 2.0, 1e-9);
    EXPECT_NEAR(seconds(makeClip(0.0, 6.0, 10.0, 0.5).duration()), 8.0, 1e-9);
}

TEST(ClipSpeed, TimelineEndFollowsTheRescaledDuration) {
    // This is what lets every overlap rule, ripple operation and clip lookup stay unaware of speed.
    EXPECT_NEAR(seconds(makeClip(5.0, 6.0, 10.0, 2.0).timelineEnd()), 7.0, 1e-9);
    EXPECT_NEAR(seconds(makeClip(5.0, 6.0, 10.0, 0.5).timelineEnd()), 13.0, 1e-9);
}

// --- the timeline-to-source mapping ----------------------------------------

TEST(ClipSourcePosition, ConsumesSpeedTimesSourcePerUnitTimeline) {
    const Clip fast = makeClip(10.0, 6.0, 10.0, 2.0);  // occupies timeline 10.0-12.0
    EXPECT_NEAR(seconds(media::clipSourcePosition(fast, Duration::fromSeconds(10.0))), 6.0, 1e-9);
    EXPECT_NEAR(seconds(media::clipSourcePosition(fast, Duration::fromSeconds(11.0))), 8.0, 1e-9);

    const Clip slow = makeClip(10.0, 6.0, 10.0, 0.5);  // occupies timeline 10.0-18.0
    EXPECT_NEAR(seconds(media::clipSourcePosition(slow, Duration::fromSeconds(14.0))), 8.0, 1e-9);
}

TEST(ClipSourcePosition, AtUnitSpeedIsTheUnchangedMapping) {
    const Clip c = makeClip(4.0, 1.0, 5.0, 1.0);
    EXPECT_NEAR(seconds(media::clipSourcePosition(c, Duration::fromSeconds(6.5))), 3.5, 1e-9);
}

// --- the split -------------------------------------------------------------

TEST(SplitClipAtSpeed, CutsTheSourceAtTheInstantTheTimelineCutCorrespondsTo) {
    // Source 6..10 (4s) at speed 2 occupies timeline 0..2. A cut one timeline second in is two
    // source seconds in, so the source is cut at 8.0.
    Project project = projectWith(makeClip(0.0, 6.0, 10.0, 2.0));
    const ClipId id = project.tracks.front().clips.front().id;

    SplitClipCommand split(id, Duration::fromSeconds(1.0));
    ASSERT_TRUE(split.apply(project).isOk());

    const Track& track = project.tracks.front();
    ASSERT_EQ(track.clips.size(), 2u);
    EXPECT_NEAR(seconds(track.clips[0].sourceOut), 8.0, 1e-9);
    EXPECT_NEAR(seconds(track.clips[1].sourceIn), 8.0, 1e-9);
    EXPECT_NEAR(seconds(track.clips[1].timelineStart), 1.0, 1e-9);
    // Both halves are one timeline second long, and both keep the speed.
    EXPECT_NEAR(seconds(track.clips[0].duration()), 1.0, 1e-9);
    EXPECT_NEAR(seconds(track.clips[1].duration()), 1.0, 1e-9);
    EXPECT_DOUBLE_EQ(track.clips[0].speed, 2.0);
    EXPECT_DOUBLE_EQ(track.clips[1].speed, 2.0);
}

TEST(SplitClipAtSpeed, TheHalvesRemainContiguousOnTheTimeline) {
    Project project = projectWith(makeClip(0.0, 0.0, 6.0, 3.0));  // occupies 0..2
    const ClipId id = project.tracks.front().clips.front().id;

    SplitClipCommand split(id, Duration::fromSeconds(0.5));
    ASSERT_TRUE(split.apply(project).isOk());

    const Track& track = project.tracks.front();
    ASSERT_EQ(track.clips.size(), 2u);
    EXPECT_NEAR(seconds(track.clips[0].timelineEnd()), seconds(track.clips[1].timelineStart), 1e-9);
    // And the project is still valid, which is the invariant a wrong source offset would break.
    EXPECT_TRUE(validateProject(project).isOk());
}

// --- a split whose source offset would be degenerate ----------------------

TEST(SplitClipAtSpeed, APlayheadThatMapsToNoInteriorSourcePositionIsRefused) {
    // Below unit speed the clip occupies MORE timeline than the source it is drawn from, so several
    // timeline ticks map to one source tick. At speed 0.25 a playhead one nanosecond inside the clip
    // is a quarter of a source nanosecond in, which is no source position at all -- the left half
    // would carry nothing. The timeline-space check alone cannot see this.
    Clip clip;
    clip.id = Uuid::generateV4();
    clip.assetRef = MediaAssetRef(Uuid::generateV4(), "mem://asset");
    clip.timelineStart = Duration::zero();
    clip.sourceIn = Duration::zero();
    clip.sourceOut = Duration::fromNanoseconds(1'000);
    clip.speed = 0.25;  // so the clip occupies 4'000 ns of timeline

    Project project = projectWith(clip);
    const ClipId id = project.tracks.front().clips.front().id;
    ASSERT_EQ(project.tracks.front().clips.front().duration().ticks(), 4'000);

    SplitClipCommand degenerate(id, Duration::fromNanoseconds(1));
    const Result<void> refused = degenerate.apply(project);
    ASSERT_TRUE(refused.isError());
    EXPECT_EQ(refused.error().code(), ErrorCode::FailedPrecondition);
    // Refused without mutating: still one clip.
    EXPECT_EQ(project.tracks.front().clips.size(), 1u);

    // The same clip splits fine at a playhead that does map to an interior source position.
    SplitClipCommand valid(id, Duration::fromNanoseconds(2'000));
    ASSERT_TRUE(valid.apply(project).isOk());
    ASSERT_EQ(project.tracks.front().clips.size(), 2u);
    EXPECT_EQ(project.tracks.front().clips[0].sourceOut.ticks(), 500);
    EXPECT_EQ(project.tracks.front().clips[1].sourceIn.ticks(), 500);
}

// --- a speed change that would collide ------------------------------------

TEST(SetClipPropertiesSpeed, ASpeedThatWouldLengthenAClipIntoItsNeighbourIsRefused) {
    // Two contiguous 2 s clips. Halving the first one's speed makes it 4 s, which would run into
    // the second. Refused rather than applied, because a successful tool call must not leave a
    // timeline whose clips overlap outside a transition region.
    Project project = projectWith(makeClip(0.0, 0.0, 2.0, 1.0));
    Clip second = makeClip(2.0, 0.0, 2.0, 1.0);
    second.assetRef = project.assets.front();
    project.tracks.front().clips.push_back(second);
    ASSERT_TRUE(validateProject(project).isOk());

    const ClipId first = project.tracks.front().clips.front().id;
    SetClipPropertiesCommand::Properties slower;
    slower.speed = 0.5;
    SetClipPropertiesCommand command(first, slower);

    const Result<void> applied = command.apply(project);
    ASSERT_TRUE(applied.isError());
    EXPECT_EQ(applied.error().code(), ErrorCode::FailedPrecondition);
    EXPECT_NE(applied.error().message().find("speed"), std::string::npos)
        << applied.error().message();

    // And the clip is left exactly as it was, so the refusal is not a partial edit.
    EXPECT_DOUBLE_EQ(project.tracks.front().clips.front().speed, 1.0);
    EXPECT_NEAR(seconds(project.tracks.front().clips.front().duration()), 2.0, 1e-9);
    EXPECT_TRUE(validateProject(project).isOk());
}

TEST(SetClipPropertiesSpeed, ASpeedThatShortensAClipIsAlwaysApplicable) {
    // The mirror image: speeding a clip up only ever leaves a gap, so it cannot collide.
    Project project = projectWith(makeClip(0.0, 0.0, 2.0, 1.0));
    Clip second = makeClip(2.0, 0.0, 2.0, 1.0);
    second.assetRef = project.assets.front();
    project.tracks.front().clips.push_back(second);

    SetClipPropertiesCommand::Properties faster;
    faster.speed = 2.0;
    SetClipPropertiesCommand command(project.tracks.front().clips.front().id, faster);

    ASSERT_TRUE(command.apply(project).isOk());
    EXPECT_NEAR(seconds(project.tracks.front().clips.front().duration()), 1.0, 1e-9);
    EXPECT_TRUE(validateProject(project).isOk());

    ASSERT_TRUE(command.revert(project).isOk());
    EXPECT_DOUBLE_EQ(project.tracks.front().clips.front().speed, 1.0);
}

TEST(SetClipPropertiesSpeed, ALoneClipCanBeSlowedFreely) {
    // Nothing to run into, so the refusal must not fire.
    Project project = projectWith(makeClip(0.0, 0.0, 2.0, 1.0));
    SetClipPropertiesCommand::Properties slower;
    slower.speed = 0.25;
    SetClipPropertiesCommand command(project.tracks.front().clips.front().id, slower);

    ASSERT_TRUE(command.apply(project).isOk());
    EXPECT_NEAR(seconds(project.tracks.front().clips.front().duration()), 8.0, 1e-9);
    EXPECT_TRUE(validateProject(project).isOk());
}

// --- validation ------------------------------------------------------------

TEST(ClipSpeedValidation, AUsableSpeedValidatesAndAnUnusableOneIsRefused) {
    Project project = projectWith(makeClip(0.0, 0.0, 2.0, 1.0));
    EXPECT_TRUE(validateProject(project).isOk());

    project.tracks.front().clips.front().speed = 2.0;
    EXPECT_TRUE(validateProject(project).isOk());

    // Refused rather than clamped: duration() divides by speed, so a zero would give the clip a
    // zero length, which every track rule assumes cannot happen.
    project.tracks.front().clips.front().speed = 0.0;
    const Result<void> zero = validateProject(project);
    ASSERT_TRUE(zero.isError());
    EXPECT_EQ(zero.error().code(), ErrorCode::InvalidArgument);
    EXPECT_NE(zero.error().message().find("speed"), std::string::npos) << zero.error().message();

    project.tracks.front().clips.front().speed = -1.0;
    EXPECT_TRUE(validateProject(project).isError());
}

}  // namespace
}  // namespace palmier
