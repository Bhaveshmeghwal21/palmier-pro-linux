// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for the effect, tone-curve, text and caption commands, split out of
// edit_commands_test.cpp. Same target, so the test names are unchanged.
//
// _Requirements: 2.2, 2.3, 2.4, 2.5, 2.6, 2.7, 3.3, 3.8, 3.10_

#include "core/EditCommands.hpp"

#include <gtest/gtest.h>

#include "edit_commands_test_support.hpp"

namespace palmier {
namespace {

using namespace palmier::testsupport::editcommands;  // NOLINT(google-build-using-namespace)

// --- EditCurvePointCommand -------------------------------------------------
//
// Requirement 5.7: add, move and remove are each ONE undoable edit. The interesting
// claim is not that each mutates correctly but that each has an exact inverse, which
// for removal means restoring the NUMBERING and not merely the coordinates.

namespace {

/// A project holding one clip with one tone-curve effect, and the ids to address it.
Project makeProjectWithCurveEffect(ClipId& clipOut, Uuid& effectOut) {
    Uuid trackId;
    Project project = makeProjectWithOneTrack(trackId);
    clipOut = Uuid::generateV4();
    Effect effect;
    effect.id = Uuid::generateV4();
    effect.type = EffectType::ToneCurve;
    effectOut = effect.id;
    Clip clip = makeClip(clipOut, ms(0), ms(0), ms(1000));
    clip.effects = {effect};
    project.tracks[0].clips = {clip};
    return project;
}

const std::map<std::string, double>& curveParams(const Project& project) {
    return project.tracks[0].clips[0].effects[0].parameters;
}

/// Add points p0..pN-1 to one channel, each through its own command, so the fixture is
/// built by the code under test rather than by hand-written parameter names.
void addPoints(Project& project, ClipId clipId, const Uuid& effectId, CurveChannel channel,
               const std::vector<CurvePoint>& points) {
    for (const CurvePoint& p : points) {
        EditCurvePointCommand cmd(clipId, effectId, channel, EditCurvePointCommand::Operation::Add,
                                  0, p.x, p.y);
        ASSERT_TRUE(cmd.apply(project).isOk());
    }
}

}  // namespace

TEST(EditCurvePointCommand, AddingAPointIsOneEditAndRevertRemovesBothCoordinates) {
    ClipId clipId;
    Uuid effectId;
    Project project = makeProjectWithCurveEffect(clipId, effectId);

    EditCurvePointCommand cmd(clipId, effectId, CurveChannel::Master,
                              EditCurvePointCommand::Operation::Add, 0, 0.25, 0.75);
    ASSERT_TRUE(cmd.apply(project).isOk());

    const std::vector<CurvePoint> after = curvePoints(curveParams(project), CurveChannel::Master);
    ASSERT_EQ(after.size(), 1u);
    EXPECT_DOUBLE_EQ(after[0].x, 0.25);
    EXPECT_DOUBLE_EQ(after[0].y, 0.75);

    // The whole point of a dedicated command: one revert removes BOTH coordinates. Two
    // set_effect_parameter edits would leave an X with no Y behind, which curvePoints
    // refuses to read as a point -- so the curve would change shape after a single undo.
    ASSERT_TRUE(cmd.revert(project).isOk());
    EXPECT_TRUE(curvePoints(curveParams(project), CurveChannel::Master).empty());
    EXPECT_TRUE(curveParams(project).empty()) << "revert must leave no orphaned coordinate";
}

TEST(EditCurvePointCommand, MovingAPointReplacesBothCoordinatesAndRevertRestoresThem) {
    ClipId clipId;
    Uuid effectId;
    Project project = makeProjectWithCurveEffect(clipId, effectId);
    addPoints(project, clipId, effectId, CurveChannel::Red, {{0.2, 0.3}, {0.8, 0.9}});

    EditCurvePointCommand cmd(clipId, effectId, CurveChannel::Red,
                              EditCurvePointCommand::Operation::Move, 0, 0.1, 0.15);
    ASSERT_TRUE(cmd.apply(project).isOk());

    std::vector<CurvePoint> after = curvePoints(curveParams(project), CurveChannel::Red);
    ASSERT_EQ(after.size(), 2u);
    EXPECT_DOUBLE_EQ(after[0].x, 0.1);
    EXPECT_DOUBLE_EQ(after[0].y, 0.15);
    EXPECT_DOUBLE_EQ(after[1].x, 0.8) << "the other point must not move";

    ASSERT_TRUE(cmd.revert(project).isOk());
    after = curvePoints(curveParams(project), CurveChannel::Red);
    ASSERT_EQ(after.size(), 2u);
    EXPECT_DOUBLE_EQ(after[0].x, 0.2);
    EXPECT_DOUBLE_EQ(after[0].y, 0.3);
}

// The case the whole-channel capture exists for. Removing a middle point renumbers
// every point after it, so the inverse of "remove p1" is not "add a point at p1" --
// after the removal there IS no p1 with the old p2's coordinates to put back.
TEST(EditCurvePointCommand, RemovingAMiddlePointRenumbersAndRevertRestoresTheNumbering) {
    ClipId clipId;
    Uuid effectId;
    Project project = makeProjectWithCurveEffect(clipId, effectId);
    addPoints(project, clipId, effectId, CurveChannel::Master,
              {{0.0, 0.0}, {0.5, 0.9}, {1.0, 1.0}});

    EditCurvePointCommand cmd(clipId, effectId, CurveChannel::Master,
                              EditCurvePointCommand::Operation::Remove, 1, 0.0, 0.0);
    ASSERT_TRUE(cmd.apply(project).isOk());

    std::vector<CurvePoint> after = curvePoints(curveParams(project), CurveChannel::Master);
    ASSERT_EQ(after.size(), 2u) << "a gap would truncate the run and lose the last point";
    EXPECT_DOUBLE_EQ(after[1].x, 1.0) << "p2 must become p1, contiguously";
    // The old p2's parameter names must be GONE, not left behind as a duplicate tail.
    EXPECT_EQ(
        curveParams(project).count(curvePointParameterName(CurveChannel::Master, 2, /*isY=*/false)),
        0u);

    ASSERT_TRUE(cmd.revert(project).isOk());
    after = curvePoints(curveParams(project), CurveChannel::Master);
    ASSERT_EQ(after.size(), 3u);
    EXPECT_DOUBLE_EQ(after[1].x, 0.5);
    EXPECT_DOUBLE_EQ(after[1].y, 0.9);
    EXPECT_DOUBLE_EQ(after[2].x, 1.0);
}

TEST(EditCurvePointCommand, AnEditToOneChannelLeavesTheOthersAlone) {
    ClipId clipId;
    Uuid effectId;
    Project project = makeProjectWithCurveEffect(clipId, effectId);
    addPoints(project, clipId, effectId, CurveChannel::Master, {{0.5, 0.5}});
    addPoints(project, clipId, effectId, CurveChannel::Blue, {{0.25, 0.4}, {0.75, 0.6}});

    EditCurvePointCommand cmd(clipId, effectId, CurveChannel::Master,
                              EditCurvePointCommand::Operation::Remove, 0, 0.0, 0.0);
    ASSERT_TRUE(cmd.apply(project).isOk());
    EXPECT_TRUE(curvePoints(curveParams(project), CurveChannel::Master).empty());
    EXPECT_EQ(curvePoints(curveParams(project), CurveChannel::Blue).size(), 2u);

    // And the inverse is equally narrow: restoring master must not resurrect or disturb
    // blue. A capture keyed on the wrong prefix would pass the line above and fail here.
    ASSERT_TRUE(cmd.revert(project).isOk());
    EXPECT_EQ(curvePoints(curveParams(project), CurveChannel::Master).size(), 1u);
    const std::vector<CurvePoint> blue = curvePoints(curveParams(project), CurveChannel::Blue);
    ASSERT_EQ(blue.size(), 2u);
    EXPECT_DOUBLE_EQ(blue[1].y, 0.6);
}

TEST(EditCurvePointCommand, AnEditLeavesOtherParametersOnTheSameEffectUntouched) {
    ClipId clipId;
    Uuid effectId;
    Project project = makeProjectWithCurveEffect(clipId, effectId);
    project.tracks[0].clips[0].effects[0].parameters["amount"] = 0.42;
    addPoints(project, clipId, effectId, CurveChannel::Green, {{0.3, 0.7}});

    EditCurvePointCommand cmd(clipId, effectId, CurveChannel::Green,
                              EditCurvePointCommand::Operation::Add, 0, 0.6, 0.2);
    ASSERT_TRUE(cmd.apply(project).isOk());
    EXPECT_DOUBLE_EQ(curveParams(project).at("amount"), 0.42);
    ASSERT_TRUE(cmd.revert(project).isOk());
    EXPECT_DOUBLE_EQ(curveParams(project).at("amount"), 0.42)
        << "the wholesale restore must be scoped to the channel, not to the effect";
}

TEST(EditCurvePointCommand, MovingOrRemovingAPointThatDoesNotExistIsRefused) {
    ClipId clipId;
    Uuid effectId;
    Project project = makeProjectWithCurveEffect(clipId, effectId);
    addPoints(project, clipId, effectId, CurveChannel::Master, {{0.5, 0.5}});

    EditCurvePointCommand move(clipId, effectId, CurveChannel::Master,
                               EditCurvePointCommand::Operation::Move, 3, 0.1, 0.1);
    const auto moved = move.apply(project);
    ASSERT_TRUE(moved.isError());
    EXPECT_EQ(moved.error().code(), ErrorCode::NotFound);

    EditCurvePointCommand remove(clipId, effectId, CurveChannel::Master,
                                 EditCurvePointCommand::Operation::Remove, 1, 0.0, 0.0);
    EXPECT_TRUE(remove.apply(project).isError());

    // A refused edit changes nothing, so the one real point is still there.
    EXPECT_EQ(curvePoints(curveParams(project), CurveChannel::Master).size(), 1u);
}

TEST(EditCurvePointCommand, AddingToAnEmptyCurveNeedsNoIndexAndRevertBeforeApplyFails) {
    ClipId clipId;
    Uuid effectId;
    Project project = makeProjectWithCurveEffect(clipId, effectId);

    // index 9 with operation Add: ignored rather than refused, because add appends.
    EditCurvePointCommand cmd(clipId, effectId, CurveChannel::Master,
                              EditCurvePointCommand::Operation::Add, 9, 0.4, 0.4);
    EditCurvePointCommand never(clipId, effectId, CurveChannel::Master,
                                EditCurvePointCommand::Operation::Add, 0, 0.4, 0.4);
    const auto premature = never.revert(project);
    ASSERT_TRUE(premature.isError());
    EXPECT_EQ(premature.error().code(), ErrorCode::FailedPrecondition);

    ASSERT_TRUE(cmd.apply(project).isOk());
    const std::vector<CurvePoint> after = curvePoints(curveParams(project), CurveChannel::Master);
    ASSERT_EQ(after.size(), 1u) << "add appends at the next free index, whatever index says";
    EXPECT_DOUBLE_EQ(after[0].x, 0.4);
}

TEST(EditCurvePointCommand, AMissingClipOrEffectIsRefusedAndNamed) {
    ClipId clipId;
    Uuid effectId;
    Project project = makeProjectWithCurveEffect(clipId, effectId);

    EditCurvePointCommand noClip(Uuid::generateV4(), effectId, CurveChannel::Master,
                                 EditCurvePointCommand::Operation::Add, 0, 0.5, 0.5);
    auto result = noClip.apply(project);
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::NotFound);

