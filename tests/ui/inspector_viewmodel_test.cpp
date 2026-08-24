// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for the Qt-free Inspector/Effects view model (task 19.4;
// Requirement 2.4).
//
// These exercise the whole selection -> read-projection -> edit -> command
// mapping without any Qt dependency:
//   * the projection reflects the selected clip's properties and effect chain
//     (with parameters in a deterministic, name-sorted order);
//   * adding an effect issues an AddEffectCommand and shows up in the engine
//     snapshot and the projection;
//   * editing an effect parameter and a clip property (opacity/gain) maps to the
//     right command, reflects in the engine, and undoes exactly;
//   * an out-of-range property value is rejected by the engine's invariant check
//     and leaves the project unchanged;
//   * trimming maps to TrimClipCommand, constrained to [1 frame, source duration]
//     (Requirement 2.4);
//   * the onChanged callback fires on selection changes, on the model's own
//     edits, and on external edits (undo / other surfaces).
//
// The view model and its own extra command (SetClipPropertyCommand) are compiled
// directly into this binary alongside Palmier::core, so the tests run without Qt
// or PALMIER_BUILD_UI. SetEffectParameterCommand, RemoveEffectCommand and
// ReorderEffectsCommand (task 9) now live in core::EditCommands, since
// `timeline.set_effect_parameter`/`remove_effect`/`reorder_effects` are published
// Tool_Surface operations and not Inspector-only.

#include "ui/InspectorViewModel.hpp"

#include <cstdint>
#include <memory>
#include <optional>

#include <gtest/gtest.h>

#include "core/Clip.hpp"
#include "core/CommandResult.hpp"
#include "core/EditCommands.hpp"
#include "core/Effect.hpp"
#include "core/FrameRate.hpp"
#include "core/Project.hpp"
#include "core/TextStyle.hpp"
#include "core/TimelineEngine.hpp"
#include "core/Track.hpp"
#include "core/Uuid.hpp"

