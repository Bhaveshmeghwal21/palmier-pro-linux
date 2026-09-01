// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/ScopesViewModel.hpp — the Qt-free logic behind the scopes panel
// (monitoring-and-grading task 6; Requirement 6.3, 6.5, 6.6, 6.7).
//
// Three rules the panel obeys, none of which needs Qt and all of which are easy to get
// wrong in a way that is invisible on screen:
//
//   * CADENCE (6.5). The scopes must update at least 10 times a second during playback
//     and must not cost the Preview more than 10 percent of its presented frame rate.
//     Those pull in opposite directions, so shouldRecompute() is a pure function of the
//     last computation's instant, the current instant and the measured cost, and it
//     REFUSES when the budget has been spent. A panel that simply recomputed on every
//     presented frame would meet the first requirement and silently violate the second.
//
//   * EMPTY STATE (6.6). No project, or a playhead past the end, means no frame — which
//     must read as an explicit empty state rather than the last reading. The view model
//     therefore CLEARS on an absent frame instead of keeping what it had.
//
//   * VISIBILITY (6.7). Each of the three scopes is individually hideable and its
//     visibility survives a restart, so the flags and their persistence keys live here
//     rather than in a widget, and a hidden scope is not computed at all.
//
// Time is always an ARGUMENT, never read from a clock, so the cadence rules are tested
// with simulated time and no sleeping — the same discipline ui::AudioMeterViewModel uses.

#ifndef PALMIER_UI_SCOPESVIEWMODEL_HPP
#define PALMIER_UI_SCOPESVIEWMODEL_HPP

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "gpu/VideoScopes.hpp"

namespace palmier::ui {

/// The three scopes, in the order they are presented.
enum class ScopeKind { Histogram, Waveform, Vectorscope };

inline constexpr std::array<ScopeKind, 3> kScopeKinds{ScopeKind::Histogram, ScopeKind::Waveform,
                                                     ScopeKind::Vectorscope};

/// Display name, and the settings key its visibility persists under.
[[nodiscard]] const char* scopeKindName(ScopeKind kind) noexcept;
[[nodiscard]] std::string scopeVisibilitySettingsKey(ScopeKind kind);

/// The measured cost of one scope computation and the Preview's own frame interval, which
/// together decide whether another computation fits inside the 10 percent budget.
struct ScopeBudget {
    /// How long the last full recomputation took.
    std::chrono::microseconds lastCost{0};
    /// The Preview's presented frame interval. Zero means "not playing", in which case
    /// there is no frame rate to protect and the cadence floor is all that applies.
    std::chrono::microseconds previewFrameInterval{0};

    /// The share of the Preview's frame interval the scopes may consume: Requirement 6.5's
    /// ten percent, as a fraction rather than a percentage so the arithmetic is exact.
    static constexpr double kMaxPreviewShare = 0.10;

    /// Whether one more computation of the last measured cost fits in the budget.
    [[nodiscard]] bool fits() const noexcept;
};

/// The scopes of the frame currently shown, with their cadence and visibility rules.
class ScopesViewModel {
public:
    /// Requirement 6.5's floor: at least 10 updates per second, so at most 100 ms apart.
    static constexpr std::chrono::milliseconds kMaxUpdateInterval{100};
    /// How many traces a waveform is bucketed into. A display-side choice, not a
    /// measurement one, because the computation sums whatever it is given.
    static constexpr int kWaveformColumns = 256;

    // --- Visibility (6.7) --------------------------------------------------

    [[nodiscard]] bool isVisible(ScopeKind kind) const noexcept;
    void setVisible(ScopeKind kind, bool visible) noexcept;
    /// Whether any scope is shown at all. When none is, nothing is computed: the cheapest
    /// way to honour the Preview's budget is not to spend it.
    [[nodiscard]] bool anyVisible() const noexcept;

    // --- Cadence (6.5) -----------------------------------------------------

    /// Whether to recompute now.
    ///
    /// True when a scope is visible AND either nothing has been computed yet, or the
    /// cadence floor has been reached, or enough time has passed that the last measured
    /// cost fits inside the Preview's budget. False when the budget is spent, which is
    /// the case that keeps the Preview smooth.
    [[nodiscard]] bool shouldRecompute(std::chrono::steady_clock::time_point now,
                                      const ScopeBudget& budget) const noexcept;

    // --- Frame intake (6.2, 6.6) -------------------------------------------

    /// Recompute from the compositor's output buffer, and record when and how long.
    ///
    /// `rgba` null or a zero size is the empty state, and it CLEARS rather than keeping
    /// the previous reading: Requirement 6.6 forbids a stale display, and a scope showing
    /// the previous shot's exposure is worse than one showing nothing, because it looks
    /// authoritative.
    void update(const std::uint8_t* rgba, int width, int height,
                std::chrono::steady_clock::time_point now, std::chrono::microseconds cost);

    /// Drop the current reading, for a closed project or a playhead past the end.
    void clear() noexcept;

    /// Overwrite the recorded cost of the last computation.
    ///
    /// Exists because a caller cannot know what a computation cost until after it has run,
    /// and calling update() twice to find out would compute every scope twice and so spend
    /// double the budget the cost is meant to protect. update() takes a cost so a test can
    /// state one exactly; the shell states the previous one and corrects it with this.
    void recordCost(std::chrono::microseconds cost) noexcept { lastCost_ = cost; }

    [[nodiscard]] bool hasFrame() const noexcept { return hasFrame_; }
    [[nodiscard]] const gpu::Histogram& histogram() const noexcept { return histogram_; }
    [[nodiscard]] const gpu::LumaWaveform& waveform() const noexcept { return waveform_; }
    [[nodiscard]] const gpu::Vectorscope& vectorscope() const noexcept { return vectorscope_; }

    /// When the last recomputation happened, or nullopt if none has.
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> lastUpdate() const noexcept {
        return lastUpdate_;
    }
    /// How long the last recomputation took, for the caller to feed back as the budget.
    [[nodiscard]] std::chrono::microseconds lastCost() const noexcept { return lastCost_; }
    /// Computations performed, for asserting that a hidden scope costs nothing.
    [[nodiscard]] std::uint64_t computeCount() const noexcept { return computeCount_; }

private:
    std::array<bool, 3> visible_{true, true, true};
    bool                hasFrame_ = false;
    gpu::Histogram      histogram_;
    gpu::LumaWaveform   waveform_;
    gpu::Vectorscope    vectorscope_;

    std::optional<std::chrono::steady_clock::time_point> lastUpdate_;
    std::chrono::microseconds lastCost_{0};
    std::uint64_t             computeCount_ = 0;
};

}  // namespace palmier::ui

#endif  // PALMIER_UI_SCOPESVIEWMODEL_HPP