    EditCurvePointCommand noEffect(clipId, Uuid::generateV4(), CurveChannel::Master,
                                   EditCurvePointCommand::Operation::Add, 0, 0.5, 0.5);
    result = noEffect.apply(project);
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::NotFound);
    EXPECT_NE(result.error().message().find("effect"), std::string::npos);
}

// --- SetEffectResourceCommand ----------------------------------------------
//
// Requirement 7.9: applying a LUT by path is ONE undoable edit. The interesting case is the
// EMPTY prior, because it is the common one -- an effect just added has no resource -- and
// treating "no prior" as "nothing to restore" would make the first LUT applied to an effect
// un-undoable.

TEST(SetEffectResourceCommand, SetsThePathAndRevertRestoresAnEmptyPrior) {
    ClipId clipId;
    Uuid effectId;
    Project project = makeProjectWithCurveEffect(clipId, effectId);
    ASSERT_TRUE(project.tracks[0].clips[0].effects[0].resourcePath.empty());

    SetEffectResourceCommand cmd(clipId, effectId, "/luts/look.cube");
    ASSERT_TRUE(cmd.apply(project).isOk());
    EXPECT_EQ(project.tracks[0].clips[0].effects[0].resourcePath, "/luts/look.cube");

    ASSERT_TRUE(cmd.revert(project).isOk());
    EXPECT_TRUE(project.tracks[0].clips[0].effects[0].resourcePath.empty())
        << "an empty prior must be restored, not skipped";
}

