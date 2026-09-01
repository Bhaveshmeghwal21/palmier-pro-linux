// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/ui/scopes_viewmodel_test.cpp — the scopes panel's cadence, empty state and
// visibility rules (monitoring-and-grading task 6; Requirement 6.3, 6.5, 6.6, 6.7).
//
// Time is an argument to every call, so the two real-time rules -- at least 10 updates a
// second, and no more than 10 percent of the Preview's frame interval -- are tested with
// simulated instants and no sleeping. A test that slept would be slow AND would assert
// the runner's scheduler rather than the rule.

#include "ui/ScopesViewModel.hpp"

#include <chrono>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

namespace palmier::ui {
namespace {

using namespace std::chrono_literals;

std::vector<std::uint8_t> solidFrame(int width, int height, std::uint8_t v) {
    std::vector<std::uint8_t> px(static_cast<std::size_t>(width) *
                                 static_cast<std::size_t>(height) * 4u);
    for (std::size_t i = 0; i < px.size(); i += 4) {
        px[i + 0] = px[i + 1] = px[i + 2] = v;
        px[i + 3] = 255;
    }
    return px;
}

constexpr std::chrono::steady_clock::time_point kT0{};

// --- Visibility and its persistence key (6.7) ------------------------------

TEST(ScopesViewModel, AllThreeScopesStartVisibleAndEachTogglesIndependently) {
    ScopesViewModel vm;
    for (const ScopeKind kind : kScopeKinds) {
        EXPECT_TRUE(vm.isVisible(kind)) << scopeKindName(kind);
    }

    vm.setVisible(ScopeKind::Waveform, false);
    EXPECT_FALSE(vm.isVisible(ScopeKind::Waveform));
    EXPECT_TRUE(vm.isVisible(ScopeKind::Histogram)) << "hiding one must not hide another";
    EXPECT_TRUE(vm.isVisible(ScopeKind::Vectorscope));
    EXPECT_TRUE(vm.anyVisible());
}

TEST(ScopesViewModel, EveryScopeHasADistinctStableSettingsKey) {
    // The keys are what carry visibility across a restart, so two scopes sharing one would
    // make hiding either hide both after a relaunch -- a bug that only appears on the
    // second run.
    std::vector<std::string> keys;
    for (const ScopeKind kind : kScopeKinds) {
        keys.push_back(scopeVisibilitySettingsKey(kind));
    }
    EXPECT_EQ(keys.size(), 3u);
    EXPECT_NE(keys[0], keys[1]);
    EXPECT_NE(keys[1], keys[2]);
    EXPECT_NE(keys[0], keys[2]);
    for (const std::string& key : keys) {
        EXPECT_EQ(key.rfind("scopes/", 0), 0u) << key << " must be namespaced";
    }
}

// --- Cadence (6.5) ---------------------------------------------------------

TEST(ScopesViewModel, TheFirstComputationIsAlwaysAllowedEvenOnATightBudget) {
    ScopesViewModel vm;
    // A cost far larger than the whole frame interval: the budget cannot possibly fit.
    const ScopeBudget impossible{.lastCost = 100ms, .previewFrameInterval = 16667us};
    EXPECT_TRUE(vm.shouldRecompute(kT0, impossible))
        << "refusing the first would leave the panel permanently empty on a slow host";
}

TEST(ScopesViewModel, TheTenHzFloorIsHonouredEvenWhenTheBudgetIsSpent) {
    ScopesViewModel vm;
    const auto frame = solidFrame(8, 8, 128);
    vm.update(frame.data(), 8, 8, kT0, 50ms);  // absurdly expensive

    const ScopeBudget spent{.lastCost = 50ms, .previewFrameInterval = 16667us};
    EXPECT_FALSE(spent.fits()) << "the premise: 50ms does not fit in 10% of 16.667ms";
    EXPECT_FALSE(vm.shouldRecompute(kT0 + 50ms, spent)) << "and before the floor it waits";

    // A panel that stops updating is a broken panel; a Preview one frame short is an
    // imperceptible one. The floor is only 10 Hz, so it wins.
    EXPECT_TRUE(vm.shouldRecompute(kT0 + ScopesViewModel::kMaxUpdateInterval, spent));
    EXPECT_TRUE(vm.shouldRecompute(kT0 + 500ms, spent));
}

TEST(ScopesViewModel, AnAffordableCostRecomputesImmediatelyRatherThanWaitingForTheFloor) {
    ScopesViewModel vm;
    const auto frame = solidFrame(8, 8, 128);
    vm.update(frame.data(), 8, 8, kT0, 200us);

    // 200us against 10% of a 60fps frame (1666us) fits comfortably.
    const ScopeBudget cheap{.lastCost = 200us, .previewFrameInterval = 16667us};
    EXPECT_TRUE(cheap.fits());
    EXPECT_TRUE(vm.shouldRecompute(kT0 + 1ms, cheap))
        << "the floor is a MINIMUM rate, not a maximum";
}

TEST(ScopesViewModel, TheBudgetIsExactlyTenPercentOfThePreviewsFrameInterval) {
    // 10% of 16667us is 1666.7us, so 1666 fits and 1667 does not. Asserted on both sides,
    // because a budget that was actually 100% or 1% would pass a one-sided test.
    EXPECT_TRUE((ScopeBudget{.lastCost = 1666us, .previewFrameInterval = 16667us}).fits());
    EXPECT_FALSE((ScopeBudget{.lastCost = 1667us, .previewFrameInterval = 16667us}).fits());
    EXPECT_DOUBLE_EQ(ScopeBudget::kMaxPreviewShare, 0.10);
}

// Paused is when a colourist actually reads a scope, and there is no presented frame rate
// to protect then -- so the budget must not throttle it.
TEST(ScopesViewModel, WithNoPreviewFrameRateAnyCostFitsBecauseThereIsNothingToProtect) {
    ScopesViewModel vm;
    const auto frame = solidFrame(8, 8, 200);
    vm.update(frame.data(), 8, 8, kT0, 80ms);

    const ScopeBudget paused{.lastCost = 80ms, .previewFrameInterval = 0us};
    EXPECT_TRUE(paused.fits());
    EXPECT_TRUE(vm.shouldRecompute(kT0 + 1ms, paused));
}

TEST(ScopesViewModel, NothingIsRecomputedWhenEveryScopeIsHidden) {
    ScopesViewModel vm;
    for (const ScopeKind kind : kScopeKinds) {
        vm.setVisible(kind, false);
    }
    EXPECT_FALSE(vm.anyVisible());
    // Even the first computation, and even on an unlimited budget: the cheapest way to
    // honour the Preview's budget is not to spend it.
    EXPECT_FALSE(vm.shouldRecompute(kT0, ScopeBudget{}));
    EXPECT_FALSE(vm.shouldRecompute(kT0 + 10s, ScopeBudget{}));
}

// --- Frame intake and the empty state (6.6) --------------------------------

TEST(ScopesViewModel, AFrameProducesAllThreeReadingsAndTheyAgreeWithTheDirectComputation) {
    ScopesViewModel vm;
    const auto frame = solidFrame(16, 4, 255);
    vm.update(frame.data(), 16, 4, kT0, 100us);

    ASSERT_TRUE(vm.hasFrame());
    EXPECT_EQ(vm.histogram().luma[255], 64u);
    EXPECT_EQ(vm.waveform().maxLuma(), 255);
    EXPECT_EQ(vm.vectorscope().neutralCount(), 64u);
    // Exactly what gpu::computeHistogram would answer: the panel adds cadence and
    // visibility, never arithmetic of its own.
    EXPECT_EQ(vm.histogram().luma, gpu::computeHistogram(frame.data(), 16, 4).luma);
    EXPECT_EQ(vm.computeCount(), 1u);
}

// The rule 6.6 exists for. A scope showing the previous shot's exposure is worse than one
// showing nothing, because it looks authoritative.
TEST(ScopesViewModel, AnAbsentFrameClearsEveryReadingRatherThanKeepingTheLastOne) {
    ScopesViewModel vm;
    const auto frame = solidFrame(8, 8, 255);
    vm.update(frame.data(), 8, 8, kT0, 100us);
    ASSERT_TRUE(vm.hasFrame());
    ASSERT_EQ(vm.histogram().luma[255], 64u);

    vm.update(nullptr, 8, 8, kT0 + 100ms, 0us);
    EXPECT_FALSE(vm.hasFrame());
    EXPECT_TRUE(vm.histogram().isEmpty());
    EXPECT_TRUE(vm.waveform().isEmpty());
    EXPECT_TRUE(vm.vectorscope().isEmpty());
    EXPECT_EQ(vm.histogram().luma[255], 0u) << "the previous reading must be gone, not stale";
}

TEST(ScopesViewModel, AZeroSizedFrameIsAlsoTheEmptyState) {
    ScopesViewModel vm;
    const auto frame = solidFrame(8, 8, 128);
    vm.update(frame.data(), 0, 8, kT0, 0us);
    EXPECT_FALSE(vm.hasFrame());
    vm.update(frame.data(), 8, 0, kT0, 0us);
    EXPECT_FALSE(vm.hasFrame());
}

TEST(ScopesViewModel, ABlackFrameIsAReadingAndNotTheEmptyState) {
    ScopesViewModel vm;
    const auto black = solidFrame(4, 4, 0);
    vm.update(black.data(), 4, 4, kT0, 100us);
    EXPECT_TRUE(vm.hasFrame()) << "black is a real exposure, and the panel must say so";
    EXPECT_FALSE(vm.histogram().isEmpty());
    EXPECT_EQ(vm.histogram().luma[0], 16u);
}

TEST(ScopesViewModel, AHiddenScopeIsNotComputedButTheVisibleOnesStillAre) {
    ScopesViewModel vm;
    vm.setVisible(ScopeKind::Vectorscope, false);
    const auto frame = solidFrame(8, 8, 200);
    vm.update(frame.data(), 8, 8, kT0, 100us);

    EXPECT_TRUE(vm.vectorscope().isEmpty()) << "a hidden scope must hold no reading at all";
    EXPECT_FALSE(vm.histogram().isEmpty());
    EXPECT_FALSE(vm.waveform().isEmpty());
    EXPECT_TRUE(vm.hasFrame()) << "the frame is still present; only one scope is not shown";
}

// clear() must NOT reset the cadence bookkeeping. If it did, shouldRecompute() would
// answer "nothing computed yet" and spin as fast as it is asked while no frame exists --
// spending the Preview's budget precisely when there is nothing to show.
TEST(ScopesViewModel, ClearingKeepsTheCadenceBookkeepingSoItDoesNotSpin) {
    ScopesViewModel vm;
    const auto frame = solidFrame(8, 8, 128);
    vm.update(frame.data(), 8, 8, kT0, 50ms);
    ASSERT_TRUE(vm.lastUpdate().has_value());

    vm.clear();
    EXPECT_FALSE(vm.hasFrame());
    ASSERT_TRUE(vm.lastUpdate().has_value()) << "when work last happened is still true";
    EXPECT_EQ(*vm.lastUpdate(), kT0);
    EXPECT_EQ(vm.lastCost(), 50ms);

    const ScopeBudget spent{.lastCost = 50ms, .previewFrameInterval = 16667us};
    EXPECT_FALSE(vm.shouldRecompute(kT0 + 10ms, spent));
}

TEST(ScopesViewModel, TheReportedCostIsWhatTheCallerMeasuredSoItCanBeFedBackAsTheBudget) {
    ScopesViewModel vm;
    const auto frame = solidFrame(8, 8, 128);
    vm.update(frame.data(), 8, 8, kT0, 1234us);
    EXPECT_EQ(vm.lastCost(), 1234us);

    // The round trip the shell performs: measure, report, then ask again with it. 1234us
    // is under 10% of a 60fps frame (1666.7us), so a second computation is affordable.
    const ScopeBudget measured{.lastCost = vm.lastCost(), .previewFrameInterval = 16667us};
    EXPECT_TRUE(measured.fits());
    EXPECT_TRUE(vm.shouldRecompute(kT0 + 5ms, measured));
}

}  // namespace
}  // namespace palmier::ui