namespace palmier {
namespace ui {
namespace {

constexpr Duration ms(std::int64_t v) { return Duration::fromMilliseconds(v); }

Clip makeClip(ClipId id, Duration timelineStart, Duration sourceIn, Duration sourceOut) {
    Clip clip;
    clip.id = id;
    clip.assetRef = MediaAssetRef(Uuid::generateV4(), "mem://asset");
    clip.timelineStart = timelineStart;
    clip.sourceIn = sourceIn;
    clip.sourceOut = sourceOut;
    return clip;
}

// A project with one video track holding a single [0,1000)ms clip.
Project makeProjectWithClip(ClipId clipId) {
    Project project;
    project.id = Uuid::generateV4();
    project.name = "test";
    project.timelineFps = FrameRate::fps24();
    Track track;
    track.id = Uuid::generateV4();
    track.kind = TrackKind::Video;
    track.clips.push_back(makeClip(clipId, ms(0), ms(0), ms(1000)));
    project.tracks.push_back(std::move(track));
    return project;
}

// --- Selection / projection ------------------------------------------------

TEST(InspectorViewModel, NoSelectionHasEmptyProjectionAndRejectsEdits) {
    const ClipId clip = Uuid::generateV4();
    TimelineEngine engine(makeProjectWithClip(clip));
    InspectorViewModel model(engine);

    EXPECT_FALSE(model.hasSelection());
    EXPECT_FALSE(model.selectedClip().has_value());

    const CommandResult r = model.setOpacity(0.5);
    EXPECT_TRUE(r.isError());
    EXPECT_EQ(r.error().code(), ErrorCode::FailedPrecondition);
    // Nothing was applied.
    EXPECT_EQ(engine.clip(clip)->opacity, 1.0);
}

TEST(InspectorViewModel, ProjectionReflectsClipPropertiesAndSortedEffectParameters) {
    const ClipId clip = Uuid::generateV4();
    Project project = makeProjectWithClip(clip);
    // Seed the clip with an effect carrying two parameters (insertion order b, a).
    Effect fx(Uuid::generateV4(), EffectType::ColorGrade, {{"zed", 2.0}, {"alpha", 1.0}});
    project.tracks[0].clips[0].effects.push_back(fx);
    project.tracks[0].clips[0].opacity = 0.4;
    project.tracks[0].clips[0].gain = 1.5;
    TimelineEngine engine(std::move(project));
    InspectorViewModel model(engine);

    model.selectClip(clip);
    const auto view = model.selectedClip();
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->id, clip);
    EXPECT_EQ(view->opacity, 0.4);
    EXPECT_EQ(view->gain, 1.5);
    EXPECT_EQ(view->duration, ms(1000));
    ASSERT_EQ(view->effects.size(), 1u);
    EXPECT_EQ(view->effects[0].id, fx.id);
    EXPECT_EQ(view->effects[0].type, EffectType::ColorGrade);
    // Parameters are exposed in name-sorted order (alpha before zed).
    ASSERT_EQ(view->effects[0].parameters.size(), 2u);
    EXPECT_EQ(view->effects[0].parameters[0].name, "alpha");
    EXPECT_EQ(view->effects[0].parameters[1].name, "zed");
}

TEST(InspectorViewModel, SelectingRemovedClipYieldsEmptyProjection) {
    const ClipId clip = Uuid::generateV4();
    TimelineEngine engine(makeProjectWithClip(clip));
    InspectorViewModel model(engine);

    model.selectClip(Uuid::generateV4());  // an id not present in the project
    EXPECT_TRUE(model.hasSelection());
    EXPECT_FALSE(model.selectedClip().has_value());
}

// --- Add effect ------------------------------------------------------------

TEST(InspectorViewModel, AddEffectIssuesAddEffectCommand) {
    const ClipId clip = Uuid::generateV4();
    TimelineEngine engine(makeProjectWithClip(clip));
    InspectorViewModel model(engine);
    model.selectClip(clip);

    const CommandResult r = model.addBrightnessEffect(0.2);
    ASSERT_TRUE(r.changed());
    EXPECT_EQ(r.message(), "AddEffect");

    // Reflected in the engine snapshot and in the projection.
    ASSERT_EQ(engine.clip(clip)->effects.size(), 1u);
    EXPECT_EQ(engine.clip(clip)->effects[0].type, EffectType::Brightness);
    const auto view = model.selectedClip();
    ASSERT_TRUE(view.has_value());
    ASSERT_EQ(view->effects.size(), 1u);
    EXPECT_EQ(view->effects[0].parameters[0].name, "amount");
    EXPECT_EQ(view->effects[0].parameters[0].value, 0.2);
}

// --- Set effect parameter --------------------------------------------------

TEST(InspectorViewModel, SetEffectParameterUpdatesValueAndUndoesExactly) {
    const ClipId clip = Uuid::generateV4();
    Project project = makeProjectWithClip(clip);
    Effect fx = Effect::brightness(0.1);
    project.tracks[0].clips[0].effects.push_back(fx);
    TimelineEngine engine(std::move(project));
    InspectorViewModel model(engine);
    model.selectClip(clip);

    const CommandResult r = model.setEffectParameter(fx.id, "amount", 0.75);
    ASSERT_TRUE(r.changed());
    EXPECT_EQ(engine.clip(clip)->effects[0].parameters.at("amount"), 0.75);

    // Undo restores the prior parameter value exactly.
    ASSERT_TRUE(engine.undo().changed());
    EXPECT_EQ(engine.clip(clip)->effects[0].parameters.at("amount"), 0.1);
}

TEST(InspectorViewModel, SetEffectParameterOnMissingEffectFailsAndLeavesProjectUnchanged) {
    const ClipId clip = Uuid::generateV4();
    TimelineEngine engine(makeProjectWithClip(clip));
    InspectorViewModel model(engine);
    model.selectClip(clip);

    const CommandResult r = model.setEffectParameter(Uuid::generateV4(), "amount", 0.5);
    EXPECT_TRUE(r.isError());
    EXPECT_EQ(r.error().code(), ErrorCode::NotFound);
    EXPECT_TRUE(engine.clip(clip)->effects.empty());
}

// --- Remove effect (task 9.2; Requirement 6.1, 6.2, 6.5) --------------------

TEST(InspectorViewModel, RemoveEffectRemovesItAndUndoesExactly) {
    const ClipId clip = Uuid::generateV4();
    Project project = makeProjectWithClip(clip);
    const Effect fx = Effect::brightness(0.1);
    project.tracks[0].clips[0].effects.push_back(fx);
    TimelineEngine engine(std::move(project));
    InspectorViewModel model(engine);
    model.selectClip(clip);

    const CommandResult r = model.removeEffect(fx.id);
    ASSERT_TRUE(r.changed());
    EXPECT_TRUE(engine.clip(clip)->effects.empty());

    // Requirement 6.2: the removal is one Undo, and it is exact.
    ASSERT_TRUE(engine.undo().changed());
    ASSERT_EQ(engine.clip(clip)->effects.size(), 1u);
    EXPECT_EQ(engine.clip(clip)->effects[0].id, fx.id);
    EXPECT_FALSE(engine.canUndo());
}

TEST(InspectorViewModel, RemoveEffectOnMissingEffectFailsAndLeavesProjectUnchanged) {
    const ClipId clip = Uuid::generateV4();
    Project project = makeProjectWithClip(clip);
    project.tracks[0].clips[0].effects.push_back(Effect::brightness(0.1));
    TimelineEngine engine(std::move(project));
    InspectorViewModel model(engine);
    model.selectClip(clip);

    // Requirement 6.5: naming an effect the clip does not carry is refused.
    const CommandResult r = model.removeEffect(Uuid::generateV4());
    EXPECT_TRUE(r.isError());
    EXPECT_EQ(r.error().code(), ErrorCode::NotFound);
    EXPECT_EQ(engine.clip(clip)->effects.size(), 1u);
    EXPECT_FALSE(engine.canUndo());
}

// --- Reorder effects (task 9.2; Requirement 6.1, 6.4, 6.5) ------------------

TEST(InspectorViewModel, ReorderEffectsChangesOrderAndUndoesExactly) {
    const ClipId clip = Uuid::generateV4();
    Project project = makeProjectWithClip(clip);
    const Effect first = Effect::brightness(0.1);
    const Effect second = Effect::contrast(0.2);
    project.tracks[0].clips[0].effects.push_back(first);
    project.tracks[0].clips[0].effects.push_back(second);
    TimelineEngine engine(std::move(project));
    InspectorViewModel model(engine);
    model.selectClip(clip);

    const CommandResult r = model.reorderEffects({second.id, first.id});
    ASSERT_TRUE(r.changed());
    ASSERT_EQ(engine.clip(clip)->effects.size(), 2u);
    EXPECT_EQ(engine.clip(clip)->effects[0].id, second.id);
    EXPECT_EQ(engine.clip(clip)->effects[1].id, first.id);

    // One Undo restores the prior order exactly.
    ASSERT_TRUE(engine.undo().changed());
    EXPECT_EQ(engine.clip(clip)->effects[0].id, first.id);
    EXPECT_EQ(engine.clip(clip)->effects[1].id, second.id);
    EXPECT_FALSE(engine.canUndo());
}

TEST(InspectorViewModel, ReorderEffectsWithAnUnknownIdFailsAndLeavesProjectUnchanged) {
    const ClipId clip = Uuid::generateV4();
    Project project = makeProjectWithClip(clip);
    const Effect fx = Effect::brightness(0.1);
    project.tracks[0].clips[0].effects.push_back(fx);
    TimelineEngine engine(std::move(project));
    InspectorViewModel model(engine);
    model.selectClip(clip);

    // Requirement 6.5: an id the clip does not carry is refused, not substituted.
    const CommandResult r = model.reorderEffects({Uuid::generateV4()});
    EXPECT_TRUE(r.isError());
    EXPECT_EQ(engine.clip(clip)->effects.size(), 1u);
    EXPECT_EQ(engine.clip(clip)->effects[0].id, fx.id);
    EXPECT_FALSE(engine.canUndo());
}

// --- Set opacity / gain ----------------------------------------------------

TEST(InspectorViewModel, SetOpacityAndGainApplyAndUndo) {
    const ClipId clip = Uuid::generateV4();
    TimelineEngine engine(makeProjectWithClip(clip));
    InspectorViewModel model(engine);
    model.selectClip(clip);

    ASSERT_TRUE(model.setOpacity(0.25).changed());
    EXPECT_EQ(engine.clip(clip)->opacity, 0.25);
    ASSERT_TRUE(model.setGain(2.0).changed());
    EXPECT_EQ(engine.clip(clip)->gain, 2.0);

    // Undo the gain change, then the opacity change (LIFO).
    ASSERT_TRUE(engine.undo().changed());
    EXPECT_EQ(engine.clip(clip)->gain, 1.0);
    ASSERT_TRUE(engine.undo().changed());
    EXPECT_EQ(engine.clip(clip)->opacity, 1.0);
}

TEST(InspectorViewModel, OutOfRangeOpacityIsRejectedByEngineAndLeavesClipUnchanged) {
    const ClipId clip = Uuid::generateV4();
    TimelineEngine engine(makeProjectWithClip(clip));
    InspectorViewModel model(engine);
    model.selectClip(clip);

    // opacity must be in [0,1]; 2.0 violates the invariant -> rolled back.
    const CommandResult r = model.setOpacity(2.0);
    EXPECT_TRUE(r.isError());
    EXPECT_EQ(engine.clip(clip)->opacity, 1.0);  // unchanged
}

TEST(InspectorViewModel, NegativeGainIsRejectedByEngineAndLeavesClipUnchanged) {
    const ClipId clip = Uuid::generateV4();
    TimelineEngine engine(makeProjectWithClip(clip));
    InspectorViewModel model(engine);
    model.selectClip(clip);

    const CommandResult r = model.setGain(-1.0);  // gain must be >= 0
    EXPECT_TRUE(r.isError());
    EXPECT_EQ(engine.clip(clip)->gain, 1.0);  // unchanged
}

// --- Trim (Requirement 2.4) ------------------------------------------------

TEST(InspectorViewModel, TrimEndMapsToTrimClipCommandAndSetsOutPoint) {
    const ClipId clip = Uuid::generateV4();
    TimelineEngine engine(makeProjectWithClip(clip));
    InspectorViewModel model(engine);
    model.selectClip(clip);

    ASSERT_TRUE(model.trimEnd(ms(600), FrameRate::fps24(), ms(2000)).changed());
    EXPECT_EQ(engine.clip(clip)->sourceOut, ms(600));
    EXPECT_EQ(engine.clip(clip)->duration(), ms(600));
    EXPECT_EQ(engine.clip(clip)->timelineStart, ms(0));  // end edge leaves start fixed
}

TEST(InspectorViewModel, TrimEndClampsToSourceDuration) {
    const ClipId clip = Uuid::generateV4();
    TimelineEngine engine(makeProjectWithClip(clip));
    InspectorViewModel model(engine);
    model.selectClip(clip);

    // Requesting an out-point past the source clamps to sourceDuration (2000ms).
    ASSERT_TRUE(model.trimEnd(ms(9000), FrameRate::fps24(), ms(2000)).changed());
    EXPECT_EQ(engine.clip(clip)->sourceOut, ms(2000));
}

TEST(InspectorViewModel, TrimStartShiftsInPointAndTimeline) {
    const ClipId clip = Uuid::generateV4();
    TimelineEngine engine(makeProjectWithClip(clip));
    InspectorViewModel model(engine);
    model.selectClip(clip);

    ASSERT_TRUE(model.trimStart(ms(300), FrameRate::fps24(), ms(2000)).changed());
    EXPECT_EQ(engine.clip(clip)->sourceIn, ms(300));
    EXPECT_EQ(engine.clip(clip)->timelineStart, ms(300));
    EXPECT_EQ(engine.clip(clip)->duration(), ms(700));
}

// --- Text and titles (usable-editor task 12; Requirement 9) ----------------

// A project with one TEXT track holding a single text clip (no assetRef,
// mirroring how AddClipCommand's own asset-registration step treats one).
Project makeProjectWithTextClip(ClipId clipId) {
    Project project;
    project.id = Uuid::generateV4();
    project.name = "test";
    project.timelineFps = FrameRate::fps24();
    Track track;
    track.id = Uuid::generateV4();
    track.kind = TrackKind::Text;
    Clip clip;
    clip.id = clipId;
    clip.timelineStart = ms(0);
    clip.sourceIn = Duration::zero();
    clip.sourceOut = ms(1000);
    TextStyle style;
    style.content = "Title";
    clip.textStyle = std::move(style);
    track.clips.push_back(std::move(clip));
    project.tracks.push_back(std::move(track));
    return project;
}

// Usable-editor task 13; Requirement 10: a project with one CAPTION track
// carrying one caption cue, mirroring makeProjectWithTextClip exactly minus
// the style.
Project makeProjectWithCaptionCue(ClipId clipId) {
    Project project;
    project.id = Uuid::generateV4();
    project.name = "test";
    project.timelineFps = FrameRate::fps24();
    Track track;
    track.id = Uuid::generateV4();
    track.kind = TrackKind::Caption;
    Clip clip;
    clip.id = clipId;
    clip.timelineStart = ms(0);
    clip.sourceIn = Duration::zero();
    clip.sourceOut = ms(1000);
    clip.captionText = "Hello";
    track.clips.push_back(std::move(clip));
    project.tracks.push_back(std::move(track));
    return project;
}

TEST(InspectorViewModel, ProjectionExposesTextStyleForATextClipAndNotForAMediaClip) {
    const ClipId textClip = Uuid::generateV4();
    TimelineEngine textEngine(makeProjectWithTextClip(textClip));
    InspectorViewModel textModel(textEngine);
    textModel.selectClip(textClip);

    const std::optional<ClipInspectorView> textView = textModel.selectedClip();
    ASSERT_TRUE(textView.has_value());
    ASSERT_TRUE(textView->textStyle.has_value());
    EXPECT_EQ(textView->textStyle->content, "Title");

    const ClipId mediaClip = Uuid::generateV4();
    TimelineEngine mediaEngine(makeProjectWithClip(mediaClip));
    InspectorViewModel mediaModel(mediaEngine);
    mediaModel.selectClip(mediaClip);

    const std::optional<ClipInspectorView> mediaView = mediaModel.selectedClip();
    ASSERT_TRUE(mediaView.has_value());
    EXPECT_FALSE(mediaView->textStyle.has_value());
}

TEST(InspectorViewModel, SetTextContentChangesTheStringAndUndoesExactly) {
    const ClipId clip = Uuid::generateV4();
    TimelineEngine engine(makeProjectWithTextClip(clip));
    InspectorViewModel model(engine);
    model.selectClip(clip);

    ASSERT_TRUE(model.setTextContent("Changed").changed());
    EXPECT_EQ(model.selectedClip()->textStyle->content, "Changed");

    ASSERT_TRUE(engine.undo().changed());
    EXPECT_EQ(model.selectedClip()->textStyle->content, "Title");
}

TEST(InspectorViewModel, SetTextStyleChangesOnlySuppliedFields) {
    const ClipId clip = Uuid::generateV4();
    TimelineEngine engine(makeProjectWithTextClip(clip));
    InspectorViewModel model(engine);
    model.selectClip(clip);

    const double before = model.selectedClip()->textStyle->pointSize;
    ASSERT_TRUE(model
                   .setTextStyle(std::nullopt, 40.0, std::nullopt, std::nullopt, std::nullopt,
                                std::nullopt, TextAlignment::Right, std::nullopt, std::nullopt)
                   .changed());

    const std::optional<ClipInspectorView> view = model.selectedClip();
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->textStyle->pointSize, 40.0);
    EXPECT_NE(view->textStyle->pointSize, before);
    EXPECT_EQ(view->textStyle->alignment, TextAlignment::Right);
    EXPECT_EQ(view->textStyle->fontFamily, "sans-serif");  // untouched, default carried through
}

TEST(InspectorViewModel, SetTextContentAndSetTextStyleRejectAMediaClipSelection) {
    const ClipId clip = Uuid::generateV4();
    TimelineEngine engine(makeProjectWithClip(clip));
    InspectorViewModel model(engine);
    model.selectClip(clip);

    EXPECT_FALSE(model.setTextContent("Anything").changed());
    EXPECT_FALSE(model
                     .setTextStyle(std::nullopt, 40.0, std::nullopt, std::nullopt, std::nullopt,
                                  std::nullopt, std::nullopt, std::nullopt, std::nullopt)
                     .changed());
}

// --- Captions and transcription (usable-editor task 13; Requirement 10) ----

TEST(InspectorViewModel, ProjectionExposesCaptionTextForACaptionCueAndNotForAMediaClip) {
    const ClipId captionCue = Uuid::generateV4();
    TimelineEngine captionEngine(makeProjectWithCaptionCue(captionCue));
    InspectorViewModel captionModel(captionEngine);
    captionModel.selectClip(captionCue);

    const std::optional<ClipInspectorView> captionView = captionModel.selectedClip();
    ASSERT_TRUE(captionView.has_value());
    ASSERT_TRUE(captionView->captionText.has_value());
    EXPECT_EQ(*captionView->captionText, "Hello");

    const ClipId mediaClip = Uuid::generateV4();
    TimelineEngine mediaEngine(makeProjectWithClip(mediaClip));
    InspectorViewModel mediaModel(mediaEngine);
    mediaModel.selectClip(mediaClip);

    const std::optional<ClipInspectorView> mediaView = mediaModel.selectedClip();
    ASSERT_TRUE(mediaView.has_value());
    EXPECT_FALSE(mediaView->captionText.has_value());
}

TEST(InspectorViewModel, SetCaptionTextChangesTheStringAndUndoesExactly) {
    const ClipId clip = Uuid::generateV4();
    TimelineEngine engine(makeProjectWithCaptionCue(clip));
    InspectorViewModel model(engine);
    model.selectClip(clip);

    ASSERT_TRUE(model.setCaptionText("Changed").changed());
    EXPECT_EQ(*model.selectedClip()->captionText, "Changed");

    ASSERT_TRUE(engine.undo().changed());
    EXPECT_EQ(*model.selectedClip()->captionText, "Hello");
}

TEST(InspectorViewModel, RetimeCaptionCueChangesOnlySuppliedField) {
    const ClipId clip = Uuid::generateV4();
    TimelineEngine engine(makeProjectWithCaptionCue(clip));
    InspectorViewModel model(engine);
    model.selectClip(clip);

    ASSERT_TRUE(model.retimeCaptionCue(ms(5000), std::nullopt).changed());

    const std::optional<ClipInspectorView> view = model.selectedClip();
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->timelineStart, ms(5000));
    EXPECT_EQ(view->duration, ms(1000));  // untouched
}

