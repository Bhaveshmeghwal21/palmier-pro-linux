// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/ui/curve_editor_viewmodel_test.cpp — direct manipulation of a tone curve
// (monitoring-and-grading task 5; Requirement 5.9).
//
// Every rule the Inspector's curve control obeys lives in ui::CurveEditorViewModel, so
// all of it is tested here with no display, no Qt and no GPU. What is NOT tested here is
// painting, which is the widget's only remaining job.
//
// The cases worth reading are the ones about what must NOT happen: a click that grabs a
// point is not an edit, a press with no movement is not an edit, and a move arriving
// without a press is not an edit. Each of those, if wrong, produces undo history entries
// that undo to the state they started from — which a user reads as broken undo rather
// than as a spurious edit.

#include "ui/CurveEditorViewModel.hpp"

#include <cmath>
#include <map>
#include <vector>

#include <gtest/gtest.h>

#include "core/ToneCurve.hpp"

namespace palmier::ui {
namespace {

// A widget 200 x 100, so a curve x of 0.5 is pixel x 100 and a curve y of 0.5 is pixel
// y 50. Deliberately non-square: a mapping that used one dimension for both axes would
// pass on a square widget and fail here.
constexpr double kW = 200.0;
constexpr double kH = 100.0;

// --- Coordinate mapping ----------------------------------------------------

TEST(CurveEditorViewModel, TheVerticalAxisIsFlippedSoFullOutputIsAtTheTop) {
    CurveEditorViewModel vm;
    const CurvePixel top = vm.toPixel(CurvePoint{0.0, 1.0}, kW, kH);
    EXPECT_DOUBLE_EQ(top.x, 0.0);
    EXPECT_DOUBLE_EQ(top.y, 0.0) << "output 1.0 belongs at the TOP, which is y=0";

    const CurvePixel bottom = vm.toPixel(CurvePoint{1.0, 0.0}, kW, kH);
    EXPECT_DOUBLE_EQ(bottom.x, kW);
    EXPECT_DOUBLE_EQ(bottom.y, kH);
}

TEST(CurveEditorViewModel, PixelAndCurveCoordinatesRoundTrip) {
    CurveEditorViewModel vm;
    for (const CurvePoint original : {CurvePoint{0.0, 0.0}, CurvePoint{0.25, 0.75},
                                      CurvePoint{0.5, 0.5}, CurvePoint{1.0, 1.0}}) {
        const CurvePoint back = vm.toCurve(vm.toPixel(original, kW, kH), kW, kH);
        EXPECT_NEAR(back.x, original.x, 1e-12);
        EXPECT_NEAR(back.y, original.y, 1e-12);
    }
}

TEST(CurveEditorViewModel, ADragOutsideTheWidgetPinsToTheEdgeRatherThanLeavingTheRange) {
    CurveEditorViewModel vm;
    const CurvePoint low = vm.toCurve(CurvePixel{-50.0, 500.0}, kW, kH);
    EXPECT_DOUBLE_EQ(low.x, 0.0);
    EXPECT_DOUBLE_EQ(low.y, 0.0);

    const CurvePoint high = vm.toCurve(CurvePixel{kW + 50.0, -20.0}, kW, kH);
    EXPECT_DOUBLE_EQ(high.x, 1.0);
    EXPECT_DOUBLE_EQ(high.y, 1.0);
}

// A widget mid-layout can be zero-sized. Dividing by it would produce NaN coordinates
// that would then be written into the project as a control point, and NaN in a curve is
// not a visible glitch -- it silently poisons the baked table.
TEST(CurveEditorViewModel, AZeroSizedWidgetProducesNoNaNCoordinates) {
    CurveEditorViewModel vm;
    const CurvePoint p = vm.toCurve(CurvePixel{10.0, 10.0}, 0.0, 0.0);
    EXPECT_FALSE(std::isnan(p.x));
    EXPECT_FALSE(std::isnan(p.y));
    EXPECT_DOUBLE_EQ(p.x, 0.0);
    EXPECT_DOUBLE_EQ(p.y, 0.0);
}

// --- Hit testing -----------------------------------------------------------

TEST(CurveEditorViewModel, APointIsGrabbableWithinTheRadiusAndNotBeyondIt) {
    CurveEditorViewModel vm;
    vm.setPoints(std::vector<CurvePoint>{{0.5, 0.5}});
    const CurvePixel at = vm.toPixel(CurvePoint{0.5, 0.5}, kW, kH);

    EXPECT_TRUE(vm.pointAt(at, kW, kH).has_value());
    const double just = CurveEditorViewModel::kGrabRadiusPx - 1.0;
    EXPECT_TRUE(vm.pointAt(CurvePixel{at.x + just, at.y}, kW, kH).has_value());
    const double beyond = CurveEditorViewModel::kGrabRadiusPx + 2.0;
    EXPECT_FALSE(vm.pointAt(CurvePixel{at.x + beyond, at.y}, kW, kH).has_value());
}

// With two points within the radius the NEAREST must win. If the first match won
// instead, the earlier-added point would always capture the pointer and the other would
// become permanently unreachable -- and dragging would silently move the wrong one.
TEST(CurveEditorViewModel, TheNearestPointWinsRatherThanTheFirstOneAdded) {
    CurveEditorViewModel vm;
    vm.setPoints(std::vector<CurvePoint>{{0.50, 0.5}, {0.52, 0.5}});
    const CurvePixel nearSecond = vm.toPixel(CurvePoint{0.52, 0.5}, kW, kH);

    const auto hit = vm.pointAt(nearSecond, kW, kH);
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(*hit, 1u) << "the pointer is on point 1, so point 1 must be grabbed";
}

// --- Gestures --------------------------------------------------------------

TEST(CurveEditorViewModel, PressingEmptySpaceAddsAPointThereAndBeginsDraggingIt) {
    CurveEditorViewModel vm;
    const CurveEditRequest request = vm.press(CurvePixel{100.0, 50.0}, kW, kH);

    ASSERT_EQ(request.kind, CurveEditRequest::Kind::Add);
    EXPECT_EQ(request.operation(), EditCurvePointCommand::Operation::Add);
    EXPECT_DOUBLE_EQ(request.point.x, 0.5);
    EXPECT_DOUBLE_EQ(request.point.y, 0.5);
    ASSERT_EQ(vm.points().size(), 1u) << "the working copy gains it at once so it can be dragged";
    EXPECT_TRUE(vm.isDragging());
}

// Grabbing a point is not an edit. Reporting a Move here would put an undo entry on
// every click, including a click that changes nothing at all.
TEST(CurveEditorViewModel, PressingAnExistingPointGrabsItWithoutAskingForAnEdit) {
    CurveEditorViewModel vm;
    vm.setPoints(std::vector<CurvePoint>{{0.5, 0.5}});
    const CurvePixel at = vm.toPixel(CurvePoint{0.5, 0.5}, kW, kH);

    const CurveEditRequest request = vm.press(at, kW, kH);
    EXPECT_TRUE(request.isNoOp());
    EXPECT_TRUE(vm.isDragging());
    EXPECT_EQ(vm.draggedIndex().value(), 0u);
    EXPECT_EQ(vm.points().size(), 1u) << "grabbing must not add a second point on top";
}

TEST(CurveEditorViewModel, DraggingMovesTheGrabbedPointAndTheReleaseCommitsTheFinalPosition) {
    CurveEditorViewModel vm;
    vm.setPoints(std::vector<CurvePoint>{{0.25, 0.25}});
    ASSERT_TRUE(vm.press(vm.toPixel(CurvePoint{0.25, 0.25}, kW, kH), kW, kH).isNoOp());

    const CurveEditRequest mid = vm.drag(CurvePixel{100.0, 50.0}, kW, kH);
    ASSERT_EQ(mid.kind, CurveEditRequest::Kind::Move);
    EXPECT_EQ(mid.index, 0u);
    EXPECT_DOUBLE_EQ(vm.points()[0].x, 0.5) << "the working copy follows the pointer live";

    const CurveEditRequest last = vm.drag(CurvePixel{150.0, 25.0}, kW, kH);
    EXPECT_DOUBLE_EQ(last.point.x, 0.75);

    // The release reports the FINAL position, which is what the caller commits -- one
    // undo entry for the whole drag rather than one per mouse move.
    const CurveEditRequest committed = vm.release();
    ASSERT_EQ(committed.kind, CurveEditRequest::Kind::Move);
    EXPECT_EQ(committed.index, 0u);
    EXPECT_DOUBLE_EQ(committed.point.x, 0.75);
    EXPECT_DOUBLE_EQ(committed.point.y, 0.75);
    EXPECT_FALSE(vm.isDragging());
}

TEST(CurveEditorViewModel, APressAndReleaseWithNoMovementCommitsNothing) {
    CurveEditorViewModel vm;
    vm.setPoints(std::vector<CurvePoint>{{0.5, 0.5}});
    ASSERT_TRUE(vm.press(vm.toPixel(CurvePoint{0.5, 0.5}, kW, kH), kW, kH).isNoOp());

    EXPECT_TRUE(vm.release().isNoOp())
        << "an undo entry that undoes to the same state reads as broken undo";
    EXPECT_FALSE(vm.isDragging());
}

TEST(CurveEditorViewModel, AMoveOrReleaseWithoutAPressIsNotAnEdit) {
    CurveEditorViewModel vm;
    vm.setPoints(std::vector<CurvePoint>{{0.5, 0.5}});

    EXPECT_TRUE(vm.drag(CurvePixel{10.0, 10.0}, kW, kH).isNoOp());
    EXPECT_TRUE(vm.release().isNoOp());
    EXPECT_DOUBLE_EQ(vm.points()[0].x, 0.5) << "and it must not have moved the point";
}

TEST(CurveEditorViewModel, RemovingReportsTheIndexUnderThePointerAndNothingWhenThereIsNone) {
    CurveEditorViewModel vm;
    vm.setPoints(std::vector<CurvePoint>{{0.2, 0.2}, {0.8, 0.8}});

    EXPECT_TRUE(vm.remove(CurvePixel{100.0, 50.0}, kW, kH).isNoOp())
        << "empty space is not a removal";

    const CurveEditRequest request = vm.remove(vm.toPixel(CurvePoint{0.2, 0.2}, kW, kH), kW, kH);
    ASSERT_EQ(request.kind, CurveEditRequest::Kind::Remove);
    EXPECT_EQ(request.index, 0u);
    EXPECT_EQ(request.operation(), EditCurvePointCommand::Operation::Remove);
    EXPECT_EQ(vm.points().size(), 1u);
    EXPECT_DOUBLE_EQ(vm.points()[0].x, 0.8) << "the other point survives, renumbered to 0";
}

// Requirement 5.5: an empty curve is the identity, not an invalid state. So there is
// nothing to protect the user from, and refusing would leave them unable to clear a
// curve they no longer want.
TEST(CurveEditorViewModel, TheLastPointCanBeRemovedBecauseAnEmptyCurveIsValid) {
    CurveEditorViewModel vm;
    vm.setPoints(std::vector<CurvePoint>{{0.5, 0.9}});
    const CurveEditRequest request = vm.remove(vm.toPixel(CurvePoint{0.5, 0.9}, kW, kH), kW, kH);
    EXPECT_EQ(request.kind, CurveEditRequest::Kind::Remove);
    EXPECT_TRUE(vm.points().empty());
}

// An undo, or an edit arriving over MCP, resynchronises the working copy. Any gesture in
// progress must be abandoned: the index being dragged may name a different point now, or
// none, and continuing would move whichever point holds that index -- a wrong edit that
// looks like a successful one.
TEST(CurveEditorViewModel, AnExternalChangeAbandonsAGestureInProgress) {
    CurveEditorViewModel vm;
    vm.setPoints(std::vector<CurvePoint>{{0.5, 0.5}});
    ASSERT_TRUE(vm.press(vm.toPixel(CurvePoint{0.5, 0.5}, kW, kH), kW, kH).isNoOp());
    ASSERT_TRUE(vm.isDragging());

    vm.setPoints(std::vector<CurvePoint>{{0.1, 0.1}, {0.9, 0.9}});
    EXPECT_FALSE(vm.isDragging());
    EXPECT_TRUE(vm.drag(CurvePixel{10.0, 10.0}, kW, kH).isNoOp());
    EXPECT_DOUBLE_EQ(vm.points()[0].x, 0.1) << "the resynchronised points are untouched";
}

// --- Reading and painting --------------------------------------------------

TEST(CurveEditorViewModel, PointsAreReadFromTheEffectsParametersForTheChosenChannelOnly) {
    std::map<std::string, double> parameters;
    parameters[curvePointParameterName(CurveChannel::Red, 0, /*isY=*/false)] = 0.3;
    parameters[curvePointParameterName(CurveChannel::Red, 0, /*isY=*/true)] = 0.7;
    parameters[curvePointParameterName(CurveChannel::Blue, 0, /*isY=*/false)] = 0.1;
    parameters[curvePointParameterName(CurveChannel::Blue, 0, /*isY=*/true)] = 0.2;

    CurveEditorViewModel vm;
    vm.setChannel(CurveChannel::Red);
    vm.setPoints(parameters);
    ASSERT_EQ(vm.points().size(), 1u);
    EXPECT_DOUBLE_EQ(vm.points()[0].y, 0.7);

    vm.setChannel(CurveChannel::Blue);
    vm.setPoints(parameters);
    ASSERT_EQ(vm.points().size(), 1u);
    EXPECT_DOUBLE_EQ(vm.points()[0].y, 0.2);
}

// The curve DRAWN must be the curve APPLIED. A separate plotting routine here would be a
// second implementation of the interpolation, free to disagree with the image, so the
// table painted is the renderer's own.
TEST(CurveEditorViewModel, TheTablePaintedIsExactlyTheOneTheRendererWouldApply) {
    const std::vector<CurvePoint> points{{0.1, 0.0}, {0.6, 0.95}};
    CurveEditorViewModel vm;
    vm.setPoints(points);
    EXPECT_EQ(vm.transferTable(), bakeToneCurve(points));
}

TEST(CurveEditorViewModel, AnEmptyCurvePaintsTheIdentity) {
    CurveEditorViewModel vm;
    EXPECT_EQ(vm.transferTable(), identityToneCurveTable());
}

}  // namespace
}  // namespace palmier::ui