TEST(SetEffectResourceCommand, ReplacingAPathRestoresThePreviousOneNotAnEmptyString) {
    ClipId clipId;
    Uuid effectId;
    Project project = makeProjectWithCurveEffect(clipId, effectId);
    project.tracks[0].clips[0].effects[0].resourcePath = "/luts/first.cube";

    SetEffectResourceCommand cmd(clipId, effectId, "/luts/second.cube");
    ASSERT_TRUE(cmd.apply(project).isOk());
    EXPECT_EQ(project.tracks[0].clips[0].effects[0].resourcePath, "/luts/second.cube");
    ASSERT_TRUE(cmd.revert(project).isOk());
    EXPECT_EQ(project.tracks[0].clips[0].effects[0].resourcePath, "/luts/first.cube");
}

TEST(SetEffectResourceCommand, AnEmptyPathClearsTheResourceWithoutRemovingTheEffect) {
    ClipId clipId;
    Uuid effectId;
    Project project = makeProjectWithCurveEffect(clipId, effectId);
    project.tracks[0].clips[0].effects[0].resourcePath = "/luts/look.cube";

    SetEffectResourceCommand cmd(clipId, effectId, "");
    ASSERT_TRUE(cmd.apply(project).isOk());
    EXPECT_TRUE(project.tracks[0].clips[0].effects[0].resourcePath.empty());
    EXPECT_EQ(project.tracks[0].clips[0].effects.size(), 1u)
        << "clearing a LUT must not remove the effect it was on";
}

