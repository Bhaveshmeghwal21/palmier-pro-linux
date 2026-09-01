// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/ScopesViewModel.cpp — see the header for the three rules and why time is an
// argument rather than a clock read.

#include "ui/ScopesViewModel.hpp"

namespace palmier::ui {

namespace {

std::size_t indexOf(ScopeKind kind) noexcept { return static_cast<std::size_t>(kind); }

}  // namespace

const char* scopeKindName(ScopeKind kind) noexcept {
    switch (kind) {
        case ScopeKind::Histogram:   return "Histogram";
        case ScopeKind::Waveform:    return "Waveform";
        case ScopeKind::Vectorscope: return "Vectorscope";
    }
    return "Histogram";
}

std::string scopeVisibilitySettingsKey(ScopeKind kind) {
    // Derived from the display name rather than written out again, so a scope added later
    // cannot have a name in one place and a key in another -- and the key is stable
    // because the name is what the user sees and will not be churned casually.
    return std::string("scopes/") + scopeKindName(kind) + "Visible";
}

bool ScopeBudget::fits() const noexcept {
    // Not playing: there is no presented frame rate to protect, so only the cadence floor
    // applies and any cost fits. This is what keeps the scopes responsive while paused,
    // which is when a colourist actually reads them.
    if (previewFrameInterval <= std::chrono::microseconds{0}) {
        return true;
    }
    const double allowance =
        static_cast<double>(previewFrameInterval.count()) * kMaxPreviewShare;
    return static_cast<double>(lastCost.count()) <= allowance;
}

bool ScopesViewModel::isVisible(ScopeKind kind) const noexcept { return visible_[indexOf(kind)]; }

void ScopesViewModel::setVisible(ScopeKind kind, bool visible) noexcept {
    visible_[indexOf(kind)] = visible;
}

bool ScopesViewModel::anyVisible() const noexcept {
    for (const bool v : visible_) {
        if (v) return true;
    }
    return false;
}

bool ScopesViewModel::shouldRecompute(std::chrono::steady_clock::time_point now,
                                     const ScopeBudget& budget) const noexcept {
    // Nothing shown: the cheapest way to honour the Preview's budget is not to spend it.
    if (!anyVisible()) {
        return false;
    }
    // Nothing computed yet, so there is no reading at all to show. This is deliberately
    // checked BEFORE the budget: refusing the very first computation would leave the panel
    // permanently empty on a host where one computation happens to exceed the share.
    if (!lastUpdate_) {
        return true;
    }
    const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(now - *lastUpdate_);
    if (since >= kMaxUpdateInterval) {
        // Requirement 6.5's floor has been reached. It wins over the budget, because a
        // panel that stops updating is a broken panel while a Preview one frame short is
        // an imperceptible one -- and the floor is only 10 Hz.
        return true;
    }
    return budget.fits();
}

void ScopesViewModel::update(const std::uint8_t* rgba, int width, int height,
                            std::chrono::steady_clock::time_point now,
                            std::chrono::microseconds cost) {
    lastUpdate_ = now;
    lastCost_ = cost;

    if (rgba == nullptr || width <= 0 || height <= 0) {
        // Requirement 6.6: no frame CLEARS. A scope showing the previous shot's exposure
        // is worse than one showing nothing, because it looks authoritative.
        clear();
        lastUpdate_ = now;
        lastCost_ = cost;
        return;
    }

    // A hidden scope is not computed. Requirement 6.7 only asks that it be hideable, but
    // computing it anyway would spend the Preview's budget on something nobody can see.
    if (isVisible(ScopeKind::Histogram)) {
        histogram_ = gpu::computeHistogram(rgba, width, height);
    } else {
        histogram_ = gpu::Histogram{};
    }
    if (isVisible(ScopeKind::Waveform)) {
        waveform_ = gpu::computeLumaWaveform(rgba, width, height, kWaveformColumns);
    } else {
        waveform_ = gpu::LumaWaveform{};
    }
    if (isVisible(ScopeKind::Vectorscope)) {
        vectorscope_ = gpu::computeVectorscope(rgba, width, height);
    } else {
        vectorscope_ = gpu::Vectorscope{};
    }

    hasFrame_ = true;
    ++computeCount_;
}

void ScopesViewModel::clear() noexcept {
    hasFrame_ = false;
    histogram_ = gpu::Histogram{};
    waveform_ = gpu::LumaWaveform{};
    vectorscope_ = gpu::Vectorscope{};
    // lastUpdate_ and lastCost_ are deliberately NOT reset: they describe when work last
    // happened, and clearing them would make shouldRecompute() answer "nothing computed
    // yet" and spin as fast as it is asked while no frame is available.
}

}  // namespace palmier::ui