TEST(InspectorViewModel, SetCaptionTextAndRetimeCaptionCueRejectAMediaClipSelection) {
    const ClipId clip = Uuid::generateV4();
    TimelineEngine engine(makeProjectWithClip(clip));
    InspectorViewModel model(engine);
    model.selectClip(clip);

    EXPECT_FALSE(model.setCaptionText("Anything").changed());
    EXPECT_FALSE(model.retimeCaptionCue(ms(5000), std::nullopt).changed());
}

// --- Change notification ---------------------------------------------------

TEST(InspectorViewModel, OnChangedFiresForSelectionEditAndExternalChange) {
    const ClipId clip = Uuid::generateV4();
    TimelineEngine engine(makeProjectWithClip(clip));
    InspectorViewModel model(engine);

    int notifications = 0;
    model.setOnChanged([&] { ++notifications; });

    model.selectClip(clip);          // selection change -> 1
    EXPECT_EQ(notifications, 1);

    ASSERT_TRUE(model.setOpacity(0.5).changed());  // model's own edit -> engine emit -> 2
    EXPECT_EQ(notifications, 2);

    // An edit from another surface (here: a direct engine command) also refreshes.
    ASSERT_TRUE(engine.apply(std::make_unique<AddEffectCommand>(clip, Effect::blur(1.0)))
                    .changed());
    EXPECT_EQ(notifications, 3);

    // Selecting the same clip again is a no-op (no extra notification).
    model.selectClip(clip);
    EXPECT_EQ(notifications, 3);
}

}  // namespace
}  // namespace ui
}  // namespace palmier