// Requirement 7.8 reaching the command: a path that cannot be read is still accepted, or a
// project that opens on one machine would be un-editable on another, and an agent setting up
// a project before its assets arrive is an ordinary workflow.
TEST(SetEffectResourceCommand, ANonExistentPathIsAcceptedRatherThanRefused) {
    ClipId clipId;
    Uuid effectId;
    Project project = makeProjectWithCurveEffect(clipId, effectId);
    SetEffectResourceCommand cmd(clipId, effectId, "/definitely/not/here.cube");
    EXPECT_TRUE(cmd.apply(project).isOk());
    EXPECT_EQ(project.tracks[0].clips[0].effects[0].resourcePath, "/definitely/not/here.cube");
}

TEST(SetEffectResourceCommand, AMissingClipOrEffectIsRefusedAndRevertBeforeApplyFails) {
    ClipId clipId;
    Uuid effectId;
    Project project = makeProjectWithCurveEffect(clipId, effectId);

    SetEffectResourceCommand noClip(Uuid::generateV4(), effectId, "/luts/a.cube");
    auto result = noClip.apply(project);
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::NotFound);

    SetEffectResourceCommand noEffect(clipId, Uuid::generateV4(), "/luts/a.cube");
    EXPECT_TRUE(noEffect.apply(project).isError());

    SetEffectResourceCommand never(clipId, effectId, "/luts/a.cube");
    result = never.revert(project);
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::FailedPrecondition);

    // Nothing was changed by any of the three refusals.
    EXPECT_TRUE(project.tracks[0].clips[0].effects[0].resourcePath.empty());
}

// --- AddEffectCommand ------------------------------------------------------

TEST(AddEffectCommand, AppendsAndRevertRemoves) {
    Uuid trackId;
    Project project = makeProjectWithOneTrack(trackId);
    const ClipId a = Uuid::generateV4();
    project.tracks[0].clips = {makeClip(a, ms(0), ms(0), ms(1000))};

    const Effect effect = Effect::brightness(0.2);
    AddEffectCommand cmd(a, effect);
    ASSERT_TRUE(cmd.apply(project).isOk());
    ASSERT_EQ(project.tracks[0].clips[0].effects.size(), 1u);
    EXPECT_EQ(project.tracks[0].clips[0].effects[0].id, effect.id);

    ASSERT_TRUE(cmd.revert(project).isOk());
    EXPECT_TRUE(project.tracks[0].clips[0].effects.empty());
}

TEST(AddEffectCommand, MissingClipFails) {
    Uuid trackId;
    Project project = makeProjectWithOneTrack(trackId);
    AddEffectCommand cmd(Uuid::generateV4(), Effect::blur(2.0));
    const auto result = cmd.apply(project);
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::NotFound);
}

TEST(SetTextContentCommand, ChangesTheStringAndUndoRestoresIt) {
    Uuid trackId;
    Project project = makeProjectWithOneTextTrack(trackId);
    const ClipId clipId = Uuid::generateV4();
    project.tracks[0].clips.push_back(makeTextClip(clipId, ms(0), ms(1000), "Original"));
    TimelineEngine engine(std::move(project));

    ASSERT_TRUE(engine.apply(std::make_unique<SetTextContentCommand>(clipId, "Changed")).changed());
    EXPECT_EQ(engine.snapshot().tracks[0].clips[0].textStyle->content, "Changed");

    ASSERT_TRUE(engine.undo().changed());
    EXPECT_EQ(engine.snapshot().tracks[0].clips[0].textStyle->content, "Original");
}

TEST(SetTextContentCommand, RefusesAClipThatIsNotATextClip) {
    Uuid trackId;
    Project project = makeProjectWithOneTrack(trackId);
    const ClipId clipId = Uuid::generateV4();
    project.tracks[0].clips.push_back(makeClip(clipId, ms(0), ms(0), ms(1000)));
    TimelineEngine engine(std::move(project));

    const CommandResult result =
        engine.apply(std::make_unique<SetTextContentCommand>(clipId, "New text"));
    EXPECT_FALSE(result.changed());
    EXPECT_FALSE(engine.canUndo());
}

// Requirement 9.2: any subset of font family, size, colour, alignment and
// position changes in one undoable edit — mirroring
// SetProjectSettingsCommand's own optional-field pattern. Only two of the nine
// possible fields are supplied here; the rest must be left exactly as they were.
TEST(SetTextStyleCommand, ChangesOnlyTheSuppliedFieldsAndUndoRestoresAll) {
    Uuid trackId;
    Project project = makeProjectWithOneTextTrack(trackId);
    const ClipId clipId = Uuid::generateV4();
    Clip clip = makeTextClip(clipId, ms(0), ms(1000), "Title");
    clip.textStyle->fontFamily = "serif";
    clip.textStyle->pointSize = 18.0;
    clip.textStyle->colorR = 0.2;
    clip.textStyle->alignment = TextAlignment::Left;
    clip.textStyle->x = 0.1;
    clip.textStyle->y = 0.9;
    const TextStyle before = *clip.textStyle;
    project.tracks[0].clips.push_back(std::move(clip));
    TimelineEngine engine(std::move(project));

    // Only pointSize and alignment are supplied; everything else is std::nullopt.
    ASSERT_TRUE(engine
                    .apply(std::make_unique<SetTextStyleCommand>(
                        clipId, std::nullopt, 36.0, std::nullopt, std::nullopt, std::nullopt,
                        std::nullopt, TextAlignment::Right, std::nullopt, std::nullopt))
                    .changed());

    const TextStyle after = *engine.snapshot().tracks[0].clips[0].textStyle;
    EXPECT_EQ(after.pointSize, 36.0);                  // changed
    EXPECT_EQ(after.alignment, TextAlignment::Right);  // changed
    EXPECT_EQ(after.fontFamily, before.fontFamily);    // untouched
    EXPECT_EQ(after.colorR, before.colorR);            // untouched
    EXPECT_EQ(after.x, before.x);                      // untouched
    EXPECT_EQ(after.y, before.y);                      // untouched

    ASSERT_TRUE(engine.undo().changed());
    const TextStyle restored = *engine.snapshot().tracks[0].clips[0].textStyle;
    EXPECT_EQ(restored.pointSize, before.pointSize);
    EXPECT_EQ(restored.alignment, before.alignment);
    EXPECT_EQ(restored.fontFamily, before.fontFamily);
}

// Requirement 9's own numeric bound (point size must be > 0) is enforced by
// TextStyle::isValid(), and a change that would violate it is refused with the
// project left exactly as it was — the same own-invariant check
// SetProjectSettingsCommand already establishes for fps/canvas.
TEST(SetTextStyleCommand, RefusesAChangeThatWouldMakeTheStyleInvalidAndLeavesItUnchanged) {
    Uuid trackId;
    Project project = makeProjectWithOneTextTrack(trackId);
    const ClipId clipId = Uuid::generateV4();
    project.tracks[0].clips.push_back(makeTextClip(clipId, ms(0), ms(1000), "Title"));
    TimelineEngine engine(std::move(project));

    const CommandResult result = engine.apply(std::make_unique<SetTextStyleCommand>(
        clipId, std::nullopt, -5.0, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
        std::nullopt, std::nullopt, std::nullopt));  // pointSize -5 is not > 0
    EXPECT_FALSE(result.changed());
    EXPECT_EQ(engine.snapshot().tracks[0].clips[0].textStyle->pointSize,
              24.0);  // default, untouched
    EXPECT_FALSE(engine.canUndo());
}

TEST(SetTextStyleCommand, RefusesAClipThatIsNotATextClip) {
    Uuid trackId;
    Project project = makeProjectWithOneTrack(trackId);
    const ClipId clipId = Uuid::generateV4();
    project.tracks[0].clips.push_back(makeClip(clipId, ms(0), ms(0), ms(1000)));
    TimelineEngine engine(std::move(project));

    const CommandResult result = engine.apply(std::make_unique<SetTextStyleCommand>(
        clipId, std::nullopt, 36.0, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
        std::nullopt, std::nullopt, std::nullopt));
    EXPECT_FALSE(result.changed());
    EXPECT_FALSE(engine.canUndo());
}

TEST(SetCaptionTextCommand, ChangesTheStringAndUndoRestoresIt) {
    Uuid trackId;
    Project project = makeProjectWithOneCaptionTrack(trackId);
    const ClipId clipId = Uuid::generateV4();
    project.tracks[0].clips.push_back(makeCaptionCue(clipId, ms(0), ms(1000), "Original"));
    TimelineEngine engine(std::move(project));

    ASSERT_TRUE(engine.apply(std::make_unique<SetCaptionTextCommand>(clipId, "Changed")).changed());
    EXPECT_EQ(*engine.snapshot().tracks[0].clips[0].captionText, "Changed");

    ASSERT_TRUE(engine.undo().changed());
    EXPECT_EQ(*engine.snapshot().tracks[0].clips[0].captionText, "Original");
}

TEST(SetCaptionTextCommand, RefusesAnEmptyString) {
    Uuid trackId;
    Project project = makeProjectWithOneCaptionTrack(trackId);
    const ClipId clipId = Uuid::generateV4();
    project.tracks[0].clips.push_back(makeCaptionCue(clipId, ms(0), ms(1000), "Original"));
    TimelineEngine engine(std::move(project));

    const CommandResult result = engine.apply(std::make_unique<SetCaptionTextCommand>(clipId, ""));
    EXPECT_FALSE(result.changed());
    EXPECT_EQ(*engine.snapshot().tracks[0].clips[0].captionText, "Original");
    EXPECT_FALSE(engine.canUndo());
}

TEST(SetCaptionTextCommand, RefusesAClipThatIsNotACaptionCue) {
    Uuid trackId;
    Project project = makeProjectWithOneTrack(trackId);
    const ClipId clipId = Uuid::generateV4();
    project.tracks[0].clips.push_back(makeClip(clipId, ms(0), ms(0), ms(1000)));
    TimelineEngine engine(std::move(project));

    const CommandResult result =
        engine.apply(std::make_unique<SetCaptionTextCommand>(clipId, "New text"));
    EXPECT_FALSE(result.changed());
    EXPECT_FALSE(engine.canUndo());
}

// Requirement 10.2: "retime" changes timelineStart and/or duration together in
// ONE undoable edit — mirroring SetTextStyleCommand's own optional-field
// pattern, but over the two timing fields RetimeCaptionCueCommand owns.
TEST(RetimeCaptionCueCommand, ChangesBothFieldsTogetherAndUndoRestoresThem) {
    Uuid trackId;
    Project project = makeProjectWithOneCaptionTrack(trackId);
    const ClipId clipId = Uuid::generateV4();
    project.tracks[0].clips.push_back(makeCaptionCue(clipId, ms(0), ms(1000), "Hello"));
    TimelineEngine engine(std::move(project));

    ASSERT_TRUE(engine.apply(std::make_unique<RetimeCaptionCueCommand>(clipId, ms(2000), ms(500)))
                    .changed());

    const Clip& after = engine.snapshot().tracks[0].clips[0];
    EXPECT_EQ(after.timelineStart, ms(2000));
    EXPECT_EQ(after.duration(), ms(500));
    EXPECT_EQ(*after.captionText, "Hello");  // retiming touched nothing else

    ASSERT_TRUE(engine.undo().changed());
    const Clip& restored = engine.snapshot().tracks[0].clips[0];
    EXPECT_EQ(restored.timelineStart, ms(0));
    EXPECT_EQ(restored.duration(), ms(1000));
}

TEST(RetimeCaptionCueCommand, ChangesOnlyTheSuppliedField) {
    Uuid trackId;
    Project project = makeProjectWithOneCaptionTrack(trackId);
    const ClipId clipId = Uuid::generateV4();
    project.tracks[0].clips.push_back(makeCaptionCue(clipId, ms(1000), ms(500), "Hello"));
    TimelineEngine engine(std::move(project));

    // Only the start is supplied; duration must be left exactly as it was.
    ASSERT_TRUE(
        engine.apply(std::make_unique<RetimeCaptionCueCommand>(clipId, ms(3000), std::nullopt))
            .changed());

    const Clip& after = engine.snapshot().tracks[0].clips[0];
    EXPECT_EQ(after.timelineStart, ms(3000));
    EXPECT_EQ(after.duration(), ms(500));
}

TEST(RetimeCaptionCueCommand, RefusesAMoveThatWouldOverlapAnotherCueOnTheSameTrack) {
    Uuid trackId;
    Project project = makeProjectWithOneCaptionTrack(trackId);
    const ClipId firstId = Uuid::generateV4();
    const ClipId secondId = Uuid::generateV4();
    project.tracks[0].clips.push_back(makeCaptionCue(firstId, ms(0), ms(1000), "First"));
    project.tracks[0].clips.push_back(makeCaptionCue(secondId, ms(2000), ms(1000), "Second"));
    TimelineEngine engine(std::move(project));

    // Moving "Second" to overlap "First" must be refused and leave both cues
    // exactly where they were (mirroring MoveClipCommand's own overlap rule).
    const CommandResult result =
        engine.apply(std::make_unique<RetimeCaptionCueCommand>(secondId, ms(500), std::nullopt));
    EXPECT_FALSE(result.changed());
    EXPECT_EQ(engine.snapshot().tracks[0].clips.size(), 2u);
    EXPECT_FALSE(engine.canUndo());
}

TEST(RetimeCaptionCueCommand, RefusesAClipThatIsNotACaptionCue) {
    Uuid trackId;
    Project project = makeProjectWithOneTrack(trackId);
    const ClipId clipId = Uuid::generateV4();
    project.tracks[0].clips.push_back(makeClip(clipId, ms(0), ms(0), ms(1000)));
    TimelineEngine engine(std::move(project));

    const CommandResult result =
        engine.apply(std::make_unique<RetimeCaptionCueCommand>(clipId, ms(500), std::nullopt));
    EXPECT_FALSE(result.changed());
    EXPECT_FALSE(engine.canUndo());
}
}  // namespace
}  // namespace palmier
